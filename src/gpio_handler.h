/*
 * Copyright 2015 Kristian Evensen <kristian.evensen@gmail.com>
 *
 * This file is part of Usb Monitor. Usb Monitor is free software: you can
 * redistribute it and/or modify it under the terms of the Lesser GNU General
 * Public License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 *
 * Usb Monitor is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * Usb Monitor. If not, see http://www.gnu.org/licenses/.
 */

#ifndef GPIO_HANDLER_H
#define GPIO_HANDLER_H

#include "usb_monitor.h"

#define GPIO_DEFAULT_ON_VAL             1
#define GPIO_DEFAULT_OFF_VAL            0
#define GPIO_TIMEOUT_SLEEP_SEC          10
#define GPIO_TIMEOUT_PROBE_DISABLE_SEC  5
#define GPIO_TIMEOUT_PROBE_ENABLE_SEC   30
//64 is large anough to store maximum sysfs paths (/sys/class/gpio/gpioX/value)
#define GPIO_PATH_MAX_LEN      64

enum {
    PROBE_IDLE = 0,
    PROBE_DOWN,
    PROBE_DOWN_DONE,
    PROBE_UP,
    PROBE_DONE
};

struct gpio_port {
    USB_PORT_MANDATORY;
    const char *gpio_path;
    uint16_t gpio_num;
    uint8_t on_val;
    uint8_t off_val;
    uint8_t probe_state;
};

struct json_object;

uint8_t gpio_handler_parse_json(struct usb_monitor_ctx *ctx, struct json_object *json);

int32_t gpio_handler_start_probe(struct usb_monitor_ctx *ctx);

void gpio_handler_handle_probe_connect(struct usb_port *port);
#endif
