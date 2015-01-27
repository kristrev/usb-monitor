#ifndef USB_MONITOR_H
#define USB_MONITOR_H

#include <stdint.h>
#include <sys/queue.h>
#include <libusb-1.0/libusb.h>

struct usb_port;

//port function pointers
typedef void (*printPort)(struct usb_port *port);

//The device pointed to here is the device that will be used for comparison when
//new hubs are added
#define USB_HUB_MANDATORY \
    libusb_device *hub_dev; \
    LIST_ENTRY(usb_hub) hub_next

//Size of path is 8 since it is bus + max depth (7)
#define USB_PORT_MANDATORY \
    libusb_device *dev; \
    printPort output; \
    uint8_t status; \
    uint8_t path_len; \
    uint8_t path[8]; \
    LIST_ENTRY(usb_port) port_next

enum port_status {
    PORT_NO_DEV_CONNECTED = 0,
    PORT_DEV_CONNECTED,
};

struct usb_hub {
    USB_HUB_MANDATORY;
};

struct usb_port {
    USB_PORT_MANDATORY;
};

struct usb_monitor_ctx {
    LIST_HEAD(hubs, usb_hub) hub_list;
    LIST_HEAD(ports, usb_port) port_list;
};

//Searches for the hub_device in hub_list and returns 1 if found, 0 otherwise
struct usb_hub* usb_monitor_find_hub(struct usb_monitor_ctx *ctx,
                                     libusb_device *hub);

//Add hub to list/delete hub from list
void usb_monitor_add_hub(struct usb_monitor_ctx *ctx, struct usb_hub *hub);
void usb_monitor_del_hub(struct usb_hub *hub);

//Add hub to list/delete port from list
void usb_monitor_add_port(struct usb_monitor_ctx *ctx, struct usb_port *port);
void usb_monitor_del_port(struct usb_port *port);
#endif
