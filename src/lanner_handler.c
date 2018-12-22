#include <json-c/json.h>
#include <string.h>

#include "usb_monitor.h"
#include "usb_logging.h"
#include "usb_helpers.h"
#include "lanner_handler.h"

static void lanner_print_port(struct usb_port *port)
{
    char buf[4] = {0};
    struct lanner_port *l_port = (struct lanner_port *) port;

    snprintf(buf, sizeof(buf), "%u", (uint8_t) ffs(l_port->bitmask));

    usb_helpers_print_port(port, "Lanner", buf);
}

static int32_t lanner_update_port(struct usb_port *port, uint8_t cmd)
{
    USB_DEBUG_PRINT_SYSLOG(port->ctx, LOG_INFO, "Update port %u\n", cmd);
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
                                       char *dev_path, uint8_t bit)
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

uint8_t lanner_handler_parse_json(struct usb_monitor_ctx *ctx,
                                  struct json_object *json)
{
    int json_arr_len = json_object_array_length(json);
    struct json_object *json_port, *path_array = NULL, *json_path;
    char *path;
    const char *path_org;
    int i, j;
    uint8_t bit = UINT8_MAX, unknown_option = 0;

    for (i = 0; i < json_arr_len; i++) {
        json_port = json_object_array_get_idx(json, i); 

        json_object_object_foreach(json_port, key, val) {
            if (!strcmp(key, "path") && json_object_is_type(val, json_type_array)) {
                path_array = val;
                continue;
            } else if (!strcmp(key, "bit") && json_object_is_type(val, json_type_int)) {
                bit = (uint8_t) json_object_get_int(val);
                continue;
            } else {
                unknown_option = 1;
                break;
            }
        }

        if (unknown_option ||
            bit == UINT8_MAX ||
            !path_array ||
            !json_object_array_length(path_array)) {
            return 1;
        }
        
        for (j = 0; j < json_object_array_length(path_array); j++) {
            json_path = json_object_array_get_idx(path_array, j);
            path_org = json_object_get_string(json_path);
            path = strdup(path_org);

            if (!path) {
                return 1;
            }

            if (lanner_handler_add_port(ctx, path, bit)) {
                free(path);
                return 1;
            }

            USB_DEBUG_PRINT_SYSLOG(ctx, LOG_INFO,
                                   "Read following info from config. Path  %s "
                                   "(bit %u) (Lanner)\n", path_org, bit);
            free(path);
        }
    }

    return 0;
}
