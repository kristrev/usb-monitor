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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/time.h>

#include "usb_monitor.h"
#include "ykush_handler.h"
#include "usb_helpers.h"
#include "usb_monitor_lists.h"
#include "usb_logging.h"

static int32_t ykush_update_port(struct usb_port *port, uint8_t cmd);

/* TODO: Parts of this function is generic, split in two */
static void ykush_print_port(struct usb_port *port)
{
    usb_helpers_print_port(port, "YKUSH");
}

static void ykush_enable_cb(struct libusb_transfer *transfer)
{
    struct ykush_port *yport = transfer->user_data;

    if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
        USB_DEBUG_PRINT_SYSLOG(yport->ctx, LOG_ERR,
                "Failed to enable %u (%.4x:%.4x)\n",
                yport->port_num, yport->u.vp.vid, yport->u.vp.pid);
        return;
    }

    yport->enabled = 1;
    yport->pwr_state = 1;
}

static void ykush_disable_cb(struct libusb_transfer *transfer)
{
    struct ykush_port *yport = transfer->user_data;

    if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
        USB_DEBUG_PRINT_SYSLOG(yport->ctx, LOG_ERR,
                "Failed to disable %u (%.4x:%.4x)\n",
                yport->port_num, yport->u.vp.vid, yport->u.vp.pid);
        return;
    }

    yport->enabled = 0;
    yport->msg_mode = IDLE;
    yport->pwr_state = 0;
}

static void ykush_reset_cb(struct libusb_transfer *transfer)
{
    struct ykush_port *yport = transfer->user_data;

    //This block is needed for several reasons. First of all, there is no point
    //in continuing with reset if port is disabled. Second, it prevents the
    //reset code from running in case of the following chain of events: disable
    //(async), reset, disable callback, rest callback
    if (!yport->enabled)
        return;

    if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
        USB_DEBUG_PRINT_SYSLOG(yport->ctx, LOG_ERR,
                "Failed to flip %u (%.4x:%.4x) %u %s\n",
                yport->port_num, yport->u.vp.vid, yport->u.vp.pid, transfer->status, libusb_strerror(transfer->status));
        //Set to IDLE in case of transfer error, we will then retry up/down on
        //next timeout (or on user request)
        yport->msg_mode = IDLE;
    } else {
        USB_DEBUG_PRINT_SYSLOG(yport->ctx, LOG_INFO, "Flipped port\n");
        //This works because what I store is the result of the comparion, which
        //is either 0 or 1
        yport->pwr_state = !yport->pwr_state;

        //We always move ON->OFF->ON. With the guard in the async handler, we
        //can never interrupt a reset "call". There is no poing setting
        //msg_state to PING again. Device is disconnected, so it will be
        //connected again and we will set flag there
        if (yport->pwr_state == POWER_ON)
            yport->msg_mode = IDLE;
        else
            usb_helpers_start_timeout((struct usb_port*) yport, DEFAULT_TIMEOUT_SEC);
    }
}

static int32_t ykush_perform_transfer(struct ykush_port *yport,
        struct ykush_hub * yhub, uint8_t port_cmd,libusb_transfer_cb_fn cb)
{
    struct libusb_transfer *transfer;
    int32_t retval = 0, buf_len = yhub->old_fw ? 6 : 64;

    yport->buf[0] = yport->buf[1] = port_cmd;

    //Follow the steps of the libusb async manual
    transfer = libusb_alloc_transfer(0);

    if (transfer == NULL) {
        USB_DEBUG_PRINT_SYSLOG(yport->ctx, LOG_ERR,
                "Could not allocate trasnfer\n");
        return -1;
    }

    //Use flags to save us from adding som basic logic
    transfer->flags = LIBUSB_TRANSFER_SHORT_NOT_OK |
                      LIBUSB_TRANSFER_FREE_TRANSFER;

    libusb_fill_interrupt_transfer(transfer,
                                   yhub->comm_handle,
                                   0x01,
                                   yport->buf,
                                   buf_len,
                                   cb,
                                   yport,
                                   5000);

    retval = libusb_submit_transfer(transfer);

    if (retval) {
        USB_DEBUG_PRINT_SYSLOG(yport->ctx, LOG_ERR,
                "Failed to submit transfer\n");
        libusb_free_transfer(transfer);
    }

    return retval;
}

