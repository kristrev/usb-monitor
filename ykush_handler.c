#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "ykush_handler.h"

static void ykush_add_device(libusb_context *ctx, libusb_device *device,
                             void *user_data)
{
    struct ykush_hub *yhub = NULL;
    struct usb_monitor_ctx *usbmon_ctx = user_data;

    //First step, get parent device and check if we already have it in the list
    libusb_device *parent = libusb_get_parent(device);
   
    if (usb_monitor_find_hub(usbmon_ctx, parent)) {
        fprintf(stderr, "Hub already found in list\n");
        return;
    }

    yhub = malloc(sizeof(struct ykush_hub));

    //TODO: Decide on error handling
    if (yhub == NULL) {
        fprintf(stderr, "Failed to allocate memory for YKUSH hub\n");
        return;
    }

    //Configure hub, split into new function
    //We need to have ownership of both devices, so we can match in del_device
    libusb_ref_device(device);
    libusb_ref_device(parent);

    printf("Found new hub\n");

    yhub->hub_dev = parent;
    yhub->comm_dev = device;

    usb_monitor_add_hub(usbmon_ctx, (struct usb_hub*) yhub);
}

static void ykush_del_device(libusb_context *ctx, libusb_device *device,
                             void *user_data)
{
    struct usb_monitor_ctx *usbmon_ctx = user_data;
    struct libusb_device *parent = libusb_get_parent(device);
    struct ykush_hub *yhub = (struct ykush_hub*)
                             usb_monitor_find_hub(usbmon_ctx, parent);

    if (yhub == NULL) {
        fprintf(stderr, "Hub not on list\n");
        return;
    }

    fprintf(stderr, "Will remove hub\n");

    libusb_unref_device(yhub->hub_dev);
    libusb_unref_device(yhub->comm_dev);

    usb_monitor_del_hub((struct usb_hub*) yhub);
    free(yhub);
}

int ykush_event_cb(libusb_context *ctx, libusb_device *device,
                   libusb_hotplug_event event, void *user_data)
{

    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED)
        ykush_add_device(ctx, device, user_data);
    else
        ykush_del_device(ctx, device, user_data);

    //Here we can return 1 if we want the callback to be deregistered. Note that
    //this is ignored for the enumerate-part. So callback is only removed after
    //first actual event
    return 0;
}

