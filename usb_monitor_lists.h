#ifndef USB_MONITOR_LISTS_H
#define USB_MONITOR_LISTS_H

#include "usb_monitor.h"
#include <libusb-1.0/libusb.h>

//Searches for the hub_device in hub_list and returns 1 if found, 0 otherwise
struct usb_hub* usb_monitor_lists_find_hub(struct usb_monitor_ctx *ctx,
                                           libusb_device *hub);

//Add hub to list/delete hub from list
void usb_monitor_lists_add_hub(struct usb_monitor_ctx *ctx, struct usb_hub *hub);
void usb_monitor_lists_del_hub(struct usb_hub *hub);

//Add port to list/delete port from list
void usb_monitor_lists_add_port(struct usb_monitor_ctx *ctx, struct usb_port *port);
void usb_monitor_lists_del_port(struct usb_port *port);
struct usb_port *usb_monitor_lists_find_port_path(struct usb_monitor_ctx *ctx,
                                                  uint8_t *path,
                                                  uint8_t path_len);

//Add or delete port from timeout list
void usb_monitor_lists_add_timeout(struct usb_monitor_ctx *ctx, struct usb_port *port);
void usb_monitor_lists_del_timeout(struct usb_port *port);

#endif
