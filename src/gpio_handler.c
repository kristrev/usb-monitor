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

static ssize_t gpio_write_value(struct gpio_port *gport, uint8_t gpio_val)
{
    char file_path_arr[GPIO_PATH_MAX_LEN];
    const char *file_path = file_path_arr;
    int32_t fd;
    ssize_t bytes_written = -1;

    //Do a write, if write is successful then we update power state
    if (gport->gpio_path) {
        file_path = gport->gpio_path;
    } else {
        snprintf(file_path_arr, sizeof(file_path_arr),
                 "/sys/class/gpio/gpio%u/value",
                 gport->port_num);
    }
    
    fd = open(file_path, O_WRONLY | FD_CLOEXEC);

    if (fd == -1) {
        USB_DEBUG_PRINT_SYSLOG(gport->ctx, LOG_ERR, "Failed to open gpio file %s\n", file_path);
        //usb_helpers_start_timeout((struct usb_port*) gport, DEFAULT_TIMEOUT_SEC);
        return bytes_written;
    }

    //USB_DEBUG_PRINT_SYSLOG(gport->ctx, LOG_INFO, "Will write %u to %s\n", gpio_val, file_path);

    if (gpio_val)
        bytes_written = write(fd, "1", 1);
    else
        bytes_written = write(fd, "0", 1);

    close(fd);

    return bytes_written;
}

//TODO: Wrap this one so that I can check for PROBE when requests from user
//space arrives
static int32_t gpio_update_port(struct usb_port *port, uint8_t cmd)
{
    struct gpio_port *gport = (struct gpio_port*) port;
    uint8_t gpio_val = gport->off_val;

    //TODO: If I am probing, start timer and return

    if (cmd == CMD_ENABLE) {
        if (gpio_write_value(gport, gport->on_val) <= 0)
            return -1;

        gport->enabled = 1;
        gport->pwr_state = 1;
        return 0;
    } else if (cmd == CMD_DISABLE) {
        //No need to any special clean-up, device will be removed and then we
        //let those functions take care of stopping timeouts etc.
        if (gpio_write_value(gport, gport->off_val) <= 0)
            return -1;

        gport->enabled = 0;
        gport->pwr_state = 0;
        //Set msg_mode to IDLE in case we interrupt a RESET. This way we make
        //sure that we can, in worst case, recover using timeout
        gport->msg_mode = IDLE;
        return 0;
    }

    //Consider returning an error here, will happen if we ever to reset a
    //disabled port
    if (!gport->enabled)
        return 0;

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
        gpio_val = gport->on_val;

    //If we for some reason fail to write, then we simply sleep and try again
    if (gpio_write_value(gport, gpio_val) <= 0) {
        USB_DEBUG_PRINT_SYSLOG(port->ctx, LOG_ERR, "Failed to write to gpio\n");
        usb_helpers_start_timeout((struct usb_port*) gport,
                DEFAULT_TIMEOUT_SEC);
        return -1;
    }

    gport->pwr_state = !gport->pwr_state;

    //port is switched on again, so it is safe to use for messages etc. It is OK
    //to set IDLE here since there will be no device seen connected to port yet
    if (gport->pwr_state) {
        gport->msg_mode = IDLE;
        return 0;
    }

    usb_helpers_start_timeout((struct usb_port*) gport, GPIO_TIMEOUT_SLEEP_SEC);
    return 0;
}

static void gpio_on_probe_down_done(struct gpio_port *port)
{
    struct usb_port *itr;
    struct gpio_port *gpio_itr;

    //Check if any device has not successfully been shut down
    LIST_FOREACH(itr, &(port->ctx->port_list), port_next) {
        if (itr->port_type != PORT_TYPE_GPIO)
            continue;

        gpio_itr = (struct gpio_port*) itr;

        if (gpio_itr->vp.vid || gpio_itr->vp.pid) {
            USB_DEBUG_PRINT_SYSLOG(gpio_itr->ctx, LOG_INFO, "Port %s still has "
                                   "device connected\n", gpio_itr->gpio_path);
            usb_helpers_start_timeout(itr, GPIO_TIMEOUT_SLEEP_SEC);
            return;
        }
    }

    USB_DEBUG_PRINT_SYSLOG(port->ctx, LOG_INFO, "All ports down, ready to "
            "start probe\n");

    //Start setting ports up
}

