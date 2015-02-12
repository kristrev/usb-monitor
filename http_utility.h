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

int32_t insert_http_header_code(uint8_t major, uint8_t minor, char *format,
                                char *buf, size_t bufsize, uint16_t code);
#endif
