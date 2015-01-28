#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sys/time.h>

#include "usb_monitor.h"
#include "ykush_handler.h"
#include "usb_helpers.h"
#include "usb_monitor_lists.h"

static void ykush_update_port(struct usb_port *port);

/* TODO: This function is generic  */
static void ykush_print_port(struct usb_port *port)
{
    int i;
    struct libusb_device_descriptor desc;

    fprintf(stdout, "Type: YKUSH Path: ");

    for (i = 0; i < port->path_len-1; i++)
        fprintf(stdout, "%u-", port->path[i]);

    fprintf(stdout, "%u State: %u Pwr: %u ", port->path[i], port->status, port->pwr_state);

    if (port->dev) {
        libusb_get_device_descriptor(port->dev, &desc);
        fprintf(stdout, " Device: %.4x:%.4x", desc.idVendor, desc.idProduct);
    }

    fprintf(stdout, "\n");
}

static void ykush_reset_cb(struct libusb_transfer *transfer)
{
    struct ykush_port *yport = transfer->user_data;

    if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
        fprintf(stderr, "Failed to flip %u (%.4x:%.4x)\n", yport->port_num, yport->vid, yport->pid);
        //Set to IDLE in case of transfer error, we will then retry up/down on
        //next timeout (or on user request)
        yport->msg_mode = IDLE;
    } else {
        //This works because what I store is the result of the comparion, which
        //is either 0 or 1
        yport->pwr_state = !yport->pwr_state;

        //We always move ON->OFF->ON. With the guard in the async handler, we
        //can never interrupt a reset "call". There is no poing setting
        //msg_state to PING again. Device is disconnected, so it will be
        //connected again and we will set flag there
        if (yport->pwr_state == POWER_ON) {
            fprintf(stdout, "YKUSH port %u is switched on again\n", yport->port_num);
            yport->msg_mode = IDLE;
        } else {
            usb_helpers_start_timeout((struct usb_port*) yport);
        }
    }
}

static void ykush_update_port(struct usb_port *port)
{
    struct ykush_port *yport = (struct ykush_port*) port;
    struct ykush_hub *yhub = (struct ykush_hub*) port->parent;
    uint8_t port_cmd = 0;
    struct libusb_transfer *transfer;

    //Prevent resetting port multiple times. msg_mode is only set to reset when
    //we call this function, and unset when a reset is done. This guard is
    //needed when we add support for async reset notifications
    //TODO: This guard need to be in the async handler!
    /*if (yport->msg_mode == RESET)
        return;*/

    yport->msg_mode = RESET;

    //For asynchrnous resets (i.e., resets triggered by something else than our
    //code), we might get a reset notification while waiting to send the net
    //ping. We therefore need to make sure the port is removed from the timeout
    //list, since it does not make sense to try to send ping while resetting
    //device.
    if (yport->timeout_next.le_next != NULL ||
        yport->timeout_next.le_prev != NULL) {
            fprintf(stdout, "Will delete:\n");
            ykush_print_port((struct usb_port*) yport);
            usb_monitor_del_timeout((struct usb_port*) yport);
    }

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
        fprintf(stderr, "Unknown port, aborting\n");
        return;
    }

    if (!yport->pwr_state)
        port_cmd |= 0x10;

    yport->buf[0] = yport->buf[1] = port_cmd;

    //Here I need to set my expected modei, to start handling errors

    fprintf(stdout, "Will send 0x%.2x to %u (%u) \n", port_cmd, yport->port_num, yport->pwr_state);

    //Follow the steps of the libusb async manual
    transfer = libusb_alloc_transfer(0);

    if (transfer == NULL) {
        fprintf(stderr, "Could not allocate trasnfer\n");
        usb_helpers_start_timeout(port);
        return;
    }

    //Use flags to save us from adding som basic logic
    transfer->flags = LIBUSB_TRANSFER_SHORT_NOT_OK |
                      LIBUSB_TRANSFER_FREE_TRANSFER;

    libusb_fill_interrupt_transfer(transfer,
                                   yhub->comm_handle,
                                   0x01,
                                   yport->buf,
                                   sizeof(yport->buf),
                                   ykush_reset_cb,
                                   port,
                                   5000);

    if (libusb_submit_transfer(transfer)) {
        fprintf(stderr, "Failed to submit transfer\n");
        libusb_free_transfer(transfer);
        usb_helpers_start_timeout(port);
        return;
    }
}

static void ykush_ping_cb(struct libusb_transfer *transfer)
{
    struct ykush_port *yport = transfer->user_data;

    //With asynchrnous reset requests, we might be waiting for a "ping" reply
    //when reset is requested. If this happens and reply arrives before device
    //is removed, ignore ping reply
    if (yport->msg_mode != PING)
        return;

    if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
        fprintf(stderr, "Ping failed for %.4x:%.4x\n", yport->vid, yport->pid);
        yport->num_retrans++;

        if (yport->num_retrans == USB_RETRANS_LIMIT) {
            ykush_update_port((struct usb_port*) yport);
            return;
        }
    } else {
        yport->num_retrans = 0;
    }

    //ykush_print_port((struct usb_port*) yport);
    //We can only get into this function after timeout has been handeled and
    //removed from timeout list. It is therefore safe to add the port to the
    //timeout list again
    usb_helpers_start_timeout((struct usb_port*) yport);
}

