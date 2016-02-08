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

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <json-c/json.h>

#include "usb_monitor_client.h"
#include "usb_logging.h"
#include "http_parser.h"
#include "usb_helpers.h"
#include "http_utility.h"
#include "socket_utility.h"
#include "usb_monitor_lists.h"
#include "usb_monitor.h"

static void usb_monitor_client_send_code(struct http_client *client,
                                         uint16_t code)
{
    int32_t actual_hdr_len = 0;
    char hdr_buf[HTTP_REPLY_HEADER_MAX_LEN];

    actual_hdr_len = insert_http_header_code(1, 0, HTTP_REPLY_ERROR, hdr_buf,
                                             HTTP_REPLY_HEADER_MAX_LEN,
                                             code);
    //For now we don't care if we fail here or not, client should have code to
    //also handle socket closing without any data
    socket_utility_send(client->fd, (void*) hdr_buf, actual_hdr_len);
}

static uint8_t usb_monitor_client_add_paths_json(struct json_object *path_array,
        struct usb_port *port, uint8_t path_idx)
{
    char path_buf[MAX_USB_PATH];
    uint8_t path_buf_len = 0;
    struct json_object *path = json_object_new_object(), *obj_add = NULL;

    if (port == NULL)
        return 1;

    json_object_array_add(path_array, path);

    //mode (IDLE, PING, RESET)
    obj_add = json_object_new_int(port->msg_mode);
    if (obj_add == NULL)
        return 1;
    else
        json_object_object_add(path, "mode", obj_add);

    //vid/pid
    obj_add = json_object_new_int(port->u.vp.vid);
    if (obj_add == NULL)
        return 1;
    else
        json_object_object_add(path, "vid", obj_add);

    obj_add = json_object_new_int(port->u.vp.pid);
    if (obj_add == NULL)
        return 1;
    else
        json_object_object_add(path, "pid", obj_add);

    usb_helpers_convert_path_char(port, path_buf, &path_buf_len, path_idx);
    obj_add = json_object_new_string_len(path_buf, path_buf_len);

    if (obj_add == NULL)
        return 1;
    else
        json_object_object_add(path, "path", obj_add);

    return 0;
}

static json_object *usb_monitor_client_get_json(struct usb_monitor_ctx *ctx)
{
    struct usb_port *itr;
    uint8_t i = 0;
    struct json_object *json_ports = json_object_new_object();
    struct json_object *ports_array;

    if (json_ports == NULL)
        return NULL;

    ports_array = json_object_new_array();

    if (ports_array == NULL) {
        json_object_put(json_ports);
        return NULL;
    }

    //The reason for doing the add as soon as possible, is to reduce the number
    //of put-calls in case of error
    json_object_object_add(json_ports, "ports", ports_array);

    LIST_FOREACH(itr, &(ctx->port_list), port_next) {
        //Only return ports that have devices connected. Other ports will be
        //reset by usb_monitor, so no need to for input
        if (itr->status != PORT_DEV_CONNECTED)
            continue;

        for (i = 0; i < MAX_NUM_PATHS; i++) {
            if (!itr->path[i])
                break;

            if (usb_monitor_client_add_paths_json(ports_array, itr, i)) {
                json_object_put(json_ports);
                return NULL;
            }
        }
    }

    return json_ports;
}

static void usb_monitor_client_handle_get(struct http_client *client)
{
    char hdr_buf[HTTP_REPLY_HEADER_MAX_LEN];
    int32_t actual_hdr_len = 0;

    const char *json_str = NULL;
    struct json_object *json_ports = usb_monitor_client_get_json(client->ctx);

    if (json_ports == NULL) {
        //Internal server error
        usb_monitor_client_send_code(client, 500);
        return;
    }

    json_str = json_object_to_json_string_ext(json_ports,
                                              JSON_C_TO_STRING_PLAIN);

    actual_hdr_len = insert_http_header(1, 0, HTTP_OK_HEADER, hdr_buf,
                                        sizeof(hdr_buf));
    if (socket_utility_send(client->fd, (void*) hdr_buf, actual_hdr_len) > 0)
        socket_utility_send(client->fd, (void*) json_str, strlen(json_str));

    json_object_put(json_ports);
}

static uint8_t usb_monitor_client_restart_ports(struct usb_monitor_ctx *ctx,
                                                struct json_object *ports)
{
    struct json_object *port;
    int len = json_object_array_length(ports);
    int i;
    const char *path = NULL;
    uint8_t cmd = CMD_RESTART;
    uint8_t dev_path[USB_PATH_MAX];
    uint8_t path_len = 0, failure = 0;
    struct usb_port *port_ptr = NULL;