static int32_t ykush_update_port(struct usb_port *port, uint8_t cmd)
{
    struct ykush_port *yport = (struct ykush_port*) port;
    struct ykush_hub *yhub = (struct ykush_hub*) port->parent;
    uint8_t port_cmd = 0;

    switch (yport->port_num) {
    case 1:
        port_cmd = YKUSH_CMD_PORT_1;
        break;
    case 2:
        port_cmd = YKUSH_CMD_PORT_2;
        break;
    case 3:
        port_cmd = YKUSH_CMD_PORT_3;
        break;
    default:
        USB_DEBUG_PRINT_SYSLOG(yport->ctx, LOG_ERR, "Unknown port, aborting\n");
        return -1;
    }

    if (cmd == CMD_ENABLE) {
        port_cmd |= 0x10;
        if (ykush_perform_transfer(yport, yhub, port_cmd, ykush_enable_cb))
            return -1;
        else
            return 0;
    } else if (cmd == CMD_DISABLE) {
        if (ykush_perform_transfer(yport, yhub, port_cmd, ykush_disable_cb))
            return -1;
        else
            return 0;
    }

    if (!yport->enabled)
        return 0;

    //Prevent resetting port multiple times. msg_mode is only set to reset when
    //we call this function, and unset when a reset is done. This guard is
    //needed when we add support for async reset notifications
    yport->msg_mode = RESET;

    //For asynchrnous resets (i.e., resets triggered by something else than our
    //code), we might get a reset notification while waiting to send the net
    //ping. We therefore need to make sure the port is removed from the timeout
    //list, since it does not make sense to try to send ping while resetting
    //device.
    if (yport->timeout_next.le_next != NULL ||
        yport->timeout_next.le_prev != NULL)
            usb_monitor_lists_del_timeout((struct usb_port*) yport);
    
    if (!yport->pwr_state)
        port_cmd |= 0x10;

    //Not considered an error, we will retry again later
    if (ykush_perform_transfer(yport, yhub, port_cmd, ykush_reset_cb))
        usb_helpers_start_timeout(port, DEFAULT_TIMEOUT_SEC);

    return 0;
}

static void ykush_handle_timeout(struct usb_port *port)
{
    if (port->msg_mode == PING)
        usb_helpers_send_ping(port);
    else
        ykush_update_port(port, CMD_RESTART);
}

//Return 1 if old firmware, 0 if new. Assumes new if something fails
static uint8_t ykush_check_old_firmware(struct usb_monitor_ctx *ctx,
        struct ykush_hub *yhub)
{
    struct libusb_device_descriptor desc;
    uint8_t old_firmware = 0;
    //Theoretical maximum length of serial is 256 bytes (0-255), so add one more
    //byte here to make sure string is terminated
    uint8_t dev_serial[257] = {0};
    uint32_t serial_num = 0;

    if (libusb_get_device_descriptor(yhub->comm_dev, &desc)) {
        USB_DEBUG_PRINT_SYSLOG(ctx, LOG_ERR,
                "Failed to allocate memory for YKUSH hub\n");
        return old_firmware;
    }

    libusb_get_string_descriptor_ascii(yhub->comm_handle, desc.iSerialNumber,
            dev_serial, 256);

    USB_DEBUG_PRINT_SYSLOG(ctx, LOG_INFO, "Serial num. %s\n", dev_serial);

    //Serial, at least for now, always starts with YK
    serial_num = atoi((const char*) (dev_serial + 2));

    if (serial_num < YKUSH_OLD_FW) {
        USB_DEBUG_PRINT_SYSLOG(ctx, LOG_INFO, "Device with old firmware\n");
        old_firmware = 1;
    }

    return old_firmware;
}

static uint8_t ykush_configure_hub(struct usb_monitor_ctx *ctx,
                                   struct ykush_hub *yhub)
{
    //TODO: When YKUSH makes a USB 3.0-hub, update this
    uint8_t num_ports = usb_helpers_get_num_ports(ctx, yhub->hub_dev, 0x200);
    uint8_t i;
    uint8_t comm_path[USB_PATH_MAX];
    const char *comm_path_ptr = (const char*) comm_path;
    int32_t num_port_numbers = 0, retval = 0;

    if (!num_ports)
        return 0;
   
    //The HID device occupies on port on hub device
    num_ports -= 1;

    if (num_ports != MAX_YKUSH_PORTS) {
        USB_DEBUG_PRINT_SYSLOG(ctx, LOG_ERR, "YKUSH hub with odd number of ports %u\n", num_ports);
        return 0;
    }

    //Set up com device
    retval = libusb_open(yhub->comm_dev, &(yhub->comm_handle));
    if (retval) {
        USB_DEBUG_PRINT_SYSLOG(ctx, LOG_ERR,
                "Open failed: %s\n", libusb_error_name(retval));
        return 0;
    }

    retval = libusb_detach_kernel_driver(yhub->comm_handle, 0);
    //This error is not critical, it just means that there was no driver
    if (retval && retval != LIBUSB_ERROR_NOT_FOUND) {
        USB_DEBUG_PRINT_SYSLOG(ctx, LOG_ERR, "Detatch failed: %s\n",
                libusb_error_name(retval));
        libusb_close(yhub->comm_handle);
        return 0;
    }

