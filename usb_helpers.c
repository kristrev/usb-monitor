#include <stdio.h>
#include <string.h>

#include "usb_helpers.h"
#include "usb_monitor.h"
#include "usb_monitor_lists.h"

uint8_t usb_helpers_get_num_ports(libusb_device *hub_device)
{
    struct libusb_device_handle *hub_handle;
    struct hub_descriptor hubd;
    int retval;

    memset(&hubd, 0, sizeof(hubd));

    if (libusb_open(hub_device, &hub_handle)) {
        fprintf(stderr, "Could not create USB handle\n");
        return 0;
    }

    //This is copied from lsusb. libusb get_descriptor helper does not set
    //class, which is required.
    //TODO: This call is currently sync, consider making async if I see any
    //performance problems. However, all devices I have tested with return
    //immediatly, so this should not be a problem
    retval = libusb_control_transfer(hub_handle,
                                     LIBUSB_ENDPOINT_IN |
                                     LIBUSB_REQUEST_TYPE_CLASS |
                                     LIBUSB_RECIPIENT_DEVICE,
                                     LIBUSB_REQUEST_GET_DESCRIPTOR,
                                     LIBUSB_DT_HUB << 8,
                                     0,
                                     (unsigned char*) &hubd,
                                     (uint16_t) sizeof(hubd),
                                     5000);
    libusb_close(hub_handle);

    if (retval < 0)
        fprintf(stderr, "Failed to read hub descriptor. Error: %s\n",
                libusb_error_name(retval));
    else
        printf("Number of ports: %u\n", hubd.bNbrPorts);

    return hubd.bNbrPorts;
}

void usb_helpers_start_timeout(struct usb_port *port)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    port->timeout_expire = ((tv.tv_sec + DEFAULT_TIMEOUT_SEC) * 1e6) + tv.tv_usec;
    usb_monitor_add_timeout(port->parent->ctx, port);
}

void usb_helpers_reset_port(struct usb_port *port)
{
    struct libusb_device_descriptor desc;

    if (port->status == PORT_DEV_CONNECTED) {
        libusb_get_device_descriptor(port->dev, &desc);
        fprintf(stdout, "Device: %.4x:%.4x removed\n", desc.idVendor, desc.idProduct);

        if (port->dev_handle) {
            libusb_release_interface(port->dev_handle, 0);
            libusb_close(port->dev_handle);
        }

        //If we are currently resetting a device, do not remove from timeout
        //list. Otherwise, we will not send the up message. This is safe because
        //RESET depends on timeout. If a reset message fails, device is moved
        //back to IDLE. If device is then removed, it will correctly be removed
        //from timeout as well
        if (port->msg_mode != RESET &&
            (port->timeout_next.le_next != NULL ||
            port->timeout_next.le_prev != NULL))
            usb_monitor_del_timeout(port);

        libusb_unref_device(port->dev);
    }

    port->vid = 0;
    port->pid = 0;
    port->dev = NULL;
    port->dev_handle = NULL;
    port->status = PORT_NO_DEV_CONNECTED;
    port->num_retrans = 0;
}
