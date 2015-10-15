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
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <linux/rtnetlink.h>
#include <libmnl/libmnl.h>

#include "socket_utility.h"

//TODO: Move functions to common library
//TODO: Use logging macros instead of printf/perror/...
int32_t socket_utility_create_unix_socket(int32_t type, int32_t protocol,
                                          char *path_name, uint8_t listen_sock,
                                          gid_t group_id)
{
	int32_t sock_fd;
	struct sockaddr_un local_addr;

	memset(&local_addr, 0, sizeof(local_addr));

	//It would have been nice to use the Linux abstract naming space and avoid
	//any file system issues (like having to use remove), but no go with nginx
	if (remove(path_name) == -1 && errno != ENOENT) {
		perror("remove");
		return -1;
	}

	if ((sock_fd = socket(AF_UNIX, type, protocol)) == -1) {
		perror("socket");
		return -1;
	}

	local_addr.sun_family = AF_UNIX;
	//The -1, combined with the memeset, is to ensure that the the string in
	//sun_addr is zero terminated.
	strncpy(local_addr.sun_path, path_name,
			sizeof(local_addr.sun_path) - 2);

	if (bind(sock_fd, (struct sockaddr*) &local_addr,
				sizeof(local_addr)) == -1) {
		perror("bind");
		close(sock_fd);
		return -1;
	}

	
    //I wanted to use fchmod, but it does not work as intended. There are
    //problems with it and fchmod
    if (chmod(path_name, S_IRWXU | S_IRGRP | S_IWGRP) == -1){
        perror("chmod");
        return -1;
    }

    if (chown(path_name, 0, group_id))
    {
        perror("chown");
        return -1;
    }

	if (listen_sock) 
		if (listen(sock_fd, PENDING_CONNECTIONS) == -1) {
			perror("listen");
			close(sock_fd);
			return -1;
		}

	return sock_fd;
}

int32_t socket_utility_send(int32_t socket, void *buf, uint32_t buflen)
{
	//Filter out the SIGPIPE-signal (we handle -1 instead)
	return send(socket, buf, buflen, MSG_NOSIGNAL);
}
