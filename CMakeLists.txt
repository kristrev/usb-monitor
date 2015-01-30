#Standard stuff
cmake_minimum_required(VERSION 2.6)
project(usb_monitor)

set(CMAKE_C_FLAGS "-O1 -Wall -g")
set(LIBS usb-1.0)

add_executable(usb_monitor
               usb_helpers.c
               usb_monitor.c
               usb_monitor_lists.c
               ykush_handler.c
               gpio_handler.c)
target_link_libraries(usb_monitor ${LIBS})
