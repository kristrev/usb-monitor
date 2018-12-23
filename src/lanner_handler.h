#ifndef LANNER_HANDLER_H
#define LANNER_HANDLER_H

#include <stdint.h>

#define LANNER_HANDLER_OK_REPLY "100 OK"

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

struct backend_epoll_handle;
struct usb_monitor_ctx;

struct lanner_shared {
    struct usb_monitor_ctx *ctx;
    char *mcu_path;
    struct backend_epoll_handle *mcu_epoll_handle;

    int mcu_fd;
    uint8_t mcu_state;
    //Mask of ports with a pending event
    uint8_t pending_ports_mask;

    //Buffer that will keep our output string. Big enough to contain:
    //SET DIGITAL_OUT X\n\0, where X has three digits
    char cmd_buf[21];
    char buf_input[256];

    uint8_t cmd_buf_strlen;
    uint8_t cmd_buf_progress;
    uint8_t input_progress;
};

struct lanner_port {
    USB_PORT_MANDATORY;
    struct lanner_shared *shared_info;
    uint8_t bitmask;
    uint8_t cur_cmd;
    uint8_t rest_sub_cmd;
    //Lanner does not allow control of a single port, instead we must write
    //the complete bitmask every time
    uint8_t cur_state;
};

struct json_object;

void lanner_handler_start_mcu_update(struct usb_monitor_ctx *ctx);

uint8_t lanner_handler_parse_json(struct usb_monitor_ctx *ctx,
                                  struct json_object *json,
                                  const char *mcu_path);

#endif
