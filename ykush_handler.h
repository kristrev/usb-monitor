#ifndef YKUSH_HANDLER_H
#define YKUSH_HANDLER_H

#include "usb_monitor.h"

#include <libusb-1.0/libusb.h>

#define YKUSH_VID 0x04d8
#define YKUSH_PID 0x0042

#define YKUSH_CMD_PORT_1    0x01
#define YKUSH_CMD_PORT_2    0x02
#define YKUSH_CMD_PORT_3    0x03
#define YKUSH_CMD_ALL       0x0A

#define MAX_YKUSH_PORTS 3

struct ykush_port {
    USB_PORT_MANDATORY;
    //This is the current port number. Added for convenience, so that I dont
    //have to read from path all the time
    uint8_t port_num;
    //When doing async transfer, buffer needs to be allocated on heap
    uint8_t buf[6];
};

struct ykush_hub {
    USB_HUB_MANDATORY;
    libusb_device *comm_dev;
    libusb_device_handle *comm_handle;
    //These values are kept around in case Yepkit launches a different hub with
    //a different number of ports.
    //TODO: Consider using pointers, to reduce size of struct
    struct ykush_port port[MAX_YKUSH_PORTS];
};

//This callback is used to handle YKUSH hubs being added and removed.
//TODO: Consider using forward declare to reduce number of headers
int ykush_event_cb(libusb_context *ctx, libusb_device *device,
                    libusb_hotplug_event event, void *user_data);

#endif
