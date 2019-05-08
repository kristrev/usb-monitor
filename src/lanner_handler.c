#include <json-c/json.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <sys/epoll.h>
#include <stdbool.h>
#include <time.h>
#include <sys/file.h>

#include "usb_monitor.h"
#include "usb_logging.h"
#include "usb_helpers.h"
#include "lanner_handler.h"
#include "backend_event_loop.h"

static void lanner_handler_start_mcu_update(struct usb_monitor_ctx *ctx);

static void lanner_print_port(struct usb_port *port)
{
    char buf[10] = {0};
    struct lanner_port *l_port = (struct lanner_port*) port;

    snprintf(buf, sizeof(buf), "(bit %u)", (uint8_t) ffs(l_port->bitmask));

    usb_helpers_print_port(port, "Lanner", buf);
}

static int32_t lanner_update_port(struct usb_port *port, uint8_t cmd)
{
    struct lanner_port *l_port = (struct lanner_port*) port;
    struct lanner_shared *l_shared = l_port->shared_info;

    //Order of operations here is:
    //* Return an error if shared is not IDLE/PENDING
    //* Set cmd of port to whatever is stored
    //* Update bitmask of shared
    if (l_shared->mcu_state != LANNER_MCU_IDLE &&
        l_shared->mcu_state != LANNER_MCU_PENDING) {
        USB_DEBUG_PRINT_SYSLOG(l_port->ctx, LOG_INFO, "Lanner MCU busy\n");
        return 503;
    }

    //We keep the current command in the port. The bitmask will be generated
    //based on the different cur_cmd values
    l_port->cur_cmd = cmd;

    if (cmd == CMD_RESTART) {
        l_port->restart_cmd = CMD_DISABLE;
        l_port->msg_mode = RESET;
    }

    //"Register" this port with the shared structure
    l_shared->pending_ports_mask |= l_port->bitmask;

    //Ensure that state of l_shared is correct + that callback is added. Doing
    //these operations multiple times does no harm
    l_shared->mcu_state = LANNER_MCU_PENDING;
    usb_monitor_start_itr_cb(l_port->ctx);

    return 0;
}

static void lanner_handle_timeout(struct usb_port *port)
{
    if (port->msg_mode == PING) {
        usb_helpers_send_ping(port);
    }
}

static uint8_t lanner_handler_add_port(struct usb_monitor_ctx *ctx,
                                       char *dev_path, uint8_t bit,
                                       struct lanner_shared *l_shared)
{
    uint8_t dev_path_array[USB_PATH_MAX];
    const char *dev_path_ptr = (const char *) dev_path_array;
    uint8_t dev_path_len = 0;
    struct lanner_port *port;

    if (usb_helpers_convert_char_to_path(dev_path, dev_path_array,
                                         &dev_path_len)) {
        fprintf(stderr, "Path for Lanner device is too long\n");
        return 1;
    }

    port = calloc(sizeof(struct lanner_port), 1);

    if (!port) {
        fprintf(stderr, "Could not allocate memory for lanner port\n");
        return 1;
    }

    port->port_type = PORT_TYPE_LANNER;
    port->output = lanner_print_port;
    port->update = lanner_update_port;
    port->timeout = lanner_handle_timeout;

    port->shared_info = l_shared;
    //This is the bitmask used to enable/disable the port. The reason bit is
    //not zero-indexed in config, is to be consistent with Lanner tools/doc
    port->bitmask = 1 << (bit - 1);

    //If we get into the situation where multiple paths are controlled by the
    //same bit, we need to implement a lookup here (similar to gpio and
    //gpio_num). However, so far on Lanner, one port == one bit
    if(usb_helpers_configure_port((struct usb_port *) port,
                                  ctx, dev_path_ptr, dev_path_len, 0, NULL)) {
        fprintf(stderr, "Failed to configure lanner port\n");
        free(port);
        return 1;
    }

    return 0;
}

static void lanner_handler_flush_mcu(struct lanner_shared *l_shared)
{
    uint8_t tmp_buf[255];

    while (read(l_shared->mcu_fd, tmp_buf, sizeof(tmp_buf)) > 0) {}
}