//TODO: Make function generic, only custom argument is the callback
static void ykush_send_ping(struct ykush_port *yport)
{
    struct libusb_transfer *transfer;

    //No multiple call guard is needed here. Ping can only be called from
    //timeout
    if (yport->dev_handle == NULL) {
        if (libusb_open(yport->dev, &(yport->dev_handle))) {
            fprintf(stderr, "Failed to open handle\n");
            usb_helpers_start_timeout((struct usb_port*) yport);
            return;
        }
    }

    //Follow the steps of the libusb async manual
    transfer = libusb_alloc_transfer(0);

    if (transfer == NULL) {
        fprintf(stderr, "Could not allocate trasnfer\n");
        usb_helpers_start_timeout((struct usb_port*) yport);
        return;
    }

    //Use flags to save us from adding som basic logic
    transfer->flags = LIBUSB_TRANSFER_SHORT_NOT_OK |
                      LIBUSB_TRANSFER_FREE_TRANSFER;

    libusb_fill_control_setup(yport->ping_buf,
                              0x80,
                              0x00,
                              0x00,
                              0x00,
                              0x02);

    libusb_fill_control_transfer(transfer,
                                 yport->dev_handle,
                                 yport->ping_buf,
                                 ykush_ping_cb,
                                 yport,
                                 5000);

    if (libusb_submit_transfer(transfer)) {
        fprintf(stderr, "Failed to submit transfer\n");
        libusb_free_transfer(transfer);
        usb_helpers_start_timeout((struct usb_port*) yport);
        return;
    }
}

static void ykush_handle_timeout(struct usb_port *port)
{
    if (port->msg_mode == PING)
        ykush_send_ping((struct ykush_port*) port);
    else
        ykush_update_port(port);
}

static uint8_t ykush_configure_hub(struct ykush_hub *yhub)
{
    uint8_t num_ports = usb_helpers_get_num_ports(yhub->hub_dev);
    uint8_t i;
    uint8_t comm_path[8];
    int32_t num_port_numbers = 0;

    if (!num_ports)
        return 0;
   
    //The HID device occupies on port on hub device
    num_ports -= 1;

    if (num_ports != MAX_YKUSH_PORTS) {
        fprintf(stderr, "YKUSH hub with odd number of ports %u\n", num_ports);
        return 0;
    }

    //Set up com device
    //TODO: Error handling!
    libusb_open(yhub->comm_dev, &(yhub->comm_handle));
    libusb_detach_kernel_driver(yhub->comm_handle, 0);
    libusb_set_configuration(yhub->comm_handle, 1);
    libusb_claim_interface(yhub->comm_handle, 0);

    yhub->num_ports = num_ports;

    comm_path[0] = libusb_get_bus_number(yhub->comm_dev);
    num_port_numbers = libusb_get_port_numbers(yhub->comm_dev,
                                               comm_path + 1,
                                               sizeof(comm_path) - 1);
    for (i = 0; i < yhub->num_ports; i++) {
        //PORT_NO_DEV_CONNECTED is 0, so no need setting it
        memset(&(yhub->port[i]), 0, sizeof(struct ykush_port));

        //Create path, use path of comm device, but update port number (last
        //value in path)
        memcpy(yhub->port[i].path, comm_path, num_port_numbers + 1);
        yhub->port[i].path[num_port_numbers] = i + 1;
        yhub->port[i].path_len = num_port_numbers + 1;
        yhub->port[i].port_num = i + 1;
        yhub->port[i].pwr_state = POWER_ON;
        yhub->port[i].parent = (struct usb_hub*) yhub;
        yhub->port[i].output = ykush_print_port;
        yhub->port[i].update = ykush_update_port;
        yhub->port[i].timeout = ykush_handle_timeout;
        
        //Insert port into global list
        usb_monitor_add_port(yhub->ctx, (struct usb_port*) &(yhub->port[i]));
    }

    return num_ports;
}

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

    printf("Added new YKUSH hub\n");

    yhub->ctx = usbmon_ctx;
    yhub->hub_dev = parent;
    yhub->comm_dev = device;

    //TODO: Check error code
    if (!ykush_configure_hub(yhub)) {
        libusb_unref_device(yhub->hub_dev);
        libusb_unref_device(yhub->comm_dev);
        free(yhub);
    } else {
        usb_monitor_add_hub(yhub->ctx, (struct usb_hub*) yhub);
    }
}

static void ykush_del_device(libusb_context *ctx, libusb_device *device,
                             void *user_data)
{
    struct usb_monitor_ctx *usbmon_ctx = user_data;
    struct libusb_device *parent = libusb_get_parent(device);
    struct ykush_hub *yhub = (struct ykush_hub*)
                             usb_monitor_find_hub(usbmon_ctx, parent);
    uint8_t i;

    if (yhub == NULL) {
        fprintf(stderr, "Hub not on list\n");
        return;
    }

    fprintf(stderr, "Will remove YKUSH hub\n");

    libusb_release_interface(yhub->comm_handle, 0);
    libusb_close(yhub->comm_handle);
    libusb_unref_device(yhub->hub_dev);
    libusb_unref_device(yhub->comm_dev);

    for (i = 0; i < yhub->num_ports; i++)
        usb_monitor_del_port((struct usb_port*) &(yhub->port[i]));

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

