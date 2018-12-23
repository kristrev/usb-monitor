#include <json-c/json.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>

#include "usb_monitor.h"
#include "usb_logging.h"
#include "usb_helpers.h"
#include "lanner_handler.h"

static void lanner_print_port(struct usb_port *port)
{
    char buf[10] = {0};
    struct lanner_port *l_port = (struct lanner_port*) port;

    snprintf(buf, sizeof(buf), "(bit %u)", (uint8_t) ffs(l_port->bitmask));

    usb_helpers_print_port(port, "Lanner", buf);
}

static int32_t lanner_update_port(struct usb_port *port, uint8_t cmd)
{
    struct lanner_port *l_port = (struct lanner_port*) port;
    struct lanner_shared *l_shared = l_port->shared_info;

    //Order of operations here is:
    //* Return an error if shared is not IDLE/PENDING
    //* Set cmd of port to whatever is stored
    //* Update bitmask of shared
    if (l_shared->mcu_state != LANNER_MCU_IDLE &&
        l_shared->mcu_state != LANNER_MCU_PENDING) {
        //TODO: Update to return 503 instead
        return 1;
    }

    //We keep the current command in the port. The bitmask will be generated
    //based on the different cur_cmd values
    l_port->cur_cmd = cmd;

    //"Register" this port with the shared structure
    l_shared->mcu_ports_mask |= l_port->bitmask;

    //Ensure that state of l_shared is correct + that callback is added. Doing
    //these operations multiple times does no harm
    l_shared->mcu_state = LANNER_MCU_PENDING;
    usb_monitor_start_itr_cb(l_port->ctx);

    return 0;
}

static void lanner_handle_timeout(struct usb_port *port)
{
    if (port->msg_mode == PING) {
        usb_helpers_send_ping(port);
    } else {
        USB_DEBUG_PRINT_SYSLOG(port->ctx, LOG_INFO, "Timeout\n");
    }
}

static uint8_t lanner_handler_add_port(struct usb_monitor_ctx *ctx,
                                       char *dev_path, uint8_t bit,
                                       struct lanner_shared *l_shared)
{
    uint8_t dev_path_array[USB_PATH_MAX];
    const char *dev_path_ptr = (const char *) dev_path_array;
    uint8_t dev_path_len = 0;
    struct lanner_port *port;

    if (usb_helpers_convert_char_to_path(dev_path, dev_path_array,
                                         &dev_path_len)) {
        fprintf(stderr, "Path for Lanner device is too long\n");
        return 1;
    }

    port = calloc(sizeof(struct lanner_port), 1);

    if (!port) {
        fprintf(stderr, "Could not allocate memory for lanner port\n");
        return 1;
    }

    port->port_type = PORT_TYPE_LANNER;
    port->output = lanner_print_port;
    port->update = lanner_update_port;
    port->timeout = lanner_handle_timeout;

    port->shared_info = l_shared;
    //This is the bitmask used to enable/disable the port. The reason bit is
    //not zero-indexed in config, is to be consistent with Lanner tools/doc
    port->bitmask = 1 << (bit - 1);
    port->cur_state = LANNER_STATE_ON;

    //If we get into the situation where multiple paths are controlled by the
    //same bit, we need to implement a lookup here (similar to gpio and
    //gpio_num). However, so far on Lanner, one port == one bit
    if(usb_helpers_configure_port((struct usb_port *) port,
                                  ctx, dev_path_ptr, dev_path_len, 0, NULL)) {
        fprintf(stderr, "Failed to configure lanner port\n");
        free(port);
        return 1;
    }

    return 0;
}

