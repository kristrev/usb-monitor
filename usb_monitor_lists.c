#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "usb_monitor.h"
#include "usb_monitor_lists.h"
#include "usb_helpers.h"

/* Port list functions. No need for a find since these do not depend on events */
void usb_monitor_lists_add_port(struct usb_monitor_ctx *ctx, struct usb_port *port)
{
    LIST_INSERT_HEAD(&(ctx->port_list), port, port_next);
}

void usb_monitor_lists_del_port(struct usb_port *port)
{
    LIST_REMOVE(port, port_next);

    //This is a work-around for an issue where a hub is removed while ports
    //are being reset. Resetting depends on the timer for sending the second
    //message, or retransmitting a message. When a device goes down, we
    //should not remove the port from the timeout list, or the message for
    //turning the port on again will never be sent. However, when hub goes
    //down, ignore whatever mode port is in and set mode to IDLE
    port->msg_mode = IDLE;
    usb_helpers_reset_port(port);

    //LIST_REMOVE does not set pointers to NULL after an element is removed.
    //Since we use NULL as a check for if an element is a member of a given list
    //or not, we need to do this manually
    port->port_next.le_next = NULL;
    port->port_next.le_prev = NULL;
}

struct usb_port *usb_monitor_lists_find_port_path(struct usb_monitor_ctx *ctx,
                                                 libusb_device *dev)
{
    uint8_t path[8];
    int32_t num_port_numbers;
    struct usb_port *itr;

    //Create path for device we want to look up
    path[0] = libusb_get_bus_number(dev);
    num_port_numbers = libusb_get_port_numbers(dev, path + 1, sizeof(path) - 1);

    LIST_FOREACH(itr, &(ctx->port_list), port_next) {
        //Need to compare both length and content to avoid matching subset of
        //paths
        if ((itr->path_len != (num_port_numbers + 1)) || 
            (memcmp(itr->path, path, num_port_numbers + 1)))
            continue;

        break;
    }

    return itr;
}

void usb_monitor_lists_add_timeout(struct usb_monitor_ctx *ctx, struct usb_port *port)
{
    LIST_INSERT_HEAD(&(ctx->timeout_list), port, timeout_next);
}

void usb_monitor_lists_del_timeout(struct usb_port *port)
{
    LIST_REMOVE(port, timeout_next);
    port->timeout_next.le_next = NULL;
    port->timeout_next.le_prev = NULL;
}

/* HUB list functions  */
void usb_monitor_lists_add_hub(struct usb_monitor_ctx *ctx, struct usb_hub *hub)
{
    //First, insert hub in list
    LIST_INSERT_HEAD(&(ctx->hub_list), hub, hub_next);

    //Whenever we add a hub, we also need to iterate through the list of devices
    //and see if we are aware of any that are connected
    usb_helpers_check_devices(ctx);
}

void usb_monitor_lists_del_hub(struct usb_hub *hub)
{
    LIST_REMOVE(hub, hub_next);
    hub->hub_next.le_next = NULL;
    hub->hub_next.le_prev = NULL;

}

struct usb_hub* usb_monitor_lists_find_hub(struct usb_monitor_ctx *ctx,
                                     libusb_device *hub)
{
    struct usb_hub *itr = ctx->hub_list.lh_first;

    LIST_FOREACH(itr, &(ctx->hub_list), hub_next)
        if (itr->hub_dev == hub)
            return itr;

    return NULL;
}