    for (i = 0; i < len; i++) {
        path = NULL;
        cmd = 0;

        port = json_object_array_get_idx(ports, i);

        json_object_object_foreach(port, key, val) {
            if (!strcmp(key, "path"))
                path = json_object_get_string(val);
            else if (!strcmp(key, "cmd"))
                //We currently ignore cmd for now
                cmd = (uint8_t) json_object_get_int(val);
        }

        if (path == NULL || cmd > CMD_DISABLE) {
            failure = 1;
            break;
        }

        //We do update path, which is const. But this is ok, as we do not expand
        //its size and it will not be used again
        if (usb_helpers_convert_char_to_path((char*) path, dev_path, &path_len)) {
            failure = 1;
            break;
        }

        port_ptr = usb_monitor_lists_find_port_path(ctx, dev_path, path_len);

        //Silently discard this error. A device can for have been disconnect
        //before request is processed
        if (port_ptr == NULL)
            continue;

        if (cmd == CMD_RESTART) {
            //If a device is being reset, do nothing. This is OK, there is no point
            //queueing up reset requests
            if (port_ptr->msg_mode == RESET)
                continue;

            USB_DEBUG_PRINT_SYSLOG(ctx, LOG_INFO, "Requested to restart:\n");
        } else if (cmd == CMD_ENABLE) {
            USB_DEBUG_PRINT_SYSLOG(ctx, LOG_INFO, "Requested to enable:\n");
        } else {
            USB_DEBUG_PRINT_SYSLOG(ctx, LOG_INFO, "Requested to disable:\n");
        }

        port_ptr->output(port_ptr);

        //TODO: Update failure based on return value from update-function
        port_ptr->update(port_ptr, cmd);

    }

    return failure;
}

static void usb_monitor_client_handle_post(struct http_client *client)
{
    json_object *json_obj, *ports = NULL;
    char hdr_buf[HTTP_REPLY_HEADER_MAX_LEN];
    int32_t actual_hdr_len = 0;
    uint8_t retval = 0;
    const char *json_str = NULL;

    if (client->body_offset == NULL) {
        //Bad request
        usb_monitor_client_send_code(client, 400);
        return;
    }

    json_obj = json_tokener_parse(client->body_offset);

    if (json_obj == NULL) {
        usb_monitor_client_send_code(client, 400);
        return;
    }

    json_object_object_foreach(json_obj, key, val) {
        if (strcmp(key, "ports")) {
            ports = NULL;
            break;
        } else {
            ports = val;
            break;
        }
    }

    if (ports == NULL) {
        usb_monitor_client_send_code(client, 400);
        json_object_put(json_obj);
        return;
    }

    if (json_object_get_type(ports) != json_type_array) {
        usb_monitor_client_send_code(client, 400);
        json_object_put(json_obj);
        return;
    }

    retval = usb_monitor_client_restart_ports(client->ctx, ports);
    json_object_put(json_obj); 

    if (retval) {
        usb_monitor_client_send_code(client, 400);
    } else {
        actual_hdr_len = insert_http_header(1, 0, HTTP_OK_HEADER, hdr_buf,
                                            sizeof(hdr_buf));
        socket_utility_send(client->fd, (void*) hdr_buf, actual_hdr_len);

        //Also try to include port info in post reply
        ports = usb_monitor_client_get_json(client->ctx);
        if (ports != NULL) {
            json_str = json_object_to_json_string_ext(ports,
                                                      JSON_C_TO_STRING_PLAIN);
            socket_utility_send(client->fd, (void*) json_str, strlen(json_str));
            json_object_put(ports);
        }
    }
}

int usb_monitor_client_on_body(struct http_parser *parser, const char *at,
                               size_t length)
{
    struct http_client *client = parser->data;

    if (client->body_offset == NULL)
        client->body_offset = at;
    return 0;
}

int usb_monitor_client_on_complete(struct http_parser *parser)
{
    struct http_client *client = parser->data;

    if (parser->method == HTTP_GET)
        usb_monitor_client_handle_get(client);
    else if (parser->method == HTTP_POST)
        usb_monitor_client_handle_post(client);
    else
        usb_monitor_client_send_code(client, 405);

    //No matter if one of the requests fail or not, we are done processing it
    client->req_done = 1;
    return 0;
}

static void usb_monitor_client_close(struct http_client *client)
{
    close(client->fd);
    client->ctx->clients_map ^= (1 << client->idx);
}

void usb_monitor_client_cb(void *ptr, int32_t fd, uint32_t events)
{
    struct http_client *client = ptr;
    int32_t numbytes = 0, numbytes_parsed = 0;

    numbytes = recv(client->fd, client->recv_buf + client->recv_progress, 
                    MAX_REQUEST_SIZE - client->recv_progress, 0);

    if (numbytes <= 0) {
        usb_monitor_client_close(client);
        return;
    }

    client->recv_progress += numbytes;
    numbytes_parsed = http_parser_execute(&(client->parser),
                                          &(client->parser_settings),
                                          client->recv_buf,
                                          numbytes);

    if (numbytes_parsed != numbytes || client->parser.http_errno != HPE_OK)
        usb_monitor_client_close(client);
    else if (client->recv_progress == MAX_REQUEST_SIZE && !client->req_done)
        usb_monitor_client_close(client);
    else if (client->req_done)
        usb_monitor_client_close(client);
}

