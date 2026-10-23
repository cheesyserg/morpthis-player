#!/bin/sh
# Morpthis: stop the AirPlay receiver, whichever daemon this image carries.

killall shairport-sync 2>/dev/null
killall shairport 2>/dev/null
exit 0
