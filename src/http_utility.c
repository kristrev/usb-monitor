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
