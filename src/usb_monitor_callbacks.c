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

#include <stdint.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "usb_logging.h"
#include "usb_monitor.h"
#include "usb_helpers.h"
#include "usb_monitor_lists.h"
#include "ykush_handler.h"
#include "generic_handler.h"
#include "backend_event_loop.h"

#include "gpio_handler.h"

/* libusb-callbacks for when devices are added/removed. It is also called
 * manually when we detect a hub, since we risk devices being added before we
 * see for example the YKUSH HID device */
static void usb_device_added(struct usb_monitor_ctx *ctx, libusb_device *dev)
{
    //Check if device is connected to a port we control
    struct usb_port *port;
    struct libusb_device_descriptor desc;
    uint8_t path[USB_PATH_MAX];
    uint8_t path_len;

    libusb_get_device_descriptor(dev, &desc);
    
    usb_helpers_fill_port_array(dev, path, &path_len);
    port = usb_monitor_lists_find_port_path(ctx, path, path_len);

    if (!port) {
        return;
    }

    //The enabled check is needed here sine we enable/disable async. So we can
    //process a disabled request before an add event. Of course, device will
    //most likely be remove in the next iteration of loop, but still ...
    if (port->msg_mode == RESET || !port->enabled)
        return;

    //Need to check port if it already has a device, since we can risk that we
    //are called two times for one device
    if (port->dev && port->dev == dev)
        return;

    USB_DEBUG_PRINT_SYSLOG(ctx, LOG_INFO,
            "Device: %.4x:%.4x added\n", desc.idVendor, desc.idProduct);

    //We need to configure port. So far, this is all generic
    port->vp.vid = desc.idVendor;
    port->vp.pid = desc.idProduct;
    port->status = PORT_DEV_CONNECTED;
    port->dev = dev;
    libusb_ref_device(dev);

    usb_monitor_print_ports(ctx);

    //Whenever we detect a device, we need to add to timeout to send ping.
    //However, we need to wait longer than the initial five seconds to let
    //usb_modeswitch potentially works its magic
    if (port->msg_mode == PROBE) {
        //TODO: Generic callback
        gpio_handler_handle_probe_connect(port);
    } else {
        port->msg_mode = PING;
        usb_helpers_start_timeout(port, ADDED_TIMEOUT_SEC);
    }
}

static void usb_device_removed(struct usb_monitor_ctx *ctx, libusb_device *dev)
{
    uint8_t path[USB_PATH_MAX];
    uint8_t path_len;
    struct usb_port *port = NULL;

    usb_helpers_fill_port_array(dev, path, &path_len);
    port = usb_monitor_lists_find_port_path(ctx, path, path_len);

    if (!port)
        return;

    usb_helpers_reset_port(port);
    usb_monitor_print_ports(ctx);
}

//Generic device callback
int usb_monitor_cb(libusb_context *ctx, libusb_device *device,
                          libusb_hotplug_event event, void *user_data)
{
    struct usb_monitor_ctx *usbmon_ctx = user_data;
    struct libusb_device_descriptor desc;

    libusb_get_device_descriptor(device, &desc);

    //Check if device belongs to a port we manage first. This is required for
    //example for cascading hubs, we need to the hub from the port is is
    //connected to, in addition to the port
    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED)
        usb_device_added(usbmon_ctx, device);
    else
        usb_device_removed(usbmon_ctx, device);

    //Multiple callbacks can be called multiple times, so it makes little sense
    //to register a separate ykush callback, when we anyway have to filter here
    if (desc.idVendor == YKUSH_VID &&
        (desc.idProduct == YKUSH_PID || desc.idProduct == YKUSH_PID2)) {
        ykush_event_cb(ctx, device, event, user_data);
    }

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

            //Due to async requests, the enabled guard is needed to prevent us
            //accidentaly sending PING on disabled port for example. However,
            //in the case of probe, we need to call timeout callback
            if (old_timeout->enabled || old_timeout->msg_mode == PROBE)
                old_timeout->timeout(old_timeout);
        } else {
            timeout_itr = timeout_itr->timeout_next.le_next;
        }
    }
}

//For events on USB socket
void usb_monitor_usb_event_cb(void *ptr, int32_t fd, uint32_t events)
{
    struct timeval tv = {0 ,0};
    libusb_unlock_events(NULL);
    libusb_handle_events_timeout_completed(NULL, &tv, NULL);
    libusb_lock_events(NULL);
}

void usb_monitor_check_devices_cb(void *ptr)
{
    struct usb_monitor_ctx *ctx = ptr;
    usb_helpers_check_devices(ctx);
}

void usb_monitor_check_reset_cb(void *ptr)
{
    struct usb_monitor_ctx *ctx = ptr;
    usb_helpers_reset_all_ports(ctx, 0);
}

//This function is called for every iteration + every second. Latter is needed
//in case of restart
void usb_monitor_itr_cb(void *ptr)
{
    struct usb_monitor_ctx *ctx = ptr;
    struct timeval tv = {0 ,0};

    //First, check for any of libusb's timers. We are incontrol of timer, so no
    //need for this function to block

    libusb_unlock_events(NULL);
    libusb_handle_events_timeout_completed(NULL, &tv, NULL);
    libusb_lock_events(NULL);

    //Check if we have any pending timeouts
    //TODO: Consider using the event loop timer queue for this
    usb_monitor_check_timeouts(ctx);
}

void usb_monitor_libusb_fd_add(int fd, short events, void *data)
{
    struct usb_monitor_ctx *ctx = data;

    backend_event_loop_update(ctx->event_loop,
                              events,
                              EPOLL_CTL_ADD,
                              fd,
                              ctx->libusb_handle);
}

void usb_monitor_libusb_fd_remove(int fd, void *data)
{
    //Closing a file descriptor causes it to be removed from epoll-set
    close(fd);
}

