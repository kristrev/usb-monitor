#include <stdio.h>
#include <string.h>

#include "usb_helpers.h"
#include "usb_monitor.h"
#include "usb_monitor_lists.h"
#include "usb_logging.h"

void usb_helpers_configure_port(struct usb_port *port,
                                struct usb_monitor_ctx *ctx,
                                uint8_t *path, uint8_t path_len,
                                uint8_t port_num)
{
        memcpy(port->path, path, path_len);
        port->path_len = path_len;
        port->port_num = port_num;
        port->pwr_state = POWER_ON;
        port->ctx = ctx;

        usb_monitor_lists_add_port(ctx, port);
}

void usb_helpers_print_port(struct usb_port *port, const char *type)
{
    int i;
    struct libusb_device_descriptor desc;

    //Generic hubs often advertise a much larger number of ports than they
    //provide. In order to avoid polluting the lists, only print ports that has
    //a device connected
    if (port->status != PORT_DEV_CONNECTED)
        return;

    USB_DEBUG_PRINT(port->ctx->logfile, "Type: %s Path: ", type);

    for (i = 0; i < port->path_len-1; i++)
        fprintf(port->ctx->logfile, "%u-", port->path[i]);

    fprintf(port->ctx->logfile, "%u State: %u Pwr: %u ", port->path[i],
            port->status, port->pwr_state);

    if (port->dev) {
        libusb_get_device_descriptor(port->dev, &desc);
        fprintf(port->ctx->logfile, " Device: %.4x:%.4x", desc.idVendor,
                desc.idProduct);
    }

    fprintf(port->ctx->logfile, "\n");
    fflush(port->ctx->logfile);
}

int8_t usb_helpers_get_power_switch(struct usb_monitor_ctx *ctx,
                                    libusb_device *hub_device, uint16_t usb_ver)
{
    struct libusb_device_handle *hub_handle;
    struct hub_descriptor hubd;
    int retval;
    uint8_t val = (usb_ver == 0x300 ? LIBUSB_DT_SUPERSPEED_HUB : LIBUSB_DT_HUB);
    uint16_t wHubChar = 0;

    memset(&hubd, 0, sizeof(hubd));

    if (libusb_open(hub_device, &hub_handle)) {
        USB_DEBUG_PRINT(ctx->logfile, "Could not create USB handle\n");
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
                                     val << 8,
                                     0,
                                     (unsigned char*) &hubd,
                                     (uint16_t) sizeof(hubd),
                                     5000);
    libusb_close(hub_handle);

    if (retval < 0)
        return -1;

    wHubChar = le16toh(hubd.wHubCharacteristics);
    //Two lsb contains the port power control support
    return wHubChar & 0x03;
}

uint8_t usb_helpers_get_num_ports(struct usb_monitor_ctx *ctx,
                                  libusb_device *hub_device, uint16_t usb_ver)
{
    struct libusb_device_handle *hub_handle;
    struct hub_descriptor hubd;
    int retval;
    uint8_t val = (usb_ver == 0x300 ? LIBUSB_DT_SUPERSPEED_HUB : LIBUSB_DT_HUB);

    memset(&hubd, 0, sizeof(hubd));

    if (libusb_open(hub_device, &hub_handle)) {
        USB_DEBUG_PRINT(ctx->logfile, "Could not create USB handle\n");
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
                                     val << 8,
                                     0,
                                     (unsigned char*) &hubd,
                                     (uint16_t) sizeof(hubd),
                                     5000);
    libusb_close(hub_handle);

    if (retval < 0) {
        USB_DEBUG_PRINT(ctx->logfile, "Failed to read hub descriptor. Error: %s\n",
                libusb_error_name(retval));
        return 0;
    } else {
        return hubd.bNbrPorts;
    }
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
        USB_DEBUG_PRINT(port->ctx->logfile, "Device: %.4x:%.4x removed\n", desc.idVendor, desc.idProduct);

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
        USB_DEBUG_PRINT(port->ctx->logfile, "Ping failed for %.4x:%.4x\n", port->vid, port->pid);
        port->num_retrans++;

        if (port->num_retrans == USB_RETRANS_LIMIT) {
            port->update(port);
            return;
        }
    } else {
        if (++port->ping_cnt == PING_OUTPUT) {
            USB_DEBUG_PRINT(port->ctx->logfile, "Ping success for %.4x:%.4x\n", port->vid, port->pid);
            port->ping_cnt = 0;
        }
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
        USB_DEBUG_PRINT(port->ctx->logfile, "Failed to open device, msg: %s, dev:\n", libusb_error_name(retval));
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
        USB_DEBUG_PRINT(port->ctx->logfile, "Could not allocate transfer for:\n");
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
        USB_DEBUG_PRINT(port->ctx->logfile, "Failed to submit transfer\n");
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
        USB_DEBUG_PRINT(ctx->logfile, "Failed to get device list\n");
        //This function is also called from timeout, so if we ever fail to get
        //list, we will just try again later
        return;
    }

    for (i = 0; i<cnt; i++) {
        dev = list[i];
        usb_monitor_cb(NULL, dev, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED, ctx);
    }

    libusb_free_device_list(list, 1);

}

void usb_helpers_fill_port_array(struct libusb_device *dev,
                                 uint8_t *path,
                                 uint8_t *path_len)
{
    path[0] = libusb_get_bus_number(dev);
    *path_len = libusb_get_port_numbers(dev, path + 1, 7);
    //We use the bus as part of path
    *path_len += 1;
}
