#ifndef USB_MONITOR_CALLBACKS_H
#define USB_MONITOR_CALLBACKS_H

#include <libusb-1.0/libusb.h>

int usb_monitor_cb(libusb_context *ctx, libusb_device *device,
                          libusb_hotplug_event event, void *user_data);

void usb_monitor_usb_event_cb(void *ptr, int32_t fd, uint32_t events);
void usb_monitor_check_devices_cb(void *ptr);
void usb_monitor_check_reset_cb(void *ptr);
void usb_monitor_itr_cb(void *ptr);
void usb_monitor_libusb_fd_add(int fd, short events, void *data);
void usb_monitor_libusb_fd_remove(int fd, void *data);

#endif
