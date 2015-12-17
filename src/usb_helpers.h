#ifndef USB_HELPERS_H
#define USB_HELPERS_H

#include <stdint.h>
#include <libusb-1.0/libusb.h>

#define MAX_USB_PATH 32 //(255-)*7 + (255) + \0

struct usb_port;
struct usb_hub;
struct usb_monitor_ctx;

//Use struct from uapi/usb/ch9.h instead
struct hub_descriptor {
    uint8_t bDescLength; // descriptor length
    uint8_t bDescriptorType; // descriptor type
    uint8_t bNbrPorts; // number of ports a hub equiped with
    uint16_t wHubCharacteristics;
    uint8_t bPwrOn2PwrGood;
    uint8_t bHubContrCurrent;
} __attribute__((packed));

//Configure the generic components of a port
uint8_t usb_helpers_configure_port(struct usb_port *port,
                                   struct usb_monitor_ctx *ctx,
                                   const char *path, uint8_t path_len,
                                   uint8_t port_num, struct usb_hub *parent);

//Release memory contained in the generic part of usb port
void usb_helpers_release_port(struct usb_port *port);

//Generic output function for a usb port
void usb_helpers_print_port(struct usb_port *port, const char *type);

//Get the per-port power switching value
int8_t usb_helpers_get_power_switch(struct usb_monitor_ctx *ctx,
                                    libusb_device *hub_device, uint16_t usb_ver);

//Get the number of ports for one hub
uint8_t usb_helpers_get_num_ports(struct usb_monitor_ctx *ctx,
                                  libusb_device *hub_device, uint16_t usb_ver);

//Generic function for starting a timer
void usb_helpers_start_timeout(struct usb_port *port, uint8_t timeout_sec);

//Reset a usb_port struct, close handle, etc.
void usb_helpers_reset_port(struct usb_port *port);

//Sending ping is generic
void usb_helpers_send_ping(struct usb_port *port);

//Iterate through devices and call add for each of them. Used in case hub config
//fails, for example
void usb_helpers_check_devices(struct usb_monitor_ctx *ctx);

//Fills path with path from dev and sets path_len. Assumes path is large enough
//to store the path
void usb_helpers_fill_port_array(struct libusb_device *dev, uint8_t *path,
                                 uint8_t *path_len);

//Reset ports. If forced is set, then restart thos with a device connected
//as well
void usb_helpers_reset_all_ports(struct usb_monitor_ctx *ctx, uint8_t forced);

//Writes the path of port to output buffer, length is store in output_len
void usb_helpers_convert_path_char(struct usb_port *port, char *output,
                                   uint8_t* output_len);

//Converts path_str to a path-array. Returns 0 on success, 1 on failure
uint8_t usb_helpers_convert_char_to_path(char *path_str, uint8_t *path,
                                         uint8_t *path_len);
#endif
