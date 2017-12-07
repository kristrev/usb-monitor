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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/epoll.h>

#include <json-c/json.h>
#include <libusb-1.0/libusb.h>

#include "usb_monitor.h"
#include "ykush_handler.h"
#include "usb_monitor_lists.h"
#include "usb_helpers.h"
#include "gpio_handler.h"
#include "generic_handler.h"
#include "usb_logging.h"
#include "backend_event_loop.h"
#include "usb_monitor_callbacks.h"
#include "socket_utility.h"
#include "usb_monitor_client.h"

//Kept global so that I can access it from the signal handler
static struct usb_monitor_ctx *usbmon_ctx = NULL;

void usb_monitor_print_ports(struct usb_monitor_ctx *ctx)
{
    struct usb_port *itr;

    LIST_FOREACH(itr, &(ctx->port_list), port_next)
        itr->output(itr);

    fprintf(ctx->logfile, "\n");
}


static uint8_t usb_monitor_parse_handlers(struct usb_monitor_ctx *ctx,
                                          struct json_object *handlers)
{
    int handlers_len = 0, i;
    uint8_t unknown_elem = 0;
    const char *handler_name = NULL;
    struct json_object *arr_obj, *handler_obj = NULL;
    
    handlers_len = json_object_array_length(handlers);

    for (i = 0; i < handlers_len; i++) {
        handler_name = NULL;
        handler_obj = NULL;

        arr_obj = json_object_array_get_idx(handlers, i);

        json_object_object_foreach(arr_obj, key, val) {
            if (!strcmp(key, "name")) {
                handler_name = json_object_get_string(val);
                continue;
            } else if(!strcmp(key, "ports")) {
                handler_obj = val;
                continue;
            } else {
                unknown_elem = 1;
                break;
            }
        }

        if (handler_name == NULL || handler_obj == NULL || unknown_elem) {
            fprintf(stderr, "Incorrect handler object found in JSON\n");
            return 1;
        }

        if (!strcmp("GPIO", handler_name)) {
            if (gpio_handler_parse_json(ctx, handler_obj))
                return 1;
        } else {
            fprintf(stderr, "Unknown handler in JSON\n");
            return 1;
        }
    }

    return 0;
}

static uint8_t usb_monitor_parse_bad_vid_pids(struct usb_monitor_ctx *ctx,
                                              struct json_object *bad_vid_pids)
{
    int num_bad_vid_pids = json_object_array_length(bad_vid_pids);
    int i;
    uint16_t vid, pid;
    struct json_object *bad_vid_pid;

    if (!(ctx->bad_devices = calloc(sizeof(struct usb_bad_device) *
                                    num_bad_vid_pids, 1))) {
        fprintf(stderr, "Could not allocate bad devices memory\n");
        return 1;
    }

    for (i = 0; i < num_bad_vid_pids; i++) {
        bad_vid_pid = json_object_array_get_idx(bad_vid_pids, i);
        vid = pid = 0;

        if (json_object_get_type(bad_vid_pid) != json_type_object) {
            fprintf(stderr, "Array element has incorrect type (not obj.)\n");
            return 1;
        }

        json_object_object_foreach(bad_vid_pid, key, val) {
            if (json_object_get_type(val) != json_type_int) {
                fprintf(stderr, "Incorrect object found in array\n");
                return 1;
            }

            if (!strcmp("vid", key)) {
                vid = json_object_get_int(val);
            } else if (!strcmp("pid", key)) {
                pid = json_object_get_int(val);
            } else {
                fprintf(stderr, "Unknown key found\n");
                return 1;
            }
        }

        //Add wildcard support (for pid) later
        if (!vid || !pid) {
            fprintf(stderr, "vid/pid is not set\n");
            return 1;
        }

        ctx->bad_devices[i].vid = vid;
        ctx->bad_devices[i].pid = pid;
    }

    return 0;
}