static uint8_t lanner_handler_open_mcu(struct lanner_shared *l_shared)
{
    int fd = open(l_shared->mcu_path, O_RDWR | O_CLOEXEC), retval;
    struct termios mcu_attr;

    if (fd == -1) {
        fprintf(stderr, "Failed to open file: %s (%d)\n", strerror(errno),
                errno);
        return 1;
    }

    //Baud rate is 57600 according to documentation
    retval = tcgetattr(fd, &mcu_attr); 

    if (retval) {
        fprintf(stderr, "Fetching terminal attributes failed: %s (%d)\n",
                strerror(errno), errno);
        close(fd);
        return 1;
    }

    if (cfsetospeed(&mcu_attr, B57600)) {
        fprintf(stderr, "Setting speed failed: %s (%d)\n", strerror(errno),
                errno);
        close(fd);
        return 1;
    }

    retval = tcsetattr(fd, TCSANOW, &mcu_attr);

    if (retval) {
        fprintf(stderr, "Setting terminal attributes failed: %s (%d)\n",
                strerror(errno), errno);
        close(fd);
        return 1;
    }

    l_shared->mcu_fd = fd;

    return 0;
}

static void lanner_handler_cleanup_shared(struct lanner_shared *l_shared)
{
    if (l_shared->mcu_fd) {
        close(l_shared->mcu_fd);
    }

    if (l_shared->mcu_path) {
        free(l_shared->mcu_path);
    }

    free(l_shared);
}

uint8_t lanner_handler_parse_json(struct usb_monitor_ctx *ctx,
                                  struct json_object *json,
                                  const char *mcu_path_org)
{
    int json_arr_len = json_object_array_length(json);
    struct json_object *json_port, *path_array = NULL, *json_path;
    char *path, *mcu_path;
    const char *path_org;
    int i, j;
    uint8_t bit = UINT8_MAX, unknown_option = 0;
    struct lanner_shared *l_shared;

    if (!mcu_path_org) {
        return 1;
    }

    if (!(mcu_path = strdup(mcu_path_org))) {
        return 1;
    }

    l_shared = calloc(sizeof(struct lanner_shared), 1);

    if (!l_shared) {
        free(mcu_path);
        return 1;
    }

    l_shared->mcu_state = LANNER_MCU_IDLE;
    l_shared->mcu_path = mcu_path;

    if (lanner_handler_open_mcu(l_shared)) {
        lanner_handler_cleanup_shared(l_shared);
        return 1;
    }

    USB_DEBUG_PRINT_SYSLOG(ctx, LOG_INFO, "Lanner shared info. Path: %s "
                           "FD: %d\n", l_shared->mcu_path,
                           l_shared->mcu_fd);

    for (i = 0; i < json_arr_len; i++) {
        json_port = json_object_array_get_idx(json, i); 

        json_object_object_foreach(json_port, key, val) {
            if (!strcmp(key, "path") && json_object_is_type(val, json_type_array)) {
                path_array = val;
            } else if (!strcmp(key, "bit") && json_object_is_type(val, json_type_int)) {
                bit = (uint8_t) json_object_get_int(val);
            } else {
                unknown_option = 1;
                break;
            }
        }

        if (unknown_option || bit == UINT8_MAX || !path_array
            || !json_object_array_length(path_array)) {
            return 1;
        }
       
        for (j = 0; j < json_object_array_length(path_array); j++) {
            json_path = json_object_array_get_idx(path_array, j);
            path_org = json_object_get_string(json_path);
            path = strdup(path_org);

            if (!path) {
                lanner_handler_cleanup_shared(l_shared);
                return 1;
            }

            if (lanner_handler_add_port(ctx, path, bit, l_shared)) {
                free(path);
                lanner_handler_cleanup_shared(l_shared);
                return 1;
            }

            USB_DEBUG_PRINT_SYSLOG(ctx, LOG_INFO,
                                   "Read following info from config. Path  %s "
                                   "(bit %u) (Lanner)\n", path_org, bit);
            free(path);
        }
    }

    //This is not very clean, lanner state should ideally be completely
    //isolated. However, I prefer this approach to iterating through ports and
    //finding the first Lanner port (for example)
    ctx->mcu_info = l_shared;

    return 0;
}

void lanner_handler_start_mcu_update(struct usb_monitor_ctx *ctx)
{
    struct lanner_shared *l_shared = ctx->mcu_info;

    l_shared->mcu_state = LANNER_MCU_WRITING;
}
