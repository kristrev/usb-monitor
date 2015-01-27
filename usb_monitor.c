#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

#include <libusb-1.0/libusb.h>

#include "usb_monitor.h"
#include "ykush_handler.h"

static void usb_device_added(struct usb_monitor_ctx *ctx, libusb_device *dev);

/* Port list functions. No need for a find since these do not depend on events */
void usb_monitor_add_port(struct usb_monitor_ctx *ctx, struct usb_port *port)
{
    LIST_INSERT_HEAD(&(ctx->port_list), port, port_next);
}

void usb_monitor_del_port(struct usb_port *port)
{
    LIST_REMOVE(port, port_next);

    if (port->status == PORT_DEV_CONNECTED)
        libusb_unref_device(port->dev);
}

struct usb_port *usb_monitor_find_port_path(struct usb_monitor_ctx *ctx,
                                            libusb_device *dev)
{
    uint8_t path[8];
    int32_t num_port_numbers;
    struct usb_port *itr = ctx->port_list.lh_first;

    //Create path for device we want to look up
    path[0] = libusb_get_bus_number(dev);
    num_port_numbers = libusb_get_port_numbers(dev, path + 1, sizeof(path) - 1);

    for (; itr != NULL; itr = itr->port_next.le_next) {
        //Need to compare both length and content to avoid matching subset of
        //paths
        if ((itr->path_len != (num_port_numbers + 1)) || 
            (memcmp(itr->path, path, num_port_numbers + 1)))
            continue;

        break;
    }

    return itr;
}

/* HUB list functions  */
void usb_monitor_add_hub(struct usb_monitor_ctx *ctx, struct usb_hub *hub)
{
    ssize_t cnt, i;
    libusb_device **list, *dev;

    //First, insert hub in list
    LIST_INSERT_HEAD(&(ctx->hub_list), hub, hub_next);

    //Whenever we add a hub, we also need to iterate through the list of devices
    //and see if we are aware of any that are connected
    cnt = libusb_get_device_list(NULL, &list);

    if (cnt < 0) {
        fprintf(stderr, "Failed to get device list\n");
        assert(0);
    }

    for (i = 0; i<cnt; i++) {
        dev = list[i];
        usb_device_added(ctx, dev);
    }

    libusb_free_device_list(list, 1);
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

static void usb_monitor_print_ports(struct usb_monitor_ctx *ctx)
{
    struct usb_port *itr = ctx->port_list.lh_first;

    for (; itr != NULL; itr = itr->port_next.le_next)
        itr->output(itr);

    fprintf(stdout, "\n");
}

/* libusb-callbacks for when devices are added/removed */
static void usb_device_added(struct usb_monitor_ctx *ctx, libusb_device *dev)
{
    //Check if device is connected to a port we control
    struct usb_port *port = usb_monitor_find_port_path(ctx, dev);
    struct libusb_device_descriptor desc;

    if (!port)
        return;

    //Need to check port if it already has a device, since we can risk that we
    //are called two times for one device
    if (port->dev && port->dev == dev)
        return;

    libusb_get_device_descriptor(dev, &desc);

    fprintf(stdout, "Device: %.4x:%.4x added\n", desc.idVendor, desc.idProduct);

    //We need to configure port. So far, this is all generic
    port->status = PORT_DEV_CONNECTED;
    port->dev = dev;
    libusb_ref_device(dev);

    usb_monitor_print_ports(ctx);
}

static void usb_device_removed(struct usb_monitor_ctx *ctx, libusb_device *dev)
{
    struct usb_port *port = usb_monitor_find_port_path(ctx, dev);
    struct libusb_device_descriptor desc;

    if (!port)
        return;

    port->status = PORT_NO_DEV_CONNECTED;

    if (port->dev) {
        libusb_get_device_descriptor(dev, &desc);
        fprintf(stdout, "Device: %.4x:%.4x removed\n", desc.idVendor, desc.idProduct);

        port->dev = NULL;
        libusb_unref_device(dev);
    }

    usb_monitor_print_ports(ctx);
}

//Generic device callback
static int usb_monitor_cb(libusb_context *ctx, libusb_device *device,
                          libusb_hotplug_event event, void *user_data)
{
    struct usb_monitor_ctx *usbmon_ctx = user_data;
    struct libusb_device_descriptor desc;

    libusb_get_device_descriptor(device, &desc);

    //So far, we assume that all hubs will have separate callbacks, so ignore
    //those
    //TODO: Add support for hubs in hubs?
    if (desc.bDeviceClass == LIBUSB_CLASS_HUB)
        return 0;

    //TODO: Impelement a more scalable filtering strategy, if needed
    if (desc.idVendor == YKUSH_VID && desc.idProduct == YKUSH_PID)
        return 0;

    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED)
        usb_device_added(usbmon_ctx, device);
    else
        usb_device_removed(usbmon_ctx, device);


    return 0;
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
    LIST_INIT(&(ctx->port_list));

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
    
    libusb_hotplug_register_callback(NULL,
                                     LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
                                     LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
                                     LIBUSB_HOTPLUG_ENUMERATE,
                                     LIBUSB_HOTPLUG_MATCH_ANY,
                                     LIBUSB_HOTPLUG_MATCH_ANY,
                                     LIBUSB_HOTPLUG_MATCH_ANY,
                                     usb_monitor_cb,
                                     ctx, NULL);

    //For now, just use the libusb wait-function as a basic event loop
    while (1)
        libusb_handle_events_timeout_completed(NULL, &tv, NULL);

    libusb_exit(NULL);
    exit(EXIT_SUCCESS);
}
