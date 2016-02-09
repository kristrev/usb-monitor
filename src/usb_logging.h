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

#ifndef USB_LOGGING_H
#define USB_LOGGING_H

#include <stdio.h>
#include <time.h>
#include <syslog.h>

#define USB_LOG_PREFIX "[%.2d:%.2d:%.2d %.2d/%.2d/%d]: "
#define USB_DEBUG_PRINT2(fd, ...){fprintf(fd, __VA_ARGS__);fflush(fd);}
#define USB_DEBUG_SYSLOG(priority, ...){syslog(LOG_MAKEPRI(LOG_DAEMON, priority), __VA_ARGS__);}
//The ## is there so that I dont have to fake an argument when I use the macro
//on string without arguments!
#define USB_DEBUG_PRINT(fd, _fmt, ...) \
    do { \
    time_t rawtime; \
    struct tm *curtime; \
    time(&rawtime); \
    curtime = gmtime(&rawtime); \
    USB_DEBUG_PRINT2(fd, USB_LOG_PREFIX _fmt, curtime->tm_hour, \
        curtime->tm_min, curtime->tm_sec, curtime->tm_mday, \
        curtime->tm_mon + 1, 1900 + curtime->tm_year, \
        ##__VA_ARGS__);} while(0)

#define USB_DEBUG_PRINT_SYSLOG(ctx, priority, _fmt, ...) \
    do { \
    time_t rawtime; \
    struct tm *curtime; \
    time(&rawtime); \
    curtime = gmtime(&rawtime); \
    if (ctx->use_syslog) \
        USB_DEBUG_SYSLOG(priority, _fmt, ##__VA_ARGS__); \
    USB_DEBUG_PRINT2(ctx->logfile, USB_LOG_PREFIX _fmt, \
        curtime->tm_hour, \
        curtime->tm_min, curtime->tm_sec, curtime->tm_mday, \
        curtime->tm_mon + 1, 1900 + curtime->tm_year, \
        ##__VA_ARGS__);} while(0)
#endif