static uint8_t lanner_handler_open_mcu(struct lanner_shared *l_shared)
{
    int fd = open(l_shared->mcu_path, O_RDWR | O_NOCTTY | O_NONBLOCK |
                  O_CLOEXEC), retval;
    struct termios mcu_attr;

    if (fd == -1) {
        fprintf(stderr, "Failed to open file: %s (%d)\n", strerror(errno),
                errno);
        return 1;
    }

    //Baud rate is 57600 according to documentation
    retval = tcgetattr(fd, &mcu_attr);

    if (retval) {
        fprintf(stderr, "Fetching terminal attributes failed: %s (%d)\n",
                strerror(errno), errno);
        close(fd);
        return 1;
    }

    // Configuration steps taken from: https://en.wikibooks.org/wiki/Serial_Programming/termios
    //
    // Input flags - Turn off input processing
    //
    // convert break to null byte, no CR to NL translation,
    // no NL to CR translation, don't mark parity errors or breaks
    // no input parity check, don't strip high bit off,
    // no XON/XOFF software flow control
    //
    mcu_attr.c_iflag &= ~(IGNBRK | BRKINT | ICRNL |
                        INLCR | PARMRK | INPCK | ISTRIP | IXON);

    //
    // Output flags - Turn off output processing
    //
    // no CR to NL translation, no NL to CR-NL translation,
    // no NL to CR translation, no column 0 CR suppression,
    // no Ctrl-D suppression, no fill characters, no case mapping,
    // no local output processing
    //
    // mcu_attr.c_oflag &= ~(OCRNL | ONLCR | ONLRET |
    //                     ONOCR | ONOEOT| OFILL | OLCUC | OPOST);
    mcu_attr.c_oflag = 0;

    //
    // No line processing
    //
    // echo off, echo newline off, canonical mode off,
    // extended input processing off, signal chars off
    //
    mcu_attr.c_lflag &= ~(ECHO | ECHONL | ICANON | IEXTEN | ISIG);

    //
    // Turn off character processing
    //
    // clear current char size mask, no parity checking,
    // no output processing, force 8 bit input
    //
    mcu_attr.c_cflag &= ~(CSIZE | PARENB);
    mcu_attr.c_cflag |= CS8;

    //
    // One input byte is enough to return from read()
    // Inter-character timer off
    //
    mcu_attr.c_cc[VMIN]  = 1;
    mcu_attr.c_cc[VTIME] = 0;

    if (cfsetospeed(&mcu_attr, B57600)) {
        fprintf(stderr, "Setting speed failed: %s (%d)\n", strerror(errno),
                errno);
        close(fd);
        return 1;
    }

    retval = tcsetattr(fd, TCSANOW, &mcu_attr);

    if (retval) {
        fprintf(stderr, "Setting terminal attributes failed: %s (%d)\n",
                strerror(errno), errno);
        close(fd);
        return 1;
    }

    l_shared->mcu_fd = fd;

    return 0;
}

static void lanner_flush_mcu(struct lanner_shared *l_shared)
{
    uint8_t buf_tmp[256];
    int numbytes;

    //Very simple flush, just read until we get an error (which will be EAGAIN
    //or EWOULDBLOCK). This is good enough for now
    while(1) {
        numbytes = read(l_shared->mcu_fd, buf_tmp, sizeof(buf_tmp));

        if (numbytes < 0) {
            return;
        }
    }
}

static void lanner_handler_cleanup_shared(struct lanner_shared *l_shared)
{
    if (l_shared->mcu_timeout_handle) {
        free(l_shared->mcu_timeout_handle);
    }

    if (l_shared->mcu_epoll_handle) {
        free(l_shared->mcu_epoll_handle);
    }

    if (l_shared->mcu_fd) {
        close(l_shared->mcu_fd);
    }

    if (l_shared->mcu_path) {
        free(l_shared->mcu_path);
    }

    free(l_shared);
}

static void lanner_handler_start_private_timer(struct lanner_shared *l_shared,
                                               uint32_t timeout_ms)
{
    struct timespec tp;
    uint64_t cur_time;

    clock_gettime(CLOCK_MONOTONIC_RAW, &tp);
    cur_time = (tp.tv_sec * 1e3) + (tp.tv_nsec / 1e6);

    l_shared->mcu_timeout_handle->timeout_clock = cur_time + timeout_ms;

    backend_insert_timeout(l_shared->ctx->event_loop,
                           l_shared->mcu_timeout_handle);
}

