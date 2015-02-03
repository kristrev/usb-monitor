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
    int i;
    struct libusb_device_descriptor desc;
    struct gpio_port *gport = (struct gpio_port*) port;

    USB_DEBUG_PRINT(port->ctx->logfile, "Type: GPIO Path: ");

    for (i = 0; i < gport->path_len-1; i++)
        fprintf(port->ctx->logfile, "%u-", gport->path[i]);

    fprintf(port->ctx->logfile, "%u State: %u Pwr: %u Gpio: %u ", gport->path[i],
            gport->status, gport->pwr_state, gport->gpio_num);

    if (gport->dev) {
        libusb_get_device_descriptor(gport->dev, &desc);
        fprintf(port->ctx->logfile, " Device: %.4x:%.4x", desc.idVendor, desc.idProduct);
    }

    fprintf(port->ctx->logfile, "\n");
    fflush(port->ctx->logfile);
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
        gport->timeout_next.le_prev != NULL) {
            USB_DEBUG_PRINT(port->ctx->logfile, "Will delete:\n");
            gpio_print_port((struct usb_port*) gport);
            usb_monitor_lists_del_timeout((struct usb_port*) gport);
    }

    //POWER_OFF is 0, so then we should switch on port
    if (!gport->pwr_state)
        gpio_val = 1;

    //Do a write, if write is successful then we update power state
    snprintf(file_path, sizeof(file_path), "/sys/class/gpio/gpio%u/value", gport->gpio_num);

    USB_DEBUG_PRINT(port->ctx->logfile, "Will write %u to %s\n", gpio_val, file_path);
    
    fd = open(file_path, O_WRONLY | FD_CLOEXEC);

    if (fd == -1) {
        USB_DEBUG_PRINT(port->ctx->logfile, "Failed to open gpio file\n");
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
            USB_DEBUG_PRINT(port->ctx->logfile, "GPIO %u switched on again\n", gport->gpio_num);
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

uint8_t gpio_handler_add_port(struct usb_monitor_ctx *ctx, char *path,
                              uint8_t gpio_num)
{
    struct gpio_port *port;

    //Bus + port(s)
    uint8_t dev_path[USB_PATH_MAX];
    uint8_t path_len = 0;
    char *cur_val = NULL;
    uint8_t i;

    //First, I need to parse path. Path is on format x-x-x-x-x
    //TODO: Make generic when we enable user input
    cur_val = strtok(path, "-");

    for (i = 0; i < 8; i++) {
        if (cur_val == NULL)
            break;
        
        dev_path[i] = (uint8_t) atoi(cur_val);
        cur_val = strtok(NULL, "-");
    }

    if (i == 8 && cur_val != NULL) {
        fprintf(stderr, "Path for GPIO device is too long\n");
        return 1;
    } else {
        path_len = i;
    }

    //TODO: Check if port is already in list
    if (usb_monitor_lists_find_port_path(ctx, dev_path, path_len)) {
         fprintf(stderr, "GPIO port already found\n");
         return 1;
    }

    port = malloc(sizeof(struct gpio_port));
    
    if (port == NULL) {
        fprintf(stderr, "Could not allocate memory for gpio port\n");
        return 1;
    }

    memset(port, 0, sizeof(struct gpio_port));
    memcpy(port->path, dev_path, path_len);
    port->path_len = path_len;
    port->pwr_state = POWER_ON;
    port->output = gpio_print_port;
    port->update = gpio_update_port;
    port->timeout = gpio_handle_timeout;
    port->gpio_num = gpio_num;
    port->ctx = ctx;

    usb_monitor_lists_add_port(ctx, (struct usb_port*) port);

    return 0;
}

uint8_t gpio_handler_parse_json(struct usb_monitor_ctx *ctx,
                                struct json_object *json)
{
    int json_arr_len = json_object_array_length(json);
    struct json_object *json_port;
    int i;
    char *path = NULL;
    uint8_t gpio_num = -1, unknown = 0;

    for (i = 0; i < json_arr_len; i++) {
        json_port = json_object_array_get_idx(json, i); 

        json_object_object_foreach(json_port, key, val) {
            if (!strcmp(key, "path")) {
                path = (char*) json_object_get_string(val);
                continue;
            } else if (!strcmp(key, "gpio_num")) {
                gpio_num = (uint8_t) json_object_get_int(val);
                continue;
            } else {
                unknown = 1;
                break;
            }
        }

        if (path == NULL || gpio_num == -1 || unknown)
            return 1;
        
        USB_DEBUG_PRINT(ctx->logfile, "Read following GPIO from config %s (%u)\n", path, gpio_num);

        if (gpio_handler_add_port(ctx, path, gpio_num)) {
            return 1;
        }
    }

    return 0;
}
