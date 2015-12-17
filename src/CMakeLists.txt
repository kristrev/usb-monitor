#Standard stuff
cmake_minimum_required(VERSION 2.6)
project(usb-monitor)

set(CMAKE_C_FLAGS "-O1 -Wall -g")
set(LIBS usb-1.0 json-c)

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_VERSION_MAJOR "0")
set(CPACK_PACKAGE_VERSION_MINOR "1")
set(CPACK_PACKAGE_VERSION_PATCH "0")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "support@celerway.com")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${PROJECT_SOURCE_DIR}/../files/postinst;${PROJECT_SOURCE_DIR}/../files/prerm")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libusb-dev, libjson-c-dev")
include(CPack)

if (NOT TARGET_OWRT)
    install(FILES ${PROJECT_SOURCE_DIR}/../files/usb-monitor.logrotate DESTINATION
            /etc/logrotate.d PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
            RENAME usb-monitor)
    install(FILES ${PROJECT_SOURCE_DIR}/../files/usb-monitor.service DESTINATION
            /lib/systemd/system/ PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
            RENAME usb-monitor.service)
endif()

add_executable(usb_monitor
               usb_helpers.c
               usb_monitor.c
               usb_monitor_lists.c
               usb_monitor_callbacks.c
               generic_handler.c
               ykush_handler.c
               gpio_handler.c
               backend_event_loop.c
               socket_utility.c
               http_parser.c
               http_utility.c
               usb_monitor_client.c)
target_link_libraries(usb_monitor ${LIBS})
install(TARGETS usb_monitor RUNTIME DESTINATION bin)
