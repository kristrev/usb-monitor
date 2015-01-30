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

void usb_helpers_start_timeout(struct usb_port *port, uint8_t timeout_sec)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    port->timeout_expire = ((tv.tv_sec + timeout_sec) * 1e6) + tv.tv_usec;
    usb_monitor_lists_add_timeout(port->ctx, port);
}

void usb_helpers_reset_port(struct usb_port *port)
{
    struct libusb_device_descriptor desc;

    if (port->status == PORT_DEV_CONNECTED) {
        libusb_get_device_descriptor(port->dev, &desc);
        fprintf(stdout, "Device: %.4x:%.4x removed\n", desc.idVendor, desc.idProduct);

        if (port->dev_handle)
            libusb_close(port->dev_handle);


        libusb_unref_device(port->dev);
    }

    //If we are currently resetting a device, do not remove from timeout
    //list. Otherwise, we will not send the up message. This is safe because
    //RESET depends on timeout. If a reset message fails, device is moved
    //back to IDLE. If device is then removed, it will correctly be removed
    //from timeout as well
    if (port->msg_mode != RESET &&
        (port->timeout_next.le_next != NULL ||
        port->timeout_next.le_prev != NULL)) {
        printf("Removed from timeout\n");
        usb_monitor_lists_del_timeout(port);
    }

    port->vid = 0;
    port->pid = 0;
    port->dev = NULL;
    port->dev_handle = NULL;
    port->status = PORT_NO_DEV_CONNECTED;
    port->num_retrans = 0;
}

static void usb_helpers_ping_cb(struct libusb_transfer *transfer)
{
    struct usb_port *port = transfer->user_data;

    //With asynchrnous reset requests, we might be waiting for a "ping" reply
    //when reset is requested. If this happens and reply arrives before device
    //is removed, ignore ping reply
    if (port->msg_mode != PING)
        return;

    if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
        fprintf(stderr, "Ping failed for %.4x:%.4x\n", port->vid, port->pid);
        port->num_retrans++;

        if (port->num_retrans == USB_RETRANS_LIMIT) {
            port->update(port);
            return;
        }
    } else {
        fprintf(stderr, "Ping success for %.4x:%.4x\n", port->vid, port->pid);
        port->num_retrans = 0;
    }

    //ykush_print_port((struct usb_port*) yport);
    //We can only get into this function after timeout has been handeled and
    //removed from timeout list. It is therefore safe to add the port to the
    //timeout list again
    usb_helpers_start_timeout(port, DEFAULT_TIMEOUT_SEC);
}

static int32_t usb_helpers_configure_handle(struct usb_port *port)
{
    int32_t retval = 0;

    retval = libusb_open(port->dev, &(port->dev_handle));

    if (retval) {
        fprintf(stderr, "Failed to open device, msg: %s, dev:\n", libusb_error_name(retval));
        port->output(port);
        //That we cant open device is an indication that something is wrong
        port->num_retrans++;
        usb_helpers_start_timeout(port, DEFAULT_TIMEOUT_SEC);
    }

    //According my understanding, a control transfer does not go to a specific
    //endpoing. Thus, there is no need to claim the interface or do anything
    //with the driver

    return retval;
}

void usb_helpers_send_ping(struct usb_port *port)
{
    struct libusb_transfer *transfer;

    if (port->dev_handle == NULL)
        if (usb_helpers_configure_handle(port))
            return;

    transfer = libusb_alloc_transfer(0);

    if (transfer == NULL) {
        fprintf(stderr, "Could not allocate transfer for:\n");
        port->output(port);
        usb_helpers_start_timeout(port, DEFAULT_TIMEOUT_SEC);
        return;
    }

    //Use flags to save us from adding som basic logic
    transfer->flags = LIBUSB_TRANSFER_SHORT_NOT_OK |
                      LIBUSB_TRANSFER_FREE_TRANSFER;

    libusb_fill_control_setup(port->ping_buf,
                              0x80,
                              0x00,
                              0x00,
                              0x00,
                              0x02);

    libusb_fill_control_transfer(transfer,
                                 port->dev_handle,
                                 port->ping_buf,
                                 usb_helpers_ping_cb,
                                 port,
                                 5000);

    if (libusb_submit_transfer(transfer)) {
        fprintf(stderr, "Failed to submit transfer\n");
        libusb_free_transfer(transfer);
        libusb_release_interface(port->dev_handle, 0);
        libusb_close(port->dev_handle);
        port->dev_handle = NULL;
        usb_helpers_start_timeout(port, DEFAULT_TIMEOUT_SEC);
        return;
    }
}

void usb_helpers_check_devices(struct usb_monitor_ctx *ctx)
{
    ssize_t cnt, i;
    libusb_device **list, *dev;

    //TODO: Make helper function
    //Whenever we add a hub, we also need to iterate through the list of devices
    //and see if we are aware of any that are connected
    cnt = libusb_get_device_list(NULL, &list);

    if (cnt < 0) {
        fprintf(stderr, "Failed to get device list\n");
        //This function is also called from timeout, so if we ever fail to get
        //list, we will just try again later
        return;
    }

    for (i = 0; i<cnt; i++) {
        dev = list[i];
        usb_device_added(ctx, dev);
    }

    libusb_free_device_list(list, 1);

}
