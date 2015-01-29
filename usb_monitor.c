#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>

#include <libusb-1.0/libusb.h>

#include "usb_monitor.h"
#include "ykush_handler.h"
#include "usb_monitor_lists.h"
#include "usb_helpers.h"

static void usb_monitor_print_ports(struct usb_monitor_ctx *ctx)
{
    struct usb_port *itr;

    LIST_FOREACH(itr, &(ctx->port_list), port_next)
        itr->output(itr);

    fprintf(stdout, "\n");
}

static void usb_monitor_reset_all_ports(struct usb_monitor_ctx *ctx)
{
    struct usb_port *itr;

    LIST_FOREACH(itr, &(ctx->port_list), port_next) {
        //Only restart which are not connected and are currently not being reset
        if (itr->status == PORT_NO_DEV_CONNECTED &&
            itr->msg_mode != RESET)
            itr->update(itr);
    }
}

/* libusb-callbacks for when devices are added/removed. It is also called
 * manually when we detect a hub, since we risk devices being added before we
 * see for example the YKUSH HID device */
void usb_device_added(struct usb_monitor_ctx *ctx, libusb_device *dev)
{
    //Check if device is connected to a port we control
    struct usb_port *port;
    struct libusb_device_descriptor desc;

    libusb_get_device_descriptor(dev, &desc);

    //This is duplicated code from the event callback. However, it is currently
    //needed in order to handle the case were a hub fails to be added (for
    //example if we can't claim device). When this happens, we will iterate
    //through device and call usb_device_added for each. Hubs can be part of
    //this list, thus, this check is needed here as well
    //TODO: Think about how to unify code
    if (desc.idVendor == YKUSH_VID && desc.idProduct == YKUSH_PID) {
        ykush_event_cb(NULL, dev, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED, ctx);
        return;
    }
    
    port = usb_monitor_lists_find_port_path(ctx, dev);

    if (!port)
        return;

    //Need to check port if it already has a device, since we can risk that we
    //are called two times for one device
    if (port->dev && port->dev == dev)
        return;

    fprintf(stdout, "Device: %.4x:%.4x added\n", desc.idVendor, desc.idProduct);

    //We need to configure port. So far, this is all generic
    port->vid = desc.idVendor;
    port->pid = desc.idProduct;
    port->status = PORT_DEV_CONNECTED;
    port->dev = dev;
    port->msg_mode = PING;
    libusb_ref_device(dev);

    usb_monitor_print_ports(ctx);

    //Whenever we detect a device, we need to add to timeout to send ping.
    //However, we need to wait longer than the initial five seconds to let
    //usb_modeswitch potentially works its magic
    //TODO: Do not use magic value
    usb_helpers_start_timeout(port, DEFAULT_TIMEOUT_SEC);
}

static void usb_device_removed(struct usb_monitor_ctx *ctx, libusb_device *dev)
{
    struct usb_port *port = usb_monitor_lists_find_port_path(ctx, dev);

    if (!port)
        return;

    usb_helpers_reset_port(port);
    usb_monitor_print_ports(ctx);
}

//Generic device callback
static int usb_monitor_cb(libusb_context *ctx, libusb_device *device,
                          libusb_hotplug_event event, void *user_data)
{
    struct usb_monitor_ctx *usbmon_ctx = user_data;
    struct libusb_device_descriptor desc;

    libusb_get_device_descriptor(device, &desc);

    //Multiple callbacks can be called multiple times, so it makes little sense
    //to register a separate ykush callback, when we anyway have to filter here
    if (desc.idVendor == YKUSH_VID && desc.idProduct == YKUSH_PID) {
        ykush_event_cb(ctx, device, event, user_data);
        return 0;
    }

    //So far, we assume that all hubs will have separate callbacks, so ignore
    //those
    //TODO: Add support for hubs in hubs?
    if (desc.bDeviceClass == LIBUSB_CLASS_HUB)
        return 0;

    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED)
        usb_device_added(usbmon_ctx, device);
    else
        usb_device_removed(usbmon_ctx, device);


    return 0;
}

static void usb_monitor_check_timeouts(struct usb_monitor_ctx *ctx)
{
    struct usb_port *timeout_itr = NULL, *old_timeout = NULL;
    struct timeval tv;
    uint64_t cur_time;

    gettimeofday(&tv, NULL);
    cur_time = (tv.tv_sec * 1e6) + tv.tv_usec;

    timeout_itr = ctx->timeout_list.lh_first;

    while (timeout_itr != NULL) {
        if (cur_time >= timeout_itr->timeout_expire) {
            //Detatch from list, then run timeout
            old_timeout = timeout_itr;
            timeout_itr = timeout_itr->timeout_next.le_next;

            usb_monitor_lists_del_timeout(old_timeout);
            old_timeout->timeout(old_timeout);
        } else {
            timeout_itr = timeout_itr->timeout_next.le_next;
        }
    }
}

int main(int argc, char *argv[])
{
    struct usb_monitor_ctx *ctx = NULL;
    int retval = 0;
    struct timeval tv = {1,0};
    struct timeval last_restart, last_dev_check, cur_time;

    ctx = malloc(sizeof(struct usb_monitor_ctx));

    if (ctx == NULL) {
        fprintf(stderr, "Failed to allocated application context struct\n");
        exit(EXIT_FAILURE);
    }

    LIST_INIT(&(ctx->hub_list));
    LIST_INIT(&(ctx->port_list));
    LIST_INIT(&(ctx->timeout_list));

    retval = libusb_init(NULL);
    if (retval) {
        fprintf(stderr, "libusb failed with error %s\n",
                libusb_error_name(retval));
        exit(EXIT_FAILURE);
    }
    
    libusb_hotplug_register_callback(NULL,
                                     LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
                                     LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
                                     LIBUSB_HOTPLUG_ENUMERATE,
                                     LIBUSB_HOTPLUG_MATCH_ANY,
                                     LIBUSB_HOTPLUG_MATCH_ANY,
                                     LIBUSB_HOTPLUG_MATCH_ANY,
                                     usb_monitor_cb,
                                     ctx, NULL);

    gettimeofday(&last_restart, NULL);
    gettimeofday(&last_dev_check, NULL);
    //For now, just use the libusb wait-function as a basic event loop
    while (1) {
        libusb_handle_events_timeout_completed(NULL, &tv, NULL);

        //Check if we have any pending timeouts
        usb_monitor_check_timeouts(ctx);

        gettimeofday(&cur_time, NULL);

        //Do not run both checkes at the same time
        if (cur_time.tv_sec - last_dev_check.tv_sec > 30) {
            last_dev_check.tv_sec = cur_time.tv_sec;
            fprintf(stderr, "Will check for lost devices\n");
            usb_helpers_check_devices(ctx);
        } else if (cur_time.tv_sec - last_restart.tv_sec > 60) {
            last_restart.tv_sec = cur_time.tv_sec;

            fprintf(stderr, "Will restart all USB devices\n");
            usb_monitor_reset_all_ports(ctx);
        }
    }

    libusb_exit(NULL);
    exit(EXIT_SUCCESS);
}
