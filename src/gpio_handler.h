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

#define GPIO_TIMEOUT_SLEEP_SEC 10

struct gpio_port {
    USB_PORT_MANDATORY;
};

struct json_object;

uint8_t gpio_handler_parse_json(struct usb_monitor_ctx *ctx, struct json_object *json);

#endif
