#!/bin/bash

PATH=$PATH:/usr/local/bin

domain=`hostname`
host=${domain%%.*}

mqtt_trigger_tasmota.py "$host off"

# magewell2ts -b 0 -i 1 -s 100 -w /home/mythtv/etc/EDID/EcoCaptureHDMI-4k-HDR-Atmos.bin
magewell2ts -b 1 -i 1 -s 100 -w /home/mythtv/etc/EDID/ProCaptureHDMI-Atmos.bin
magewell2ts -b 1 -i 2 -s 100 -w /home/mythtv/etc/EDID/ProCaptureHDMI-Atmos.bin
magewell2ts -b 1 -i 3 -s 100 -w /home/mythtv/etc/EDID/ProCaptureHDMI-Atmos.bin
magewell2ts -b 1 -i 4 -s 100 -w /home/mythtv/etc/EDID/ProCaptureHDMI-Atmos.bin

sleep 2

mqtt_trigger_tasmota.py "$host on"

