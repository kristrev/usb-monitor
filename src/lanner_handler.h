#ifndef LANNER_HANDLER_H
#define LANNER_HANDLER_H

#include <stdint.h>

#define LANNER_HANDLER_REPLY "100 DIGITAL_OUT"
#define LANNER_HANDLER_OK_REPLY "100 OK"

#define LANNER_HANDLER_RESTART_MS   5000

enum {
    LANNER_STATE_ON,
    LANNER_STATE_OFF
};

enum {
    //There are no pending operations for the MCU
    LANNER_MCU_IDLE,
    //There are pending operations, but we have not started updating yet
    LANNER_MCU_PENDING,
    //These two states are used as indications to the timeout handler
    LANNER_MCU_LOCK,
    LANNER_MCU_OPEN_FILE,
    //We are reading from the MCU
    LANNER_MCU_READING,
    //Wait for MCU to reply with DIGITAL_OUT
    LANNER_MCU_WAIT_REPLY,
    //We are writing to the MCU, MCU locked
    LANNER_MCU_WRITING,
    //We are waiting for OK from the MCU, MCU locked
    LANNER_MCU_WAIT_OK,
    LANNER_MCU_UPDATE_DONE,
};

struct backend_epoll_handle;
struct backend_timeout_handle;
struct usb_monitor_ctx;

struct lanner_shared {
    struct usb_monitor_ctx *ctx;
    char *mcu_path;
    struct backend_epoll_handle *mcu_epoll_handle;
    struct backend_timeout_handle *mcu_timeout_handle;

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

    uint8_t mcu_bitmask;
    uint8_t mcu_bitmask_to_write;
};

struct lanner_port {
    USB_PORT_MANDATORY;
    struct lanner_shared *shared_info;
    uint8_t bitmask;
    uint8_t cur_cmd;
    uint8_t restart_cmd;
};

struct json_object;

void lanner_handler_itr_cb(struct usb_monitor_ctx *ctx);

uint8_t lanner_handler_parse_json(struct usb_monitor_ctx *ctx,
                                  struct json_object *json,
                                  const char *mcu_path);

#endif