//Return 0 on success, 1 on failure
static uint8_t usb_monitor_parse_config(struct usb_monitor_ctx *ctx,
                                        const char *config_file_name)
{
    //Limit the number of bytes we read from file
    char buf[1024];
    FILE *conf_file;
    //TODO: Clean up a bit here
    struct json_object *conf_json;
    int retval = 0;

    memset(buf, 0, sizeof(buf));
    conf_file = fopen(config_file_name, "re");

    if (conf_file == NULL) {
        fprintf(stderr, "Failed to open config file\n");
        return 1;
    }

    retval = fread(buf, 1, 1024, conf_file);

    if (retval != sizeof(buf) &&
        ferror(conf_file)) {
        fprintf(stderr, "Failed to read from config file\n");
        fclose(conf_file);
        return 1;
    } else {
        fclose(conf_file);
    }

    //Parse JSON
    conf_json = json_tokener_parse(buf);

    if (conf_json == NULL) {
        fprintf(stderr, "Failed to parse JSON\n");
        return 1;
    }

    retval = 0;

    json_object_object_foreach(conf_json, key, val) {
        if (!strcmp("handlers", key)) {
            if (json_object_get_type(val) != json_type_array) {
                fprintf(stderr, "handlers object is of incorrect type");
                retval = 1;
                break;
            }

            if ((retval = usb_monitor_parse_handlers(ctx, val))) {
                break;
            }
        } else if (!strcmp("disable_auto_restart", key)) {
            if (json_object_get_type(val) != json_type_boolean) {
                fprintf(stderr, "disable_auto_reset is of incorect type");
                retval = 1;
                break;
            } else {
                ctx->disable_auto_restart = json_object_get_boolean(val);
            }
        } else if (!strcmp("bad_vid_pids", key)) {
            if (json_object_get_type(val) != json_type_array) {
                fprintf(stderr, "bad_vid_pids is of incorrect type");
                retval = 1;
                break;
            }

            if ((retval = usb_monitor_parse_bad_vid_pids(ctx, val))) {
                break;
            }
        }
    }

    json_object_put(conf_json);
    
    return retval;
}

static void usb_monitor_signal_handler(int signum)
{
    USB_DEBUG_PRINT_SYSLOG(usbmon_ctx, LOG_INFO, "Signalled to restart all ports\n");
    usb_helpers_reset_all_ports(usbmon_ctx, 1);
}

static void usb_monitor_start_event_loop(struct usb_monitor_ctx *ctx)
{
    struct timeval tv;
    uint64_t cur_time;

    gettimeofday(&tv, NULL);

    cur_time = (tv.tv_sec * 1e3) + (tv.tv_usec / 1e3);

    //These timeout pointers will live for as long as the application.
    //Therefore, there is no need to save them anywhere
    if (!backend_event_loop_add_timeout(ctx->event_loop, cur_time + 1000,
                                        usb_monitor_itr_cb, ctx, 1000))
        return;
   
    //Do not make this one a multiple of reset_cb timeout. There is no need
    //resetting and checking at the same time
    if (!backend_event_loop_add_timeout(ctx->event_loop, cur_time + 25000,
                                        usb_monitor_check_devices_cb,
                                        ctx, 25000))
        return;

    if (!ctx->disable_auto_restart &&
        !backend_event_loop_add_timeout(ctx->event_loop, cur_time + 60000,
                                        usb_monitor_check_reset_cb,
                                        ctx, 60000))
        return;

    backend_event_loop_run(ctx->event_loop);
}

static void usb_monitor_configure_client(struct usb_monitor_ctx *ctx,
                                         struct http_client *client,
                                         int32_t sock,
                                         uint8_t client_idx)
{
    memset(client, 0, sizeof(struct http_client));
    client->ctx = ctx;
    client->fd = sock;
    client->idx = client_idx;

    http_parser_init(&(client->parser), HTTP_REQUEST);
    client->parser.data = (void*) client;
    client->parser_settings.on_body = usb_monitor_client_on_body;
    client->parser_settings.on_message_complete =
        usb_monitor_client_on_complete;

    backend_configure_epoll_handle(&(client->handle), client, sock,
                                   usb_monitor_client_cb);
    backend_event_loop_update(ctx->event_loop, EPOLLIN, EPOLL_CTL_ADD, sock,
                              &(client->handle));
}

