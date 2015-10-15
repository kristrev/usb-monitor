/*
 * Copyright 2015 Kristian Evensen <kristian.evensen@gmail.com>
 *
 * This file is part of Usb Monitor. Usb Monitor is free software: you can
 * redistribute it and/or modify it under the terms of the Lesser GNU General
 * Public License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 *
 * Usb Monitor is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * Usb Monitor. If not, see http://www.gnu.org/licenses/.
 */

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

//HTTP parse callbacks for the events we are interested in
int usb_monitor_client_on_body(struct http_parser *parser, const char *at,
                               size_t length);
int usb_monitor_client_on_complete(struct http_parser *parser);

//Event loop callback (client socket)
void usb_monitor_client_cb(void *ptr, int32_t fd, uint32_t events);
#endif
