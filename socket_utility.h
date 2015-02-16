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

#ifndef SOCKET_UTILITY_H
#define SOCKET_UTILITY_H

#include <stdint.h>

//This is the number of connections that can be in the wait queue
#define PENDING_CONNECTIONS 10

//TODO: Use logging macros instead of printf/perror/...
int32_t socket_utility_create_unix_socket(int32_t type, int32_t protocol,
                                          char *path_name, uint8_t listen_sock);

//Right now this is only a wrapper for send(), but it will potentially be more
//advanced as we switch to non-blocking sockets
int32_t socket_utility_send(int32_t socket, void *buf, uint32_t buflen);

#endif