static void gpio_on_probe_timeout(struct gpio_port *port)
{
    if (port->probe_state == PROBE_DOWN ||
        port->probe_state == PROBE_DOWN_DONE) {
        USB_DEBUG_PRINT_SYSLOG(port->ctx, LOG_INFO, "Port %s moved to/in "
                "PROBE_DOWN_DONE\n", port->gpio_path);
        port->probe_state = PROBE_DOWN_DONE;
        gpio_on_probe_down_done(port);
    }
}

static void gpio_handle_timeout(struct usb_port *port)
{
    if (port->msg_mode == PING) {
        usb_helpers_send_ping(port);
    } else if (port->msg_mode == PROBE) {
        gpio_on_probe_timeout((struct gpio_port*) port);
    } else {
        gpio_update_port(port, CMD_RESTART);
    }
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
        uint8_t gpio_num, uint8_t on_val, uint8_t off_val, const char *gpio_path)
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

    port->on_val = on_val;
    port->off_val = off_val;
    port->gpio_path = gpio_path;

    return port;
}

static uint8_t gpio_handler_add_port_gpio_path(struct usb_monitor_ctx *ctx,
        const char *dev_path_ptr, uint8_t dev_path_len, uint8_t on_val,
        uint8_t off_val, const char *gpio_path)
{
    struct gpio_port *port = NULL;
    const char *gpio_path_cpy = NULL;

    gpio_path_cpy = strdup(gpio_path);

    if (!gpio_path_cpy) {
        fprintf(stderr, "Failed to allocate memory for gpio path\n");
        return 1;
    }

    //TODO: Add lookup for port when needed
    port = gpio_handler_create_port(ctx, 0, on_val, off_val, gpio_path_cpy);

    if (!port) {
        fprintf(stderr, "Failed to allocate memory for gpio port\n");
        return 1;
    }

    if(usb_helpers_configure_port((struct usb_port *) port,
                ctx, dev_path_ptr, dev_path_len, 0, NULL)) {
        fprintf(stderr, "Failed to configure gpio port\n");
        return 1;
    }

    return 0;
}

static uint8_t gpio_handler_add_port_gpio_num(struct usb_monitor_ctx *ctx,
        char *path, uint8_t gpio_num, const char *dev_path_ptr,
        uint8_t dev_path_len)
{
    struct gpio_port *port;
    uint8_t do_configure = 0, retval = 0;

    port = gpio_handler_get_port(ctx, gpio_num);

    //Port not found, create new
    if (!port) {
        port = gpio_handler_create_port(ctx, gpio_num, GPIO_DEFAULT_ON_VAL,
                GPIO_DEFAULT_OFF_VAL, NULL);
        do_configure = 1;
    }

    if (!port) {
        fprintf(stderr, "Failed to allocate memory for gpio port\n");
        return 1;
    }

    //Update path
    if (do_configure)
        retval = usb_helpers_configure_port((struct usb_port *) port,
                ctx, dev_path_ptr, dev_path_len, gpio_num, NULL);
    else
        retval = usb_helpers_port_add_path((struct usb_port *) port,
                dev_path_ptr, dev_path_len);

    if (retval) {
        fprintf(stderr, "Failed to configure gpio port\n");
        //No need for a thorough clean-up, we will fail and exit anyway
        free(port);
        return 1;
    }

    return retval;
}

static uint8_t gpio_handler_add_port(struct usb_monitor_ctx *ctx,
        char *path, uint8_t gpio_num, uint8_t on_val, uint8_t off_val,
        const char *gpio_path)
{
    //Bus + port(s)
    uint8_t dev_path[USB_PATH_MAX];
    const char *dev_path_ptr = (const char *) dev_path;
    uint8_t dev_path_len = 0;

    if (usb_helpers_convert_char_to_path(path, dev_path, &dev_path_len)) {
        fprintf(stderr, "Path for GPIO device is too long\n");
        return 1;
    }

    if (gpio_num) {
        return gpio_handler_add_port_gpio_num(ctx, path, gpio_num, dev_path_ptr,
                dev_path_len);
    } else {
        return gpio_handler_add_port_gpio_path(ctx, dev_path_ptr, dev_path_len,
                on_val, off_val, gpio_path);
    }
}

