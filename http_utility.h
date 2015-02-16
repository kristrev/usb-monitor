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

#ifndef HTTP_UTILITY_H
#define HTTP_UTILITY_H

#include <stdint.h>

//This assumes that the clients we are talking with a well-behaved and send
//correctly formatted HTTP requests. This will break.
//TODO: Make more robust
#define CONTENT_LENGTH "Content-Length: "

//This constant should match what is stored in the nginx
//large_client_header_buffers variable
//TODO: Need to set upload limit too
#define HTTP_REQUEST_SIZE	4096

#define HTTP_OK_HEADER "HTTP/%u.%u 200 OK\r\n" \
	"Content-Type: application/json\r\n" \
	"Connection: close\r\n" \
	"\r\n"
	//"Content-Length: %u\r\n"

#define HTTP_REPLY_ERROR "HTTP/%u.%u %u\r\n" \
    "Connection: close\r\n" \
    "\r\n"

//We waste some memory, but we are on the safe side
#define HTTP_REPLY_HEADER_MAX_LEN 4096

struct http_client;

//Insert an http header into buffer buf, returning the length of the header.
//Reads version from client
//Returns zero in case of failure, this is currently only triggered if version
//is wrong. Otherwise, snprintf return value
int32_t insert_http_header(uint8_t major, uint8_t minor, char *format, 
                           char *buf, size_t bufsize);

//Helper for inserting a header containing code different than 200
int32_t insert_http_header_code(uint8_t major, uint8_t minor, char *format,
                                char *buf, size_t bufsize, uint16_t code);
#endif
