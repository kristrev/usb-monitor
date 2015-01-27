#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ykush_handler.h"
#include "usb_helpers.h"

/* TODO: This function is generic  */
static void ykush_print_port(struct usb_port *port)
{
    int i;
    struct libusb_device_descriptor desc;

    fprintf(stdout, "Type: YKUSH Path: ");

    for (i = 0; i < port->path_len-1; i++)
        fprintf(stdout, "%u-", port->path[i]);

    fprintf(stdout, "%u State: %u", port->path[i], port->status);

    if (port->dev) {
        libusb_get_device_descriptor(port->dev, &desc);
        fprintf(stdout, " Device: %.4x:%.4x", desc.idVendor, desc.idProduct);
    }

    fprintf(stdout, "\n");
}

static uint8_t ykush_configure_hub(struct usb_monitor_ctx *ctx,
                                   struct ykush_hub *yhub)
{
    uint8_t num_ports = usb_helpers_get_num_ports(yhub->hub_dev);
    uint8_t i, j;
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
        yhub->port[i].output = ykush_print_port;
        
        //Insert port into global list
        usb_monitor_add_port(ctx, (struct usb_port*) &(yhub->port[i]));
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

    yhub->hub_dev = parent;
    yhub->comm_dev = device;

    //TODO: Check error code
    if (!ykush_configure_hub(usbmon_ctx, yhub)) {
        libusb_unref_device(yhub->hub_dev);
        libusb_unref_device(yhub->comm_dev);
        free(yhub);
    } else {
        usb_monitor_add_hub(usbmon_ctx, (struct usb_hub*) yhub);
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

