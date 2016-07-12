/*
 * Copyright 2015 Kristian Evensen <kristian.evensen@gmail.com>
 *
 * This file is part of Usb Monitor. Usb Monitor is free software: you can
 * redistribute it and/or modify it under the terms of the Lesser GNU General
 * Public License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 *
 * Usb Montior is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * Usb Monitor. If not, see http://www.gnu.org/licenses/.
 */

#include <stdlib.h>
#include <string.h>

//TODO: Check this file closer, maybe there is something more I can use
#include "usb-kernel.h"

#include "generic_handler.h"
#include "usb_monitor.h"
#include "usb_logging.h"
#include "usb_helpers.h"
#include "usb_monitor_lists.h"

static void generic_print_port(struct usb_port *port)
{
    usb_helpers_print_port(port, "Generic");
}

static void generic_update_cb(struct libusb_transfer *transfer)
{
	struct generic_port *gport = (struct generic_port*) transfer->user_data;

	if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
        USB_DEBUG_PRINT(gport->ctx->logfile, "Failed to flip %u (%.4x:%.4x)\n",
                gport->port_num, gport->vp.vid, gport->vp.pid);
        //Set to IDLE in case of transfer error, we will then retry up/down on
        //next timeout (or on user request)
        gport->msg_mode = IDLE;
    } else {
        //This works because what I store is the result of the comparion, which
        //is either 0 or 1
        gport->pwr_state = !gport->pwr_state;

        //We always move ON->OFF->ON. With the guard in the async handler, we
        //can never interrupt a reset "call". There is no poing setting
        //msg_state to PING again. Device is disconnected, so it will be
        //connected again and we will set flag there
        if (gport->pwr_state == POWER_ON)
            gport->msg_mode = IDLE;
        else
            usb_helpers_start_timeout((struct usb_port*) gport, DEFAULT_TIMEOUT_SEC);
    }
}

static int32_t generic_update_port(struct usb_port *port, uint8_t cmd)
{
    struct libusb_transfer *transfer;
    struct generic_port *gport = (struct generic_port *) port;
    struct generic_hub *ghub = (struct generic_hub*) gport->parent;
	uint8_t request;

    gport->msg_mode = RESET;

	//Timeout guard
	if (gport->timeout_next.le_next != NULL ||
        gport->timeout_next.le_prev != NULL)
            usb_monitor_lists_del_timeout((struct usb_port*) gport);

    transfer = libusb_alloc_transfer(0);

    if (transfer == NULL) {
        USB_DEBUG_PRINT(gport->ctx->logfile,
                        "Could not allocate transfer for:\n");
        gport->output((struct usb_port*) gport);
        usb_helpers_start_timeout((struct usb_port*) gport, DEFAULT_TIMEOUT_SEC);
        return 0;
    }

    //Use flags to save us from adding som basic logic
    transfer->flags = LIBUSB_TRANSFER_SHORT_NOT_OK |
                      LIBUSB_TRANSFER_FREE_TRANSFER;

    //These values are defined in USB2.0 spec 11.2.2
    //Note that clearing port power, according to spec, MIGHT cause port to be
    //powered off. So this support is best effort. However, section 11.11 states
    //that if a hub indicates that is supports per-port power switching, it must
    //have a power switch per port. Seems like several manufacturers did not get
    //that memo
	request = gport->pwr_state ? USB_REQ_CLEAR_FEATURE : USB_REQ_SET_FEATURE;

    libusb_fill_control_setup(gport->ping_buf,
                              USB_TYPE_CLASS | USB_RECIP_OTHER,
                              request,
                              USB_PORT_FEAT_POWER,
                              gport->port_num,
                              0x00);

    libusb_fill_control_transfer(transfer,
                                 ghub->hub_handle,
                                 gport->ping_buf,
                                 generic_update_cb,
                                 gport,
                                 5000);
    
    if (libusb_submit_transfer(transfer)) {
        USB_DEBUG_PRINT(gport->ctx->logfile, "Failed to submit generic reset\n");
        libusb_free_transfer(transfer);
        libusb_release_interface(gport->dev_handle, 0);
        libusb_close(gport->dev_handle);
        gport->dev_handle = NULL;
        usb_helpers_start_timeout((struct usb_port*) gport, DEFAULT_TIMEOUT_SEC);
    }

    return 0;
}

static void generic_timeout_port(struct usb_port *port)
{
    if (port->msg_mode == PING)
        usb_helpers_send_ping(port);
    else
        generic_update_port(port, CMD_RESTART);
}

