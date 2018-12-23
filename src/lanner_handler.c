#include <json-c/json.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <sys/epoll.h>

#include "usb_monitor.h"
#include "usb_logging.h"
#include "usb_helpers.h"
#include "lanner_handler.h"
#include "backend_event_loop.h"

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
    l_shared->pending_ports_mask |= l_port->bitmask;

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
    int fd = open(l_shared->mcu_path, O_RDWR | O_NONBLOCK | O_CLOEXEC), retval;
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
    if (l_shared->mcu_epoll_handle) {
        free(l_shared->mcu_epoll_handle);
    }

    if (l_shared->mcu_fd) {
        close(l_shared->mcu_fd);
    }

    if (l_shared->mcu_path) {
        free(l_shared->mcu_path);
    }

    free(l_shared);
}

static void lanner_handler_write_cmd_buf(struct lanner_shared *l_shared)
{
    struct usb_monitor_ctx *ctx = l_shared->ctx;
    //uint8_t bytes_to_write = l_shared->cmd_buf_strlen - l_shared->cmd_buf_progress;
    uint8_t bytes_to_write = 1;
    ssize_t numbytes = write(l_shared->mcu_fd,
                             l_shared->cmd_buf + l_shared->cmd_buf_progress,
                             bytes_to_write);
    uint32_t monitor_events;

    if (numbytes == -1) {
        //Start EPOLLOUT
        if (errno == EAGAIN) {
            backend_event_loop_update(ctx->event_loop, EPOLLIN | EPOLLOUT,
                                      EPOLL_CTL_MOD, l_shared->mcu_fd,
                                      l_shared->mcu_epoll_handle);
        } else {
            //START TIMER AND TRY AGAIN? OR JUST STOP? OR JUST NOT DO ANYTHING?
            USB_DEBUG_PRINT_SYSLOG(ctx, LOG_ERR, "Failed to write to MCU: "
                                   "%s (%d)\n", strerror(errno), errno);
        }

        return;
    }

    l_shared->cmd_buf_progress += numbytes;

    if (l_shared->cmd_buf_progress == l_shared->cmd_buf_strlen) {
        USB_DEBUG_PRINT_SYSLOG(ctx, LOG_INFO, "Done writing command\n");
        monitor_events = EPOLLIN;
    } else {
        //We need to wait for EPOLLOUT, we had a short write
        monitor_events = EPOLLIN | EPOLLOUT;
    }

    backend_event_loop_update(ctx->event_loop, monitor_events,
                              EPOLL_CTL_MOD, l_shared->mcu_fd,
                              l_shared->mcu_epoll_handle);
}

static void lanner_handler_handle_input(struct lanner_shared *l_shared)
{
    uint8_t input_buf_tmp[256] = {0};
    ssize_t numbytes = read(l_shared->mcu_fd, input_buf_tmp,
                            sizeof(input_buf_tmp));

    USB_DEBUG_PRINT_SYSLOG(l_shared->ctx, LOG_INFO, "Read %zd bytes: %s\n",
                           numbytes, input_buf_tmp);
}

static void lanner_handler_event_cb(void *ptr, int32_t fd, uint32_t events)
{
    struct usb_monitor_ctx *ctx = ptr;

    if (events & EPOLLIN) {
        USB_DEBUG_PRINT_SYSLOG(ctx, LOG_INFO, "EPOLLIN\n");
        lanner_handler_handle_input(ctx->mcu_info);
    }

    if (events & EPOLLOUT) {
        USB_DEBUG_PRINT_SYSLOG(ctx, LOG_INFO, "EPOLLOUT\n");
        lanner_handler_write_cmd_buf(ctx->mcu_info);
    }
}

void lanner_handler_start_mcu_update(struct usb_monitor_ctx *ctx)
{
    struct usb_port *itr = NULL;
    struct lanner_port *l_port;
    struct lanner_shared *l_shared = ctx->mcu_info;
    uint8_t mcu_bitmask = 0;

    l_shared->mcu_state = LANNER_MCU_WRITING;

    LIST_FOREACH(itr, &(ctx->port_list), port_next) {
        if (itr->port_type != PORT_TYPE_LANNER) {
            continue;
        }

        l_port = (struct lanner_port*) itr;

        if (!(l_shared->pending_ports_mask & l_port->bitmask)) {
            //Initial value of mcu_bitmask is 0, which means that all ports
            //will be enabled. We need to set the bits of the ports that are
            //disabled
            if (!l_port->pwr_state) {
                mcu_bitmask |= l_port->bitmask;
            }
        }

        if (l_port->cur_cmd == CMD_DISABLE) {
            mcu_bitmask |= l_port->bitmask;
        }
    }

    snprintf(l_shared->cmd_buf, sizeof(l_shared->cmd_buf),
             "SET DIGITAL_OUT %u\n", mcu_bitmask);
    USB_DEBUG_PRINT_SYSLOG(ctx, LOG_INFO, "MCU cmd %s", l_shared->cmd_buf);

    memset(l_shared->buf_input, 0, sizeof(l_shared->buf_input));
    l_shared->input_progress = 0;
    l_shared->cmd_buf_strlen = strlen(l_shared->cmd_buf);
    l_shared->cmd_buf_progress = 0;

    lanner_handler_write_cmd_buf(l_shared);
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

    l_shared->ctx = ctx;
    l_shared->mcu_state = LANNER_MCU_IDLE;
    l_shared->mcu_path = mcu_path;

    if (lanner_handler_open_mcu(l_shared)) {
        lanner_handler_cleanup_shared(l_shared);
        return 1;
    }

    if (!(l_shared->mcu_epoll_handle = backend_create_epoll_handle(ctx,
                                                                   l_shared->mcu_fd,
                                                                   lanner_handler_event_cb,
                                                                   0))) {
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

    //Start monitoring for EPOLLIN events right away, we will just add a filter
    //to the callback to prevent parsing data when in wrong state
    backend_event_loop_update(ctx->event_loop, EPOLLIN, EPOLL_CTL_ADD,
                              l_shared->mcu_fd, l_shared->mcu_epoll_handle);

    //This is not very clean, lanner state should ideally be completely
    //isolated. However, I prefer this approach to iterating through ports and
    //finding the first Lanner port (for example)
    ctx->mcu_info = l_shared;
    return 0;
}
