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