static void lanner_handler_write_cmd_buf(struct lanner_shared *l_shared)
{
    struct usb_monitor_ctx *ctx = l_shared->ctx;
    //uint8_t bytes_to_write = l_shared->cmd_buf_strlen - l_shared->cmd_buf_progress;
    uint8_t bytes_to_write = 1;
    ssize_t numbytes = write(l_shared->mcu_fd,
                             l_shared->cmd_buf + l_shared->cmd_buf_progress,
                             bytes_to_write);
    uint32_t monitor_events;

    if (numbytes == -1) {
        //Start EPOLLOUT
        if (errno == EAGAIN) {
            backend_event_loop_update(ctx->event_loop, EPOLLIN | EPOLLOUT,
                                      EPOLL_CTL_MOD, l_shared->mcu_fd,
                                      l_shared->mcu_epoll_handle);
        } else {
            //I don't know what to do here? Can it happen? Can we recover? Just
            //add EPOLLIN and hope for the best? exit?
            USB_DEBUG_PRINT_SYSLOG(ctx, LOG_ERR, "Failed to write to Lanner MCU: "
                                   "%s (%d)\n", strerror(errno), errno);
            exit(EXIT_FAILURE);
        }

        return;
    }

    l_shared->cmd_buf_progress += numbytes;

    if (l_shared->cmd_buf_progress == l_shared->cmd_buf_strlen) {
        USB_DEBUG_PRINT_SYSLOG(ctx, LOG_INFO, "Done writing command\n");
        monitor_events = EPOLLIN;

        if (l_shared->mcu_state == LANNER_MCU_READING) {
            l_shared->mcu_state = LANNER_MCU_WAIT_REPLY;
        } else {
            l_shared->mcu_state = LANNER_MCU_WAIT_OK;
        }
    } else {
        //We need to wait for EPOLLOUT, we had a short write
        monitor_events = EPOLLIN | EPOLLOUT;
    }

    backend_event_loop_update(ctx->event_loop, monitor_events,
                              EPOLL_CTL_MOD, l_shared->mcu_fd,
                              l_shared->mcu_epoll_handle);
}

static uint8_t lanner_handler_create_mcu_bitmask(struct lanner_shared *l_shared,
                                                 uint8_t bitmask_from_mcu)
{
    struct usb_port *itr;
    struct lanner_port *l_port;
    uint8_t bitmask_to_mcu = bitmask_from_mcu, cmd_to_check;

    //TODO: Separate function
    LIST_FOREACH(itr, &(l_shared->ctx->port_list), port_next) {
        if (itr->port_type != PORT_TYPE_LANNER) {
            continue;
        }

        l_port = (struct lanner_port*) itr;

        if (!(l_shared->pending_ports_mask & l_port->bitmask)) {
            continue;
        }

        cmd_to_check = l_port->cur_cmd == CMD_RESTART ? l_port->restart_cmd :
                                                        l_port->cur_cmd;

        //In order to enable a port, we disable the bit
        if (cmd_to_check == CMD_ENABLE) {
             bitmask_to_mcu &= ~l_port->bitmask;
        } else {
            bitmask_to_mcu |= l_port->bitmask;
        }
    }

    return bitmask_to_mcu;
}

static void lanner_handler_set_digital_out(struct lanner_shared *l_shared)
{
    l_shared->mcu_bitmask_to_write = lanner_handler_create_mcu_bitmask(l_shared,
                                                                       (uint8_t) l_shared->mcu_bitmask);

    USB_DEBUG_PRINT_SYSLOG(l_shared->ctx, LOG_ERR,
                           "Bitmask from mcu %u to mcu %u\n",
                           l_shared->mcu_bitmask,
                           l_shared->mcu_bitmask_to_write);

    snprintf(l_shared->cmd_buf, sizeof(l_shared->cmd_buf),
             "SET DIGITAL_OUT %u\n", l_shared->mcu_bitmask_to_write);
    l_shared->cmd_buf_strlen = strlen(l_shared->cmd_buf);
    l_shared->cmd_buf_progress = 0;

    memset(l_shared->buf_input, 0, sizeof(l_shared->buf_input));
    l_shared->input_progress = 0;

    l_shared->mcu_state = LANNER_MCU_WRITING;

    USB_DEBUG_PRINT_SYSLOG(l_shared->ctx, LOG_INFO, "Lanner MCU cmd %s",
                           l_shared->cmd_buf);

    lanner_handler_write_cmd_buf(l_shared);
}