static void usb_monitor_accept_cb(void *ptr, int32_t fd, uint32_t events)
{
    struct usb_monitor_ctx *ctx = ptr;
    uint8_t client_idx = 0;
    int32_t client_sock = accept(fd, NULL, NULL);
    struct http_client *client = NULL;

    //We can't handle more connections
    if (!ctx->clients_map) {
        close(client_sock);
        return;
    }

    //Get the first set bit in map. Note the ffs returns 1 for lsb, convert to
    //0-indexed
    client_idx = ffs(ctx->clients_map) - 1;

    //Allocate clients dynamically. We never free though, but it is not a
    //problem with such a low number since we consume little memory anyways.
    //However, if we increase number of concurrent clients, some sort of garbage
    //collection will be needed
    if (ctx->clients[client_idx] == NULL) {
        client = malloc(sizeof(struct http_client));

        if (client == NULL) {
            USB_DEBUG_PRINT_SYSLOG(ctx, LOG_INFO, "Could not allocate memory for client\n");
            return;
        } else {
            ctx->clients[client_idx] = client;
        }
    } else {
        client = ctx->clients[client_idx];
    }

    ctx->clients_map ^= (1 << client_idx);

    //Start off by only supporting one concurrent client
    usb_monitor_configure_client(ctx, client, client_sock, client_idx);
    USB_DEBUG_PRINT_SYSLOG(ctx, LOG_INFO, 
            "Accepted new client with index: %u\n", client_idx);
}

static int usb_monitor_configure_unix_socket(struct usb_monitor_ctx *ctx)
{
    int32_t fd = socket_utility_create_unix_socket(SOCK_STREAM, 0,
                                                   "/tmp/usbmonitor", 1,
                                                   ctx->group_id);

    if (fd == -1)
        return fd;

    backend_configure_epoll_handle(ctx->accept_handle, ctx, fd,
                                   usb_monitor_accept_cb);
    return backend_event_loop_update(ctx->event_loop, EPOLLIN, EPOLL_CTL_ADD,
                                     fd, ctx->accept_handle);
}

static uint8_t usb_monitor_configure(struct usb_monitor_ctx *ctx, uint8_t sock)
{
    int i = 0;
    const struct libusb_pollfd **libusb_fds;
    const struct libusb_pollfd *libusb_fd;

    LIST_INIT(&(ctx->hub_list));
    LIST_INIT(&(ctx->port_list));
    LIST_INIT(&(ctx->timeout_list));

    //We handle maximum of five concurrent clients
    ctx->clients_map = 0x1F;
    ctx->event_loop = backend_event_loop_create(); 

    for (i = 0; i < MAX_HTTP_CLIENTS; i++)
        ctx->clients[i] = NULL;

    if (ctx->event_loop == NULL) {
        fprintf(stderr, "Failed to create event loop\n");
        fclose(ctx->logfile);
        return 1;
    }
   
    if (sock) {
        ctx->accept_handle = backend_create_epoll_handle(ctx, 0, NULL, 0);

        if (ctx->accept_handle == NULL) {
            fprintf(stderr, "Could not create accept handle\n");
            fclose(ctx->logfile);
            return 1;
        }

        if (usb_monitor_configure_unix_socket(ctx)) {
            fprintf(stderr, "Could not create UNIX socket\n");
            fclose(ctx->logfile);
            return 1;
        }
    }

    libusb_fds = libusb_get_pollfds(NULL);
    if (libusb_fds == NULL) {
        fprintf(stderr, "Failed to get list of libusb fds to poll\n");
        fclose(ctx->logfile);
        return 1;
    }

    ctx->libusb_handle = 
        backend_create_epoll_handle(ctx, 0, usb_monitor_usb_event_cb, 1);

    if (ctx->libusb_handle == NULL) {
        fprintf(stderr, "Failed to create epoll handle\n");
        fclose(ctx->logfile);
        return 1;
    }

    i = 0;
    libusb_fd = libusb_fds[i];
   
    //Use one handler for all file descriptors. We know that there will always
    //be at least one (USB) file descriptor we want to listen to (or one will
    //come)
    while (libusb_fd != NULL) {
        backend_event_loop_update(ctx->event_loop,
                                  libusb_fd->events,
                                  EPOLL_CTL_ADD,
                                  libusb_fd->fd,
                                  ctx->libusb_handle);
        libusb_fd = libusb_fds[++i];
    }

    free(libusb_fds);

    libusb_set_pollfd_notifiers(NULL,
                                usb_monitor_libusb_fd_add,
                                usb_monitor_libusb_fd_remove,
                                ctx);

    libusb_hotplug_register_callback(NULL,
                                     LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
                                     LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
                                     0,
                                     LIBUSB_HOTPLUG_MATCH_ANY,
                                     LIBUSB_HOTPLUG_MATCH_ANY,
                                     LIBUSB_HOTPLUG_MATCH_ANY,
                                     usb_monitor_cb,
                                     usbmon_ctx, NULL);


    //libusb documentation is a bit unclear on the interaction between event
    //loops, the libusb thread and hotplug events/async transfers. However, the
    //documentation states that if poll() is used to monitor file descriptors,
    //then the libusb event lock must be acquired. The lock shall only be
    //released when we are done handling libusb events, i.e., never
    libusb_lock_events(NULL);

    return 0;
}

