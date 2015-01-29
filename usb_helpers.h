#ifndef USB_HELPERS_H
#define USB_HELPERS_H

#include <stdint.h>
#include <libusb-1.0/libusb.h>

struct usb_port;
struct usb_monitor_ctx;

struct hub_descriptor {
    uint8_t bDescLength; // descriptor length
    uint8_t bDescriptorType; // descriptor type
    uint8_t bNbrPorts; // number of ports a hub equiped with

    struct {
        uint16_t LogPwrSwitchMode : 2;
        uint16_t CompoundDevice : 1;
        uint16_t OverCurrentProtectMode : 2;
        uint16_t TTThinkTime : 2;
        uint16_t PortIndicatorsSupported : 1;
        uint16_t Reserved : 8;
    } __attribute__((packed));

    uint8_t bPwrOn2PwrGood;
    uint8_t bHubContrCurrent;
} __attribute__((packed));

uint8_t usb_helpers_get_num_ports(libusb_device *hub_device);
//Generic function for starting a timer
void usb_helpers_start_timeout(struct usb_port *port, uint8_t timeout_sec);

//Reset a usb_port struct, close handle, etc.
void usb_helpers_reset_port(struct usb_port *port);

//Sending ping is generic
void usb_helpers_send_ping(struct usb_port *port);

//Iterate through devices and call add for each of them. Used in case hub config
//fails, for example
void usb_helpers_check_devices(struct usb_monitor_ctx *ctx);
#endif