static void lanner_handler_reply(struct lanner_shared *l_shared)
{
    int n;

    //The Lanner MCU is available in two different versions (at least). On one
    //version, the GET_DIGITAL_OUT reply has not space after "=". On the other,
    //there is a space
    n = sscanf(l_shared->buf_input, LANNER_HANDLER_REPLY "= %u",
               &(l_shared->mcu_bitmask));
    if (!n) {
        n = sscanf(l_shared->buf_input, LANNER_HANDLER_REPLY "=%u",
                   &(l_shared->mcu_bitmask));
    }

    if (!n) {
        USB_DEBUG_PRINT_SYSLOG(l_shared->ctx, LOG_ERR, "No bitmask found\n");
        //I do not know what to here except fail, we don't know what we are
        //working with
        exit(EXIT_FAILURE);
    }

    lanner_handler_set_digital_out(l_shared);
}

static void lanner_handler_ok_reply(struct lanner_shared *l_shared)
{
    struct usb_port *itr;
    struct lanner_port *l_port;
    uint8_t cmd_to_check;

    LIST_FOREACH(itr, &(l_shared->ctx->port_list), port_next) {
        if (itr->port_type != PORT_TYPE_LANNER) {
            continue;
        }

        l_port = (struct lanner_port*) itr;

        if (!(l_shared->pending_ports_mask & l_port->bitmask)) {
            continue;
        }

        cmd_to_check = l_port->cur_cmd == CMD_RESTART ? l_port->restart_cmd :
                                                        l_port->cur_cmd;

        if (cmd_to_check == CMD_ENABLE) {
            l_port->enabled = 1;
            l_port->pwr_state = 1;
            l_port->msg_mode = IDLE;

            //Always unset bit in ENABLE. It is either a command itself or the
            //last part of restart (restart is done)
            l_shared->pending_ports_mask &= ~l_port->bitmask;
        } else if (cmd_to_check == CMD_DISABLE) {
            l_port->enabled = 0;
            l_port->pwr_state = 0;

            //Only unset bit if command is not RESTART. If command is restart,
            //weneed to enable port again
            if (l_port->cur_cmd != CMD_RESTART) {
                l_shared->pending_ports_mask &= ~l_port->bitmask;
            } else {
                l_port->restart_cmd = CMD_ENABLE;
            }
        }
    }

    l_shared->mcu_bitmask = l_shared->mcu_bitmask_to_write;
    l_shared->mcu_bitmask_to_write = 0;

    USB_DEBUG_PRINT_SYSLOG(l_shared->ctx, LOG_INFO, "Lanner pending after OK: %u\n",
                           l_shared->pending_ports_mask);

    if (!l_shared->pending_ports_mask) {
        l_shared->mcu_state = LANNER_MCU_UPDATE_DONE;
        usb_monitor_start_itr_cb(l_port->ctx);
    } else {
        l_shared->mcu_state = LANNER_MCU_WRITING;
        lanner_handler_start_private_timer(l_shared, LANNER_HANDLER_RESTART_MS);
    }
}

