#!/bin/bash

PATH=$PATH:/usr/local/bin

domain=`hostname`
host=${domain%%.*}

mqtt_trigger_tasmota.py "$host off"

magewell2ts -b 0 -i 1 -w /home/mythtv/etc/EDID/Magewell-4K-Default+Atmos.bin
magewell2ts -b 1 -i 1 -w /home/mythtv/etc/EDID/Magewell-1080p-Default+AC3.bin
magewell2ts -b 1 -i 2 -w /home/mythtv/etc/EDID/Magewell-1080p-Default+Atmos.bin
magewell2ts -b 1 -i 3 -w /home/mythtv/etc/EDID/Magewell-1080p-Default+Atmos.bin
magewell2ts -b 1 -i 4 -w /home/mythtv/etc/EDID/Magewell-1080p-Default+Atmos.bin

sleep 2

mqtt_trigger_tasmota.py "$host on"

