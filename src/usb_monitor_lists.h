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

#ifndef USB_MONITOR_LISTS_H
#define USB_MONITOR_LISTS_H

#include "usb_monitor.h"
#include <libusb-1.0/libusb.h>

//Searches for the hub_device in hub_list and returns 1 if found, 0 otherwise
struct usb_hub* usb_monitor_lists_find_hub(struct usb_monitor_ctx *ctx,
                                           libusb_device *hub);

//Add hub to list/delete hub from list
void usb_monitor_lists_add_hub(struct usb_monitor_ctx *ctx, struct usb_hub *hub);
void usb_monitor_lists_del_hub(struct usb_hub *hub);

//Add port to list/delete port from list
void usb_monitor_lists_add_port(struct usb_monitor_ctx *ctx, struct usb_port *port);
void usb_monitor_lists_del_port(struct usb_port *port);
struct usb_port *usb_monitor_lists_find_port_path(struct usb_monitor_ctx *ctx,
                                                  uint8_t *path,
                                                  uint8_t path_len);

//Add or delete port from timeout list
void usb_monitor_lists_add_timeout(struct usb_monitor_ctx *ctx, struct usb_port *port);
void usb_monitor_lists_del_timeout(struct usb_port *port);
uint8_t usb_monitor_lists_is_timeout_active(struct usb_port *port);

#endif
