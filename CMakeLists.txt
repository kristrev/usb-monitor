#Standard stuff
cmake_minimum_required(VERSION 2.6)
project(usb_monitor)

set(CMAKE_C_FLAGS "-O1 -Wall -g")
set(LIBS usb-1.0 json-c)

add_executable(usb_monitor
               usb_helpers.c
               usb_monitor.c
               usb_monitor_lists.c
               ykush_handler.c
               gpio_handler.c
               backend_event_loop.c)
target_link_libraries(usb_monitor ${LIBS})
install(TARGETS usb_monitor RUNTIME DESTINATION bin)
