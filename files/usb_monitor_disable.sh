#!/bin/sh

CURL_PATH=`which curl`
USB_URL="http://localhost:88/usbmonitor"

#ash does not support arrays, use set and add the ports to the command line
#arguments of the shell
set -- "2-1-1" "2-1-2" "2-1-3"

JSON_STRING="{'ports':["
NUM_PORTS=$#


for port in $@;
do
    if [ "$NUM_PORTS" -eq 1 ];
    then
        JSON_STRING="$JSON_STRING{'path':'$port','cmd':2}"
    else
        JSON_STRING="$JSON_STRING{'path':'$port','cmd':2},"
    fi

    NUM_PORTS=$(( $NUM_PORTS-1 ))
done

JSON_STRING="$JSON_STRING]}"

$CURL_PATH -X POST -d "$JSON_STRING" $USB_URL