static void lanner_handler_handle_input(struct lanner_shared *l_shared)
{
    uint8_t input_buf_tmp[256] = {0};
    ssize_t numbytes = read(l_shared->mcu_fd, input_buf_tmp,
                            (sizeof(input_buf_tmp) - 1));
    uint8_t i;
    bool found_newline = false;

    if ((l_shared->mcu_state != LANNER_MCU_WAIT_OK &&
         l_shared->mcu_state != LANNER_MCU_WAIT_REPLY) || numbytes <= 0) {
        return;
    }

    //Aborting here is not very nice, but it is the only thing I can think of
    //now. Aborting is not a big problem, as usb monitor will recover fine after
    //a restart. Remember that we have already written our command (unless something
    //breaks in the MCU), so if we get here and fail then for example a disabled
    //port will be enabled again by our watchdog.
    if (numbytes + l_shared->input_progress > (sizeof(l_shared->buf_input) - 1)) {
        USB_DEBUG_PRINT_SYSLOG(l_shared->ctx, LOG_ERR,
                               "Oversized reply from Lanner MCU\n");
        exit(EXIT_FAILURE);
    }

    memcpy(l_shared->buf_input + l_shared->input_progress, input_buf_tmp,
           numbytes);
    l_shared->input_progress += numbytes;

    for (i = 0; i < l_shared->input_progress; i++) {
        if (l_shared->buf_input[i] == '\n') {
            found_newline = true;
            break;
        }
    }

    if (!found_newline) {
        return;
    }

    USB_DEBUG_PRINT_SYSLOG(l_shared->ctx, LOG_INFO, "BUFFER %s\n", l_shared->buf_input);

    if (l_shared->mcu_state == LANNER_MCU_WAIT_REPLY &&
        !strncmp(LANNER_HANDLER_REPLY, l_shared->buf_input,
                 strlen(LANNER_HANDLER_REPLY))) {
        lanner_handler_reply(l_shared);
    } else if (l_shared->mcu_state == LANNER_MCU_WAIT_OK &&
               !strncmp(LANNER_HANDLER_OK_REPLY, l_shared->buf_input,
                        strlen(LANNER_HANDLER_OK_REPLY))) {
        lanner_handler_ok_reply(l_shared);
    }
}

static void lanner_handler_event_cb(void *ptr, int32_t fd, uint32_t events)
{
    struct usb_monitor_ctx *ctx = ptr;

    if (events & EPOLLIN) {
        lanner_handler_handle_input(ctx->mcu_info);
    }

    if (events & EPOLLOUT) {
        lanner_handler_write_cmd_buf(ctx->mcu_info);
    }
}

static void lanner_handle_private_timeout(void *ptr)
{
    lanner_handler_start_mcu_update(ptr);
}

static void lanner_handler_get_digital_out(struct usb_monitor_ctx *ctx)
{
    struct lanner_shared *l_shared = ctx->mcu_info;

    snprintf(l_shared->cmd_buf, sizeof(l_shared->cmd_buf), "GET DIGITAL_OUT\n");

    l_shared->cmd_buf_strlen = strlen(l_shared->cmd_buf);
    l_shared->cmd_buf_progress = 0;

    memset(l_shared->buf_input, 0, sizeof(l_shared->buf_input));
    l_shared->input_progress = 0;

    l_shared->mcu_state = LANNER_MCU_READING;

    lanner_handler_write_cmd_buf(l_shared);
}

static void lanner_handler_start_mcu_update(struct usb_monitor_ctx *ctx)
{
    struct lanner_shared *l_shared = ctx->mcu_info;

    if (l_shared->mcu_state == LANNER_MCU_PENDING) {
        //Lock file, MCU is exclusive resource
        if (flock(l_shared->lock_fd, LOCK_EX | LOCK_NB)) {
            USB_DEBUG_PRINT_SYSLOG(l_shared->ctx, LOG_ERR, "Failed to lock MCU "
                                   "file. Reason %s (%d)\n", strerror(errno),
                                   errno);
            lanner_handler_start_private_timer(l_shared,
                                               LANNER_HANDLER_RESTART_MS);
            return;
        }

        //Open device
        if (lanner_handler_open_mcu(l_shared)) {
            flock(l_shared->lock_fd, LOCK_UN);
            lanner_handler_start_private_timer(l_shared,
                                               LANNER_HANDLER_RESTART_MS);
            return;
        }

        lanner_handler_flush_mcu(l_shared);

        //Update epoll handle
        l_shared->mcu_epoll_handle->fd = l_shared->mcu_fd;
        backend_event_loop_update(ctx->event_loop, EPOLLIN, EPOLL_CTL_ADD,
                                  l_shared->mcu_fd, l_shared->mcu_epoll_handle);

        lanner_handler_get_digital_out(ctx);
    } else {
        lanner_handler_set_digital_out(ctx->mcu_info);
    }
}

