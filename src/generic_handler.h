/*
 * Copyright 2015 Kristian Evensen <kristian.evensen@gmail.com>
 *
 * This file is part of Usb Monitor. Usb Monitor is free software: you can
 * redistribute it and/or modify it under the terms of the Lesser GNU General
 * Public License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 *
 * Usb Montior is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * Usb Monitor. If not, see http://www.gnu.org/licenses/.
 */

#ifndef GENERIC_HANDLER
#define GENERIC_HANDLER

#include <stdint.h>
#include <libusb-1.0/libusb.h>

#include "usb_monitor.h"

#define USB_PORT_FEAT_POWER 8

struct generic_hub {
    USB_HUB_MANDATORY;
    //With a generic hub, reset messages are sent to the hub directly
    libusb_device_handle *hub_handle;
};

struct generic_port {
    USB_PORT_MANDATORY;
};

int generic_event_cb(libusb_context *ctx, libusb_device *device,
                   libusb_hotplug_event event, void *user_data);

#endif