uint8_t gpio_handler_parse_json(struct usb_monitor_ctx *ctx,
                                struct json_object *json)
{
    int json_arr_len = json_object_array_length(json);
    struct json_object *json_port, *path_array = NULL, *json_path;
    char *path;
    const char *path_org, *gpio_path = NULL;
    int i, j;
    uint8_t gpio_num = 0; 
    uint8_t on_val = GPIO_DEFAULT_ON_VAL;
    uint8_t off_val = GPIO_DEFAULT_OFF_VAL;
    uint8_t unknown = 0;

    for (i = 0; i < json_arr_len; i++) {
        json_port = json_object_array_get_idx(json, i); 

        json_object_object_foreach(json_port, key, val) {
            if (!strcmp(key, "path") && json_object_is_type(val, json_type_array)) {
                //USB path to match
                path_array = val;
                continue;
            } else if (!strcmp(key, "gpio_num") && json_object_is_type(val, json_type_int)) {
                //GPIO number (used to create sysfs template)
                gpio_num = (uint8_t) json_object_get_int(val);
                continue;
            } else if (!strcmp(key, "on_val") && json_object_is_type(val, json_type_int)) {
                //Custom on value (some devices are GPIO-like)
                on_val = (uint8_t) json_object_get_int(val);
                continue;
            } else if (!strcmp(key, "off_val") && json_object_is_type(val, json_type_int)) {
                //Custom off value
                off_val = (uint8_t) json_object_get_int(val);
                continue;
            } else if (!strcmp(key, "gpio_path") && json_object_is_type(val, json_type_string)) {
                //Custom path for GPIO like device (like modem on Glinet Mifi)
                gpio_path = json_object_get_string(val);
                break;
            } else {
                unknown = 1;
                break;
            }
        }

        if (unknown ||
            path_array == NULL ||
            !json_object_array_length(path_array) ||
            (!gpio_num && !gpio_path) ||
            (gpio_num && gpio_path))
            return 1;
        
        for (j = 0; j < json_object_array_length(path_array); j++) {
            json_path = json_object_array_get_idx(path_array, j);
            path_org = json_object_get_string(json_path);
            path = strdup(path_org);

            if (!path)
                return 1;

            if (gpio_handler_add_port(ctx, path, gpio_num, on_val, off_val,
                        gpio_path)) {
                free(path);
                return 1;
            }

            USB_DEBUG_PRINT_SYSLOG(ctx, LOG_INFO,
                                   "Read following GPIO from config %s (%u) on: %u off: %u\n",
                                   path_org, gpio_num, on_val, off_val);
            free(path);
        }
    }

    return 0;
}

int32_t gpio_handler_start_probe(struct usb_monitor_ctx *ctx)
{
    struct usb_port *itr;
    struct gpio_port *port = NULL;

    LIST_FOREACH(itr, &(ctx->port_list), port_next) {
        if (itr->port_type != PORT_TYPE_GPIO)
            continue;

        port = (struct gpio_port*) itr;

        if (gpio_update_port(itr, CMD_DISABLE)) {
            USB_DEBUG_PRINT_SYSLOG(ctx, LOG_ERR,
                                   "Failed to start probe for pin %s\n",
                                   port->gpio_path);
            return -1; 
        }

        USB_DEBUG_PRINT_SYSLOG(ctx, LOG_INFO,
                               "Started probe for pin %s\n", port->gpio_path);
        port->probe_state = PROBE_DOWN;
        port->msg_mode = PROBE;

        //Timeout is started when we add a device (in order to run USB ping).
        //Stop timeout for all ports here. It is maybe not very elegant, but I
        //found it easier than adding some probe/non-probe check to the generic
        //add helper
        if (usb_monitor_lists_is_timeout_active(itr)) {
            usb_monitor_lists_del_timeout(itr);
        }
    }

    //Does not matter which port we start the timer for
    //TODO: Consider restructuring usb monitor to have a handler for every port
    //type (and not just port objects for != Ykush). Right now, for example this
    //timer is used as a global timer for GPIO, but the timer is actually tied
    //to only one port. The code works because access to ports is serialized
    //while probing, but the structure is a bit confusing
    usb_helpers_start_timeout((struct usb_port*) port, GPIO_TIMEOUT_SLEEP_SEC);

    return 0;
}
