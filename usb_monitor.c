#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <libusb-1.0/libusb.h>

#include "usb_monitor.h"
#include "ykush_handler.h"

void usb_monitor_add_hub(struct usb_monitor_ctx *ctx, struct usb_hub *hub)
{
    LIST_INSERT_HEAD(&(ctx->hub_list), hub, hub_next);
}

void usb_monitor_del_hub(struct usb_hub *hub)
{
    LIST_REMOVE(hub, hub_next);
}

struct usb_hub* usb_monitor_find_hub(struct usb_monitor_ctx *ctx,
                                     libusb_device *hub)
{
    struct usb_hub *itr = ctx->hub_list.lh_first;

    for (; itr != NULL; itr = itr->hub_next.le_next)
        if (itr->hub_dev == hub)
            return itr;

    return NULL;
}

int main(int argc, char *argv[])
{
    struct usb_monitor_ctx *ctx = NULL;
    int retval = 0;
    struct timeval tv = {1,0};

    ctx = malloc(sizeof(struct usb_monitor_ctx));

    if (ctx == NULL) {
        fprintf(stderr, "Failed to allocated application context struct\n");
        exit(EXIT_FAILURE);
    }

    LIST_INIT(&(ctx->hub_list));

    retval = libusb_init(NULL);
    if (retval) {
        fprintf(stderr, "libusb failed with error %s\n",
                libusb_error_name(retval));
        exit(EXIT_FAILURE);
    }

    //Register ykush callback
    libusb_hotplug_register_callback(NULL,
                                     LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
                                     LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
                                     LIBUSB_HOTPLUG_ENUMERATE,
                                     YKUSH_VID,
                                     YKUSH_PID,
                                     LIBUSB_HOTPLUG_MATCH_ANY,
                                     ykush_event_cb,
                                     ctx, NULL);

    //For now, just use the libusb wait-function as a basic event loop
    while (1)
        libusb_handle_events_timeout_completed(NULL, &tv, NULL);

    libusb_exit(NULL);
    exit(EXIT_SUCCESS);
}