    retval = libusb_set_configuration(yhub->comm_handle, 1);
    if (retval) {
        USB_DEBUG_PRINT_SYSLOG(ctx, LOG_ERR, "Config failed: %s\n",
                libusb_error_name(retval));
        libusb_close(yhub->comm_handle);
        return 0;
    }

    retval = libusb_claim_interface(yhub->comm_handle, 0);
    if (retval) {
        USB_DEBUG_PRINT_SYSLOG(ctx, LOG_ERR, "Claim failed: %s\n",
                libusb_error_name(retval));
        libusb_close(yhub->comm_handle);
        return 0;
    }

    yhub->old_fw = ykush_check_old_firmware(ctx, yhub);
    yhub->num_ports = num_ports;

    comm_path[0] = libusb_get_bus_number(yhub->comm_dev);
    num_port_numbers = libusb_get_port_numbers(yhub->comm_dev,
                                               comm_path + 1,
                                               sizeof(comm_path) - 1);
    for (i = 0; i < yhub->num_ports; i++) {
        memset(&(yhub->port[i]), 0, sizeof(struct ykush_port));
        comm_path[num_port_numbers] = i + 1;
        yhub->port[i].output = ykush_print_port;
        yhub->port[i].update = ykush_update_port;
        yhub->port[i].timeout = ykush_handle_timeout;
        yhub->port[i].port_type = PORT_TYPE_YKUSH;

        retval = usb_helpers_configure_port((struct usb_port*) &(yhub->port[i]),
                                            ctx, comm_path_ptr,
                                            num_port_numbers + 1, i + 1,
                                            (struct usb_hub*) yhub);

        if (retval)
            break;
    }

    if (!retval)
        return num_ports;
    else
        return 0;
}

static void ykush_release_memory(struct ykush_hub *yhub)
{
    uint8_t i = 0;

    //According to documentation, comm_handle is only populated if open() is
    //successfull
    if (yhub->comm_handle) {
        //According to documentation, safe to call also for non-claimed devices
        libusb_release_interface(yhub->comm_handle, 0);
        libusb_close(yhub->comm_handle);
    }

    //Always referenced before this function is called (see add_device)
    libusb_unref_device(yhub->hub_dev);
    libusb_unref_device(yhub->comm_dev);

    for (i = 0; i < yhub->num_ports; i++) {
        usb_helpers_release_port((struct usb_port*) &(yhub->port[i]));
        usb_monitor_lists_del_port((struct usb_port*) &(yhub->port[i]));
    }

    usb_monitor_lists_del_hub((struct usb_hub*) yhub);
    free(yhub);
}

static void ykush_add_device(libusb_context *ctx, libusb_device *device,
                             void *user_data)
{
    struct ykush_hub *yhub = NULL;
    struct usb_monitor_ctx *usbmon_ctx = user_data;

    //First step, get parent device and check if we already have it in the list
    libusb_device *parent = libusb_get_parent(device);
   
    if (usb_monitor_lists_find_hub(usbmon_ctx, parent))
        return;

    yhub = calloc(sizeof(struct ykush_hub), 1);

    //TODO: Decide on error handling
    if (yhub == NULL) {
        USB_DEBUG_PRINT_SYSLOG(usbmon_ctx, LOG_ERR,
                "Failed to allocate memory for YKUSH hub\n");
        return;
    }

    //Configure hub, split into new function
    //We need to have ownership of both devices, so we can match in del_device
    libusb_ref_device(device);
    libusb_ref_device(parent);

    yhub->hub_dev = parent;
    yhub->comm_dev = device;

    //TODO: Check error code
    if (!ykush_configure_hub(usbmon_ctx, yhub)) {
        USB_DEBUG_PRINT_SYSLOG(usbmon_ctx, LOG_ERR,
                "YKUSH hub configuration failed\n");
        ykush_release_memory(yhub);
        return;
    }
    
    usb_monitor_lists_add_hub(usbmon_ctx, (struct usb_hub*) yhub);
    USB_DEBUG_PRINT_SYSLOG(usbmon_ctx, LOG_INFO,
            "Added new YKUSH hub. Num. ports %u\n", yhub->num_ports);
}

static void ykush_del_device(libusb_context *ctx, libusb_device *device,
                             void *user_data)
{
    struct usb_monitor_ctx *usbmon_ctx = user_data;
    struct libusb_device *parent = libusb_get_parent(device);
    struct ykush_hub *yhub = (struct ykush_hub*)
                             usb_monitor_lists_find_hub(usbmon_ctx, parent);

    if (yhub == NULL) {
        USB_DEBUG_PRINT_SYSLOG(usbmon_ctx, LOG_INFO, "Hub not on list\n");
        return;
    }

    USB_DEBUG_PRINT_SYSLOG(usbmon_ctx, LOG_INFO, "Will remove YKUSH hub\n");
    ykush_release_memory(yhub);
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

