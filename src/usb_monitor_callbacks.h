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

#ifndef USB_MONITOR_CALLBACKS_H
#define USB_MONITOR_CALLBACKS_H

#include <libusb-1.0/libusb.h>

//Libusb event callback
int usb_monitor_cb(libusb_context *ctx, libusb_device *device,
                          libusb_hotplug_event event, void *user_data);

//Event callback from our event loop
void usb_monitor_usb_event_cb(void *ptr, int32_t fd, uint32_t events);

//These are the three timeout callbacks
void usb_monitor_check_devices_cb(void *ptr);
void usb_monitor_check_reset_cb(void *ptr);
void usb_monitor_1sec_timeout_cb(void *ptr);

//Libusb file descriptor callbacks
void usb_monitor_libusb_fd_add(int fd, short events, void *data);
void usb_monitor_libusb_fd_remove(int fd, void *data);

#endif
