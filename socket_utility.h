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