static void usb_monitor_print_usage()
{
    fprintf(stdout, "usb monitor command line arguments:\n");
    fprintf(stdout, "\t-o : path to log file (optional)\n");
    fprintf(stdout, "\t-c : path to config file (optional)\n");
    fprintf(stdout, "\t-g : group id of usb monitor socket (optional)\n");
    fprintf(stdout, "\t-d : run as daemon\n");
    fprintf(stdout, "\t-s : write to syslog\n");
    fprintf(stdout, "\t-h : this output\n");
}

//TODO: Refactor this function, it is too big now
int main(int argc, char *argv[])
{
    int retval = 0;
    uint8_t daemonize = 0;
    char *conf_file_name = NULL;
    struct sigaction sig_handler;
    int32_t pid_fd;

    //We should only allow one running instance of usb_monitor
    pid_fd = open("/var/run/usb_monitor.pid", O_CREAT | O_RDWR | O_CLOEXEC, 0644);

    if (pid_fd == -1 ||
        lockf(pid_fd, F_TLOCK, 0)) {
        fprintf(stderr, "Could not open pid file\n");
        exit(EXIT_FAILURE);
    }

    memset(&sig_handler, 0, sizeof(sig_handler));

    usbmon_ctx = calloc(sizeof(struct usb_monitor_ctx), 1);

    if (usbmon_ctx == NULL) {
        fprintf(stderr, "Failed to allocated application context struct\n");
        exit(EXIT_FAILURE);
    }

    usbmon_ctx->logfile = stderr;

    while ((retval = getopt(argc, argv, "o:c:g:dhs")) != -1) {
        switch (retval) {
        case 'o':
            usbmon_ctx->logfile = fopen(optarg, "a+");
            break;
        case 'c':
            conf_file_name = optarg;
            break;
        case 'd':
            daemonize = 1;
            break;
        case 's':
            usbmon_ctx->use_syslog = 1;
            break;
        case 'g':
            usbmon_ctx->group_id = atoi(optarg);
            break;
        case 'h':
        default:
            usb_monitor_print_usage();
            exit(EXIT_SUCCESS);
        } 
    }

    if (daemonize && daemon(1,1)) {
        fprintf(stderr, "Failed to start usb-monitor as daemon\n");
        fclose(usbmon_ctx->logfile);
        libusb_exit(NULL);
        exit(EXIT_FAILURE);
    }

    if (usbmon_ctx->logfile == NULL) {
        fprintf(stderr, "Failed to create logfile\n");
        exit(EXIT_FAILURE);
    }

    if (usbmon_ctx->use_syslog)
        openlog("usb-monitor", LOG_PID | LOG_NDELAY, LOG_DAEMON); 

    retval = libusb_init(NULL);

    if (retval) {
        fprintf(stderr, "libusb failed with error %s\n",
                libusb_error_name(retval));
        fclose(usbmon_ctx->logfile);
        exit(EXIT_FAILURE);
    }

    //libusb_set_debug(NULL, LIBUSB_LOG_LEVEL_DEBUG);

    if (usb_monitor_configure(usbmon_ctx, 1))
        exit(EXIT_FAILURE);

    if (conf_file_name != NULL &&
        usb_monitor_parse_config(usbmon_ctx, conf_file_name)) {
        fprintf(stderr, "Failed to read config file\n");
        fclose(usbmon_ctx->logfile);
        exit(EXIT_FAILURE);
    }

    //Start signal handler
    sig_handler.sa_handler = usb_monitor_signal_handler;

    if (sigaction(SIGUSR1, &sig_handler, NULL)) {
        fprintf(stderr, "Could not intall signal handler\n");
        exit(EXIT_FAILURE);
    }

    usb_helpers_check_devices(usbmon_ctx);

    USB_DEBUG_PRINT_SYSLOG(usbmon_ctx, LOG_INFO, "Initial state:\n");

    usb_monitor_print_ports(usbmon_ctx);

    usb_monitor_start_event_loop(usbmon_ctx);

    libusb_exit(NULL);

    //We shall never stop
    exit(EXIT_FAILURE);
}
