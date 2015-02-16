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

#ifndef YKUSH_HANDLER_H
#define YKUSH_HANDLER_H

#include "usb_monitor.h"

#include <libusb-1.0/libusb.h>

#define YKUSH_VID 0x04d8
#define YKUSH_PID 0x0042

#define YKUSH_CMD_PORT_1    0x01
#define YKUSH_CMD_PORT_2    0x02
#define YKUSH_CMD_PORT_3    0x03
#define YKUSH_CMD_ALL       0x0A

#define MAX_YKUSH_PORTS 3

struct ykush_port {
    USB_PORT_MANDATORY;
    //When doing async transfer, buffer needs to be allocated on heap
    uint8_t buf[6];
};

struct ykush_hub {
    USB_HUB_MANDATORY;
    libusb_device *comm_dev;
    libusb_device_handle *comm_handle;
    //These values are kept around in case Yepkit launches a different hub with
    //a different number of ports.
    //TODO: Consider using pointers, to reduce size of struct
    struct ykush_port port[MAX_YKUSH_PORTS];
};

//This callback is used to handle YKUSH hubs being added and removed.
//TODO: Consider using forward declare to reduce number of headers
int ykush_event_cb(libusb_context *ctx, libusb_device *device,
                    libusb_hotplug_event event, void *user_data);

#endif
