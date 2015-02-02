#ifndef USB_LOGGING_H
#define USB_LOGGING_H

#include <stdio.h>
#include <time.h>

#define USB_LOG_PREFIX "[%.2d:%.2d:%.2d %.2d/%.2d/%d]: "
#define USB_DEBUG_PRINT2(fd, ...){fprintf(fd, __VA_ARGS__);fflush(fd);}
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

#endif
