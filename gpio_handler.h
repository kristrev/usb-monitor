#ifndef GPIO_HANDLER_H
#define GPIO_HANDLER_H

#include "usb_monitor.h"

#define GPIO_TIMEOUT_SLEEP_SEC 10

struct gpio_port {
    USB_PORT_MANDATORY;
};

struct json_object;

uint8_t gpio_handler_add_port(struct usb_monitor_ctx *ctx, char *path, uint8_t gpio_num);
uint8_t gpio_handler_parse_json(struct usb_monitor_ctx *ctx, struct json_object *json);

#endif