static void generic_configure_hub(struct usb_monitor_ctx *ctx,
                                   struct generic_hub *ghub)
{
    uint8_t hub_path[USB_PATH_MAX];
    const char *hub_path_ptr = (const char *) hub_path;
    int32_t num_port_numbers, i = 0;
    struct generic_port *gport = (struct generic_port*) (ghub + 1);

    hub_path[0] = libusb_get_bus_number(ghub->hub_dev);
    num_port_numbers = libusb_get_port_numbers(ghub->hub_dev,
                                               hub_path + 1,
                                               sizeof(hub_path) - 1);
    
    while (i < ghub->num_ports) {
        memset(gport, 0, sizeof(struct generic_port));
        hub_path[num_port_numbers + 1] = i + 1;
        gport->output = generic_print_port;
        gport->update = generic_update_port;
        gport->timeout = generic_timeout_port;

        usb_helpers_configure_port((struct usb_port*) gport,
                                  ctx, hub_path_ptr,
                                  num_port_numbers + 2, i + 1,
                                  (struct usb_hub*) ghub);

        gport = gport + 1;
        ++i;
    }
}

static void generic_add_device(libusb_context *ctx, libusb_device *device,
        void *user_data)
{
    struct usb_monitor_ctx *usbmon_ctx = user_data;
    struct libusb_device_descriptor desc;
    uint8_t num_ports = 0;
    struct generic_hub *ghub;
    int retval;

    libusb_get_device_descriptor(device, &desc);

    if (usb_monitor_lists_find_hub(usbmon_ctx, device))
        return;

    //Check if we support per port switching
    if (usb_helpers_get_power_switch(usbmon_ctx, device, desc.bcdUSB) != 1)
        return;

    num_ports = usb_helpers_get_num_ports(usbmon_ctx, device, desc.bcdUSB);

    if (!num_ports)
        return;

    USB_DEBUG_PRINT(usbmon_ctx->logfile,
                    "%.4x:%.4x supports port switching. Num. ports %u\n",
                    desc.idVendor, desc.idProduct, num_ports);

    ghub = malloc(sizeof(struct generic_hub) +
            (num_ports * sizeof(struct generic_port)));
    ghub->num_ports = num_ports;

    if (ghub == NULL) {
        USB_DEBUG_PRINT(usbmon_ctx->logfile, "Failed to allocate memory for generic hub\n");
        return;
    }

    retval = libusb_open(device, &(ghub->hub_handle));

    if (retval) {
        USB_DEBUG_PRINT(usbmon_ctx->logfile,
                        "Failed to open hub handle. Error: %s\n",
                        libusb_error_name(retval));
        free(ghub);
        return;
    }

    libusb_ref_device(device);
    ghub->hub_dev = device;

    generic_configure_hub(usbmon_ctx, ghub);
    usb_monitor_lists_add_hub(usbmon_ctx, (struct usb_hub*) ghub);
}

static void generic_del_device(libusb_context *ctx, libusb_device *device,
        void *user_data)
{
    struct usb_monitor_ctx *usbmon_ctx = user_data;
    struct generic_hub *ghub = (struct generic_hub*)
                               usb_monitor_lists_find_hub(usbmon_ctx, device);
    struct generic_port *gport = (struct generic_port*) (ghub + 1);
    uint8_t i = 0;

    if (ghub == NULL) {
        USB_DEBUG_PRINT(usbmon_ctx->logfile, "Generic hub not on list\n");
        return;
    }

    USB_DEBUG_PRINT(usbmon_ctx->logfile, "Will remove generic hub\n");

    libusb_close(ghub->hub_handle);
    libusb_unref_device(ghub->hub_dev);

    while (i < ghub->num_ports) {
        usb_helpers_release_port((struct usb_port*) gport);
        usb_monitor_lists_del_port((struct usb_port*) gport);
        gport = gport + 1;
        ++i;
    }

    usb_monitor_lists_del_hub((struct usb_hub*) ghub);
    free(ghub);
}

int generic_event_cb(libusb_context *ctx, libusb_device *device,
                   libusb_hotplug_event event, void *user_data)
{
    struct libusb_device_descriptor desc;

    libusb_get_device_descriptor(device, &desc);

    //For now, we blacklist the hub used in the YKUSH. This will be removed
    //later, when I have a proper solution ready. Problem is that YKUSH exports
    //a generic hub which advertises per-port power switching. This confuses our
    //current algorithms
    if (desc.idVendor == 0x0424 && desc.idProduct == 0x2514)
        return 0;


    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED)
        generic_add_device(ctx, device, user_data);
    else
        generic_del_device(ctx, device, user_data);

    //Here we can return 1 if we want the callback to be deregistered. Note that
    //this is ignored for the enumerate-part. So callback is only removed after
    //first actual event
    return 0;
}
