#ifndef LANNER_HANDLER_H
#define LANNER_HANDLER_H

#include <stdint.h>

enum {
    LANNER_STATE_ON,
    LANNER_STATE_OFF
};

struct lanner_shared {
    char *mcu_path;
    int mcu_fd;
};

struct lanner_port {
    USB_PORT_MANDATORY;
    struct lanner_shared *shared_info;
    uint8_t bitmask;
    //Lanner does not allow control of a single port, instead we must write the complete bitmask every time
    uint8_t cur_state;
};

struct usb_monitor_ctx;
struct json_object;

uint8_t lanner_handler_parse_json(struct usb_monitor_ctx *ctx, struct json_object *json);

#endif
