#ifndef LANNER_HANDLER_H
#define LANNER_HANDLER_H

#include <stdint.h>

enum {
    LANNER_STATE_ON,
    LANNER_STATE_OFF
};

enum {
    //There are no pending operations for the MCU
    LANNER_MCU_IDLE,
    //There are pending operations, but we have not started updating yet
    LANNER_MCU_PENDING,
    //We are writing to the MCU, MCU locked
    LANNER_MCU_WRITING,
    //We are waiting for OK from the MCU, MCU locked
    LANNER_MCU_WAIT_OK
};

struct lanner_shared {
    char *mcu_path;
    int mcu_fd;
    uint8_t mcu_state;
    //Mask of ports with a pending event
    uint8_t mcu_ports_mask;
};

struct lanner_port {
    USB_PORT_MANDATORY;
    struct lanner_shared *shared_info;
    uint8_t bitmask;
    uint8_t cur_cmd;
    //Lanner does not allow control of a single port, instead we must write
    //the complete bitmask every time
    uint8_t cur_state;
};

struct usb_monitor_ctx;
struct json_object;

uint8_t lanner_handler_parse_json(struct usb_monitor_ctx *ctx,
                                  struct json_object *json,
                                  const char *mcu_path);

#endif
