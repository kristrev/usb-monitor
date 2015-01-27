#ifndef YKUSH_HANDLER_H
#define YKUSH_HANDLER_H

#include "usb_monitor.h"

#include <libusb-1.0/libusb.h>

#define YKUSH_VID 0x04d8
#define YKUSH_PID 0x0042

struct ykush_hub {
    USB_HUB_MANDATORY;
    libusb_device *comm_dev;
};

//This callback is used to handle YKUSH hubs being added and removed.
//TODO: Consider using forward declare to reduce number of headers
int ykush_event_cb(libusb_context *ctx, libusb_device *device,
                    libusb_hotplug_event event, void *user_data);

#endif
