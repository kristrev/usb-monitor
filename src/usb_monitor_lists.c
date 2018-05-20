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
    if (port->port_next.le_next == NULL &&
        port->port_next.le_prev == NULL)
        return;

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

//This one should only accept context + path + len
struct usb_port *usb_monitor_lists_find_port_path(struct usb_monitor_ctx *ctx,
                                                  uint8_t *path,
                                                  uint8_t path_len)
{
    uint8_t i;
    struct usb_port *itr = NULL;

    LIST_FOREACH(itr, &(ctx->port_list), port_next) {
        for (i = 0; i < MAX_NUM_PATHS; i++) {
            if (!itr->path[i])
                break;

            if ((itr->path_len[i] == path_len) &&
                (!memcmp(itr->path[i], path, path_len)))
                return itr;
        }
    }

    return NULL;
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

uint8_t usb_monitor_lists_is_timeout_active(struct usb_port *port)
{
    return port->timeout_next.le_next || port->timeout_next.le_prev;
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

    if (hub->hub_next.le_next == NULL &&
        hub->hub_next.le_prev == NULL)
        return;

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
