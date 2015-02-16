USB Monitor
===========

USB Monitor is a tool designed to monitor and restart USB devices connected to
hubs or ports that support power switching. This is useful if you for example
have a device placed in a remote location, and which depends on one or more USB
devices to function correctly.  USB Monitor can be run as a daemon and provides
a small REST API for resetting individual ports. Checking if a device is alive
is done using the GET\_STATUS USB control message. Control messages have the
advantage that they do not interfere with the normal operation of the device,
and a reply is always expected.

The tool currently supports ports where the power is controlled through GPIO, as
well as the [Yepkit YKUSH hub](https://www.yepkit.com/products/ykush). Support
for controlling power using the standardized Clear/SetPortFeature USB-messages
is also implemented, but currently not implemented. Some more work on cascading
hub support is needed before generic per-port power control can be used (on hubs
that support it).  Pull requests are welcome :)

A concrete use-case is the EU-funded research-project MONROE, which funded the
development of the tool. The project will build a testbed that enables users to
measure different networks across Europe, with a focus on mobile broadband. Each
measurement will be equipped with multiple USB MBB-modems, which needs to be
power-cycled in case of for example a firmware crash.

Requirements
------------

USB Monitor depends on libusb and is compiled using cmake. USB Monitor must be
run as root in order to work.

Parameters
----------

USB monitor supports the following command line parameters:

* -o : Path to log file. This is optional, if no log file is provided then
  stderr is used.
* -c : Path to configuration file. Also optional. This is currently used to
  provide a mapping between GPIO numbers and USB paths. See archer\_c5.conf for
  an example.
* -d : Run USB Monitor as daemon.

REST API
--------

The REST API currently supports one GET and one POST operation. For now, we
ignore the URL and only look at HTTP method.

GET is used to get the status, vid and pid of the ports. An example of the
output is:
{"ports":[{"path":"3-1-2-5-4-3","mode":1,"vid":4817,"pid":5382}]}

In order to restart one or more devices, a POST request must be sent. The syntax
is as follows:
{"ports": [{"path":"3-1-2-5-4-3", "cmd": 1}]}

The reply is the same as for the GET request. Only the root-user can currently
send HTTP requests to USB Monitor.

Adding new handlers
-------------------

Adding new handlers if fairly straight forward. A handle needs to implement
three callbacks:

* print\_port() : Output details about the port
* update\_port() : Used to reset, or start resetting port. Will most likely be
  called two times.
* handle\_timeout() : The code is event-loop based, so a handler needs to handle
  timeouts. Examples of where timeouts are used, are for sending GET\_STATUS or
  handling the second part of a reset.

The easiest is to look at either the YKUSH- or GPIO-handlers.
