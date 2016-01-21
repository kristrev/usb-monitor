/*
 * Copyright 2015 Kristian Evensen <kristian.evensen@gmail.com>
 *
 * This file is part of Usb Monitor. Usb Monitor is free software: you can
 * redistribute it and/or modify it under the terms of the Lesser GNU General
 * Public License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 *
 * Usb Montior is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * Usb Monitor. If not, see http://www.gnu.org/licenses/.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <json-c/json.h>

#include "gpio_handler.h"
#include "usb_monitor_lists.h"
#include "usb_helpers.h"
#include "usb_logging.h"

static void gpio_print_port(struct usb_port *port)
{
    usb_helpers_print_port(port, "GPIO");
}

static void gpio_update_port(struct usb_port *port)
{
    struct gpio_port *gport = (struct gpio_port*) port;
    uint8_t gpio_val = 0;
    //We will just write to /sys/class/gpio/gpioX/value, so no need for the full
    //4096 (upper limit according to getconf)
    char file_path[64];
    int32_t bytes_written = 0, fd = 0;

    gport->msg_mode = RESET;

    //Guard agains async reset requests. This guard is also needed for the
    //scenario where we are waiting to ping and device is reset. Then we will
    //still be in timeout list, but also reset. Since we then re-add port to
    //timeout (there is no USB timer), we create infinite loop and that is that
    if (gport->timeout_next.le_next != NULL ||
        gport->timeout_next.le_prev != NULL)
            usb_monitor_lists_del_timeout((struct usb_port*) gport);

    //POWER_OFF is 0, so then we should switch on port
    if (!gport->pwr_state)
        gpio_val = 1;

    //Do a write, if write is successful then we update power state
    snprintf(file_path, sizeof(file_path), "/sys/class/gpio/gpio%u/value", gport->port_num);
    
    fd = open(file_path, O_WRONLY | FD_CLOEXEC);

    if (fd == -1) {
        USB_DEBUG_PRINT_SYSLOG(port->ctx, LOG_ERR, "Failed to open gpio file\n");
        usb_helpers_start_timeout((struct usb_port*) gport, DEFAULT_TIMEOUT_SEC);
        return;
    }

    if (gpio_val)
        bytes_written = write(fd, "1", 1);
    else
        bytes_written = write(fd, "0", 1);

    close(fd);

    if (bytes_written > 0) {
        gport->pwr_state = !gport->pwr_state;

        if (gport->pwr_state) {
            gport->msg_mode = IDLE;
            return;
        }
    }

    //There is no error case. If we fail, then we simply sleep and try again. No
    //device to free or anything

    //Sleep no matter if write is successful or not
    usb_helpers_start_timeout((struct usb_port*) gport, GPIO_TIMEOUT_SLEEP_SEC);
   
    //TODO: How to check if we are done? If we get here, and inverse of
    //power_state is off, then we have switched? Or?
    //USB_DEBUG_PRINT(stderr, "GPIO update\n");
}

static void gpio_handle_timeout(struct usb_port *port)
{
    if (port->msg_mode == PING)
        usb_helpers_send_ping(port);
    else
        gpio_update_port(port);
}

//For GPIO, what is unique is the gpio number. A gpio number might be mapped to
//multiple paths
static struct gpio_port* gpio_handler_get_port(struct usb_monitor_ctx *ctx,
        uint8_t gpio_num)
{
    struct usb_port *itr = NULL;
    struct gpio_port *port = NULL;

    LIST_FOREACH(itr, &(ctx->port_list), port_next) {
        if (itr->port_type != PORT_TYPE_GPIO)
            continue;

        if (itr->port_num == gpio_num) {
            port = (struct gpio_port *) itr;
            break;
        }
    }

    return port;

}

static struct gpio_port* gpio_handler_create_port(struct usb_monitor_ctx *ctx,
        uint8_t gpio_num)
{
    struct gpio_port *port = calloc(sizeof(struct gpio_port), 1);

    if (!port) {
        fprintf(stderr, "Could not allocate memory for gpio port\n");
        return NULL;
    }

    port->port_type = PORT_TYPE_GPIO;
    port->output = gpio_print_port;
    port->update = gpio_update_port;
    port->timeout = gpio_handle_timeout;

    return port;
}

static uint8_t gpio_handler_add_port(struct usb_monitor_ctx *ctx,
        char *path, uint8_t gpio_num)
{
    struct gpio_port *port;

    //Bus + port(s)
    uint8_t dev_path[USB_PATH_MAX];
    const char *dev_path_ptr = (const char *) dev_path;
    uint8_t path_len = 0, do_configure = 0, retval = 0;

    if (usb_helpers_convert_char_to_path(path, dev_path, &path_len)) {
        fprintf(stderr, "Path for GPIO device is too long\n");
        return 1;
    }

    port = gpio_handler_get_port(ctx, gpio_num);

    //Port not found, create new
    if (!port) {
        port = gpio_handler_create_port(ctx, gpio_num);
        do_configure = 1;
    }

    if (!port) {
        fprintf(stderr, "Failed to allocate memory for gpio port\n");
        return 1;
    }

    //Update path
    if (do_configure)
        retval = usb_helpers_configure_port((struct usb_port *) port,
                ctx, dev_path_ptr, path_len, gpio_num, NULL);
    else
        retval = usb_helpers_port_add_path((struct usb_port *) port,
                dev_path_ptr, path_len);

    if (retval) {
        fprintf(stderr, "Failed to configure gpio port\n");
        //No need for a thorough clean-up, we will fail and exit anyway
        free(port);
        return 1;
    }

    return retval;
}

uint8_t gpio_handler_parse_json(struct usb_monitor_ctx *ctx,
                                struct json_object *json)
{
    int json_arr_len = json_object_array_length(json);
    struct json_object *json_port, *path_array = NULL, *json_path;
    char *path;
    const char *path_org;
    int i, j;
    uint8_t gpio_num = -1, unknown = 0;

    for (i = 0; i < json_arr_len; i++) {
        json_port = json_object_array_get_idx(json, i); 

        json_object_object_foreach(json_port, key, val) {
            if (!strcmp(key, "path") && json_object_is_type(val, json_type_array)) {
                path_array = val;
                continue;
            } else if (!strcmp(key, "gpio_num") && json_object_is_type(val, json_type_int)) {
                gpio_num = (uint8_t) json_object_get_int(val);
                continue;
            } else {
                unknown = 1;
                break;
            }
        }

        if (path_array == NULL || !json_object_array_length(path_array) ||
                gpio_num == -1 || unknown)
            return 1;
        
        for (j = 0; j < json_object_array_length(path_array); j++) {
            json_path = json_object_array_get_idx(path_array, j);
            path_org = json_object_get_string(json_path);
            path = strdup(path_org);

            if (!path)
                return 1;

            if (gpio_handler_add_port(ctx, path, gpio_num)) {
                free(path);
                return 1;
            }

            USB_DEBUG_PRINT_SYSLOG(ctx, LOG_INFO, "Read following GPIO from config %s (%u)\n", path_org, gpio_num);
            free(path);

        }
    }

    return 0;
}
