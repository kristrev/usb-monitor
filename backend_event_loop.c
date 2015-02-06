#include <stdint.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>

//Remove
#include <stdio.h>

#include "backend_event_loop.h"

struct backend_event_loop* backend_event_loop_create()
{
    struct backend_event_loop *del = calloc(sizeof(struct backend_event_loop), 1);

    if(!del)
        return NULL;

    if((del->efd = epoll_create(MAX_EPOLL_EVENTS)) == -1){
        free(del);
        return NULL;
    }

    LIST_INIT(&(del->timeout_list));

    return del;
}

void backend_configure_epoll_handle(struct backend_epoll_handle *handle,
		void *ptr, int fd, backend_epoll_cb cb)
{
	handle->data = ptr;
	handle->fd = fd;
	handle->cb = cb;
}

struct backend_epoll_handle* backend_create_epoll_handle(
        void *ptr, int fd, backend_epoll_cb cb, uint8_t libusb_fd){
    struct backend_epoll_handle *handle =
        calloc(sizeof(struct backend_epoll_handle), 1);

    if(handle != NULL)
		backend_configure_epoll_handle(handle, ptr, fd, cb);

    handle->libusb_fd = libusb_fd;

    return handle;
}

int32_t backend_event_loop_update(struct backend_event_loop *del, uint32_t events,
        int32_t op, int32_t fd, void *ptr)
{
    struct epoll_event ev;

    ev.events = events;
    ev.data.ptr = ptr;

    return epoll_ctl(del->efd, op, fd, &ev);
} 

static void backend_print_timeouts(struct backend_event_loop *del)
{
    struct backend_timeout_handle *itr = del->timeout_list.lh_first;

    while (itr != NULL) {
        printf("%lu\n", itr->timeout_clock);
        itr = itr->timeout_next.le_next;
    }
    printf("\n");
}

static void backend_insert_timeout(struct backend_event_loop *del,
                                   struct backend_timeout_handle *handle)
{
    struct backend_timeout_handle *itr = del->timeout_list.lh_first, *prev_itr;

    if (itr == NULL ||
        handle->timeout_clock < itr->timeout_clock) {
        LIST_INSERT_HEAD(&(del->timeout_list), handle, timeout_next);
        return;
    } 

    //Move to a separate insert function
    for (; itr != NULL; itr = itr->timeout_next.le_next) {
        if (handle->timeout_clock < itr->timeout_clock)
            break;

        prev_itr = itr;
    }

    LIST_INSERT_AFTER(prev_itr, handle, timeout_next);
}

//For now, this has to be set before event loop is run. Can't be update while
//running.
//TODO: Use absolute time instead. Then I can have timeouts in a sorted list (or
//other data structure) and use difftime-ish instead
struct backend_timeout_handle* backend_event_loop_add_timeout(
        struct backend_event_loop *del, uint64_t timeout_clock,
        backend_timeout_cb timeout_cb, void *ptr, uint32_t intvl)
{
    //In an improved version, handle can be passed as argument so that it is up
    //to application how to allocate it
    struct backend_timeout_handle *handle =
        malloc(sizeof(struct backend_timeout_handle));

    if (!handle)
        return NULL;

    handle->timeout_clock = timeout_clock;
    handle->cb = timeout_cb;
    handle->data = ptr;
    handle->intvl = intvl;

    backend_insert_timeout(del, handle);
    return handle;
}

static void backend_event_loop_run_timers(struct backend_event_loop *del)
{
    struct backend_timeout_handle *timeout = del->timeout_list.lh_first;
    struct backend_timeout_handle *cur_timeout;
    struct timeval tv;
    uint64_t cur_time;

    gettimeofday(&tv, NULL);
    cur_time = (tv.tv_sec * 1e3) + (tv.tv_usec / 1e3);

    while (timeout != NULL) {
        if (timeout->timeout_clock <= cur_time) {
            cur_timeout = timeout;
            timeout = timeout->timeout_next.le_next;

            //Execute and remove timeout from list
            cur_timeout->cb(cur_timeout->data);
            LIST_REMOVE(cur_timeout, timeout_next);

            //Rearm timer or free memory if we are done
            if (cur_timeout->intvl) {
                cur_timeout->timeout_clock = cur_time + cur_timeout->intvl;
                backend_insert_timeout(del, cur_timeout);
            } else {
                free(cur_timeout);
            }
        } else {
            break;
        }
    }
}

void backend_event_loop_run(struct backend_event_loop *del)
{
    struct backend_epoll_handle *cur_handle, *usb_handle = NULL;
    struct epoll_event events[MAX_EPOLL_EVENTS];
    int nfds, i, sleep_time;

    struct timeval tv;
    uint64_t cur_time;
    struct backend_timeout_handle *timeout;

    while(1){
        usb_handle = NULL;
        timeout = del->timeout_list.lh_first;
        gettimeofday(&tv, NULL);
        cur_time = (tv.tv_sec * 1e3) + (tv.tv_usec / 1e3);
       
        if (timeout != NULL) {
            if (cur_time > timeout->timeout_clock)
                sleep_time = 0;
            else
                sleep_time = timeout->timeout_clock - cur_time;
        } else {
            sleep_time = -1;
        }

		nfds = epoll_wait(del->efd, events, MAX_EPOLL_EVENTS, sleep_time);

		if (nfds < 0)
			continue;

        //TODO: Make sure the order of processing is safe wrt event caching and
        //so on. I can't think of any problems right now, since we will not for
        //example free a device in the internal libusb_list. So a USB event will
        //always work as intended, only difference is that event might be
        //removed from list, but our code should handle that
        
        //No callbacks have been called between last timeout check and here, so
        //I can recycle timeout value
        if (timeout != NULL)
            backend_event_loop_run_timers(del);

        for(i=0; i<nfds; i++) {
            cur_handle = events[i].data.ptr;
            
            if (cur_handle->libusb_fd) {
                usb_handle = cur_handle;
                continue;
            } else {
                cur_handle->cb(cur_handle->data, cur_handle->fd, events[i].events);
            }
        }

        //We should only run usb_handle once per loop. Keeping it here makes it
        //easier also when I remove descriptors
        if (usb_handle)
            usb_handle->cb(usb_handle->data, usb_handle->fd, 0);

        if (del->itr_cb != NULL)
            del->itr_cb(del->itr_data);
    }
}
