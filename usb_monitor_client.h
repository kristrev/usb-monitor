#ifndef USB_MONITOR_CLIENT
#define USB_MONITOR_CLIENT

#include <stdint.h>

#include "backend_event_loop.h"
#include "usb_monitor.h"
#include "http_parser.h"

#define MAX_REQUEST_SIZE 4096


struct http_client {
    char recv_buf[MAX_REQUEST_SIZE];
    const char *body_offset;
    struct http_parser parser;
    struct http_parser_settings parser_settings;
    struct backend_epoll_handle handle;
    struct usb_monitor_ctx *ctx;
    int32_t fd;
    uint16_t recv_progress;
    uint8_t req_done;
    uint8_t idx;
};

int usb_monitor_client_on_body(struct http_parser *parser, const char *at,
                               size_t length);
int usb_monitor_client_on_complete(struct http_parser *parser);
void usb_monitor_client_cb(void *ptr, int32_t fd, uint32_t events);
#endif
