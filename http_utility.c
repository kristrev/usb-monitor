#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "http_utility.h"
#include "usb_monitor_client.h"

int32_t insert_http_header(uint8_t major, uint8_t minor, char *format,
                           char *buf, size_t bufsize)
{
	return snprintf(buf, bufsize, format, major, minor);
}

int32_t insert_http_header_code(uint8_t major, uint8_t minor, char *format,
                                char *buf, size_t bufsize, uint16_t code)
{
	return snprintf(buf, bufsize, format, major, minor, code);

}