void lanner_handler_itr_cb(struct usb_monitor_ctx *ctx)
{
    struct lanner_shared *l_shared = ctx->mcu_info;

    if (l_shared->mcu_state == LANNER_MCU_UPDATE_DONE) {
        //Close file and clean lock, we are done
        flock(l_shared->lock_fd, LOCK_UN);
        close(l_shared->mcu_fd);
        l_shared->mcu_state = LANNER_MCU_IDLE;
    } else {
        lanner_handler_start_mcu_update(ctx);
    }
}

uint8_t lanner_handler_parse_json(struct usb_monitor_ctx *ctx,
                                  struct json_object *json,
                                  const char *mcu_path_org,
                                  const char *mcu_lock_path)
{
    int json_arr_len = json_object_array_length(json);
    struct json_object *json_port, *path_array = NULL, *json_path;
    char *path, *mcu_path;
    const char *path_org;
    int i, j;
    uint8_t bit = UINT8_MAX, unknown_option = 0;
    struct lanner_shared *l_shared;

    if (!mcu_path_org || !mcu_lock_path) {
        return 1;
    }

    if (!(mcu_path = strdup(mcu_path_org))) {
        return 1;
    }

    l_shared = calloc(sizeof(struct lanner_shared), 1);

    if (!l_shared) {
        free(mcu_path);
        return 1;
    }

    l_shared->ctx = ctx;
    l_shared->mcu_state = LANNER_MCU_IDLE;
    l_shared->mcu_path = mcu_path;

    if ((l_shared->lock_fd = open(mcu_lock_path, O_RDONLY)) < 0) {
        lanner_handler_cleanup_shared(l_shared);
        return 1;
    }

    if (!(l_shared->mcu_epoll_handle = backend_create_epoll_handle(ctx,
                                                                   0,
                                                                   lanner_handler_event_cb,
                                                                   0))) {
        lanner_handler_cleanup_shared(l_shared);
        return 1;
    }

    if (!(l_shared->mcu_timeout_handle = backend_event_loop_add_timeout(ctx->event_loop,
                                                                        0,
                                                                        lanner_handle_private_timeout,
                                                                        ctx,
                                                                        0,
                                                                        false))) {
        lanner_handler_cleanup_shared(l_shared);
        return 1;
    }


    USB_DEBUG_PRINT_SYSLOG(ctx, LOG_INFO, "Lanner shared info. Path: %s\n",
                           l_shared->mcu_path);

    for (i = 0; i < json_arr_len; i++) {
        json_port = json_object_array_get_idx(json, i);

        json_object_object_foreach(json_port, key, val) {
            if (!strcmp(key, "path") && json_object_is_type(val, json_type_array)) {
                path_array = val;
            } else if (!strcmp(key, "bit") && json_object_is_type(val, json_type_int)) {
                bit = (uint8_t) json_object_get_int(val);
            } else {
                unknown_option = 1;
                break;
            }
        }

        if (unknown_option || bit == UINT8_MAX || !path_array
            || !json_object_array_length(path_array)) {
            lanner_handler_cleanup_shared(l_shared);
            return 1;
        }

        for (j = 0; j < json_object_array_length(path_array); j++) {
            json_path = json_object_array_get_idx(path_array, j);
            path_org = json_object_get_string(json_path);
            path = strdup(path_org);

            if (!path) {
                lanner_handler_cleanup_shared(l_shared);
                return 1;
            }

            if (lanner_handler_add_port(ctx, path, bit, l_shared)) {
                free(path);
                lanner_handler_cleanup_shared(l_shared);
                return 1;
            }

            USB_DEBUG_PRINT_SYSLOG(ctx, LOG_INFO,
                                   "Read following info from config. Path  %s "
                                   "(bit %u) (Lanner)\n", path_org, bit);
            free(path);
        }
    }

    //This is not very clean, lanner state should ideally be completely
    //isolated. However, I prefer this approach to iterating through ports and
    //finding the first Lanner port (for example)
    ctx->mcu_info = l_shared;
    return 0;
}
