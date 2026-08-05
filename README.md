#!/usr/bin/env -S retext --preview

[//]: # (Install retext from your distribution then ./README.md will look prettier.)

# Magewell PRO/ECO capture to Transport Stream

This application reads audio and video from a Magewell PRO or ECO PCIe capture card and muxes them into a Transport Stream.

If bitstream audio is detected it will be muxed directly into the resulting Transport Stream. LPCM audio will be encoded as AC3 and then muxed.

Both AC3 and EAC3 (5.1) are supported if the source device outputs them as a bitstream.

In theory, more than two channels of LPCM should work, but has not been tested.

The Magewell driver provides V4L2 and ALSA interfaces to the card. This application by-passes those interfaces and talks directly to it via the Magewell API. A big advantage to this is you don't have to figure out which /dev/videoX or ALSA "device" is needed to make it work. The other advantage is that a raw bitstream can be captured. Unfortunately, the Magewell API depends on ALSA so we have to link it even though it is not used.

----
## Caveats

The Magewell PRO and ECO capture cards capture raw audio and video. The video (at least) needs compressed and it is up to the Linux PC to do that. The only practical way of accomplishing this is with GPU assist. Intel QSV and nVidia nvenc are supported. I don't test with nVidia very often, so there may be times when that is broken -- please let me know.

Eco cards are noticably weaker than Pro cards, and not as good at handling signal changes.

***
## Magewell driver
The Magewell PRO driver can be found here:
[https://www.magewell.com/downloads/pro-capture#/driver/linux-x86](https://www.magewell.com/downloads/pro-capture#/driver/linux-x86)

The Magewell ECO driver can be found here:
[https://www.magewell.com/downloads/eco-capture#/driver/linux-x86](https://www.magewell.com/downloads/eco-capture#/driver/linux-x86)

In the past, the drivers listed on the official Magewell download page have been for Ubuntu kernels and may or may not work with other distributions like Fedora. However, I have found Magewell to be very responsive with driver requests for Fedora when the official driver doesn't work. They usually give me a new Fedora driver within 24 hours of opening a ticket, but it will sometimes take 48 hours. ProCapture 1.3.4429 works well with Fedora 43 so they may be consolidating thei Linux driver support.

### Install the driver:
```bash
mkdir -p ~/src/Magewell
cd ~/src/Magewell
gtar -xzvf ~/Downloads/ProCaptureForLinux_4425.tar.gz
cd ProCaptureForLinux_4425/
sudo ./install.sh
```

With newer kernels, it may be necessary to add "ibt=off" to the kernel parameters:
```
sudo grubby --update-kernel=ALL --args="ibt=off"
sudo grub2-mkconfig -o /boot/grub2/grub.cfg
```

On Fedora it might be useful to lock in a "long term" kernel:
[https://copr.fedorainfracloud.org/coprs/kwizart/kernel-longterm-6.12](https://copr.fedorainfracloud.org/coprs/kwizart/kernel-longterm-6.12)

----
### Testing the Magewell driver using ALSA and V4L

Find ALSA audio input:
```bash
arecord -L
```
Pick an audio input
```bash
export AUDIO=sysdefault:CARD=HDMI_3
```
After you figure out which video device is appropriate, select it:
```bash
export VIDEO=/dev/video3
```

The following command should work to capture audio and video even with AC3 as long as you have loaded an appropriate EDID onto the Magewell card:
```bash
ffmpeg -ac 2 -f alsa -i $AUDIO -c:a copy -f wav - | ffmpeg -f wav -i pipe:0 -f v4l2 -i $VIDEO -c:v libx264 -map 0:a -map 1:v cap.ts
```

***
# Installing this application

## Building
The Magewell SDK can be found here:
[https://www.magewell.com/sdk](https://www.magewell.com/sdk)

Download the Linux version. Then unpacket it
```bash
mkdir -p ~/src/Magewell/
cd ~/src/Magewell/
gtar -xzvf ~/Download/Magewell_Capture_SDK_Linux_3.3.1.1515.tar.gz

```
Along side the Magewell SDK directory, grab the source for this application:
```bash
cd ~/src/Magewell/
git clone https://github.com/jpoet/Magewell2TS.git
```
If you place the Magewell2TS source somewhere else, you will need to edit Magewell2TS/helpers/FindMagewell.cmake and teach it how to find the Magewell SDK.

### Dependencies
#### Fedora:
```bash
sudo dnf install -y cmake gcc gcc-c++ libstdc++-devel libv4l-devel patch kernel-devel alsa-lib-devel libv4l-devel systemd-devel
```
FFmpeg
```bash
sudo dnf install ffmpeg-devel
```
##### Intel GPU and oneVPL:
```bash
sudo dnf install libvpl-devel intel-media-driver libvpl-tools
```
Verify that oneVPL is installed correctly:
```
vpl-inspect
```

##### nVidia

For nVidia GPU you will want to have the closed source driver installed as well as cuda libs. For example:
```bash
sudo dnf install https://download1.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm https://download1.rpmfusion.org/nonfree/fedora/rpmfusion-nonfree-release-$(rpm -E %fedora).noarch.rpm`
sudo dnf install akmod-nvidia xorg-x11-drv-nvidia-cuda xorg-x11-drv-nvidia-cuda-libs vdpauinfo
```

#### Ubuntu:
```bash
sudo apt-get install build-essential libv4l-dev cmake libudev-dev nvidia-cuda-toolkit
```
FFmpeg
```bash
apt-get install ffmpeg-dev
```
##### Intel GPU and onvVPL:
```bash
sudo apt-get install intel-media-va-driver-non-free libmfx1 intel-opencl-icd libmfx-gen1.2 libvpl-dev onevpl-tools
```

Verify that oneVPL is installed correctly:
```
vpl-inspect
```
##### nVidia
```
sudo apt-get install nvidia-cuda-toolkit
```


If you have trouble with oneVPL, check out [https://www.intel.com/content/www/us/en/developer/articles/guide/onevpl-installation-guide.html](https://www.intel.com/content/www/us/en/developer/articles/guide/onevpl-installation-guide.html) for more information.

## Building the application
```bash
cd ~/src/Magewell/Magewell2TS
```
Use CMake to compile and install:
```bash
mkdir build
cd build
cmake ..
make
sudo make install
```

----
## Running
The application provides help via --help or -h:
```bash
magewell2ts -h
magewell2ts --list
magewell2ts -i 1 -m -c hevc_qsv -d renderD129 | mpv -

magewell2ts -i 1 -m -c hevc_qsv -d renderD129 | mpv - --cache=no --demuxer-readahead-secs=0 --video-sync=desync
```

----
## MythTV
The easiest way to use this with MythTV is to create an "External Recorder" configuration file. Something like (/home/mythtv/etc/magewell-2.conf):
```
[VARIABLES]
BOARD=1
INPUT=1
DEVICE=onn2
TUNER=/usr/local/bin/adb-control --device %DEVICE%
CODEC=hevc_qsv --device renderD129 -q 22 --lookahead 50 -p010
#CODEC=hevc_nvenc -q 22

[RECORDER]
# The recorder command to execute.  %URL% is optional, and
# will be replaced with the channel's "URL" as defined in the
# [TUNER/channels] (channel conf) configuration file
command="/usr/local/bin/magewell2ts -b %BOARD% -i %INPUT% -m -c %CODEC%"

cleanup="%TUNER% --reset"
desc="%DEVICE%-%BOARD%-%INPUT%"

[TUNER]
# An optional CONF file which provides channel details.  If it does not
# exist, then channel changes are not supported.
#channels=/home/mythtv/etc/adb-channels.conf

# If [TUNER/command] is provided, it will be executed to "tune" the
# channel. A %URL% parameter will be substituted with the "URL" as
# defined in the [TUNER/channels] configuration file

command=%TUNER% --sourceid %SOURCEID% --channum %CHANNUM% --recordid %RECORDID%
```

Then configure a MythTV External Recorder capture card with an appropriate command such as:
```bash
/usr/local/bin/mythexternrecorder --conf /home/myth/etc/magewell-1-1.conf
```

----
## EDID
If you want to allow bitstream AC3 and/or EAC3 then a different EDID needs written to the Magewell card. This data does not survive a reboot, though, so you may want to setup systemd to load the EDID. This can be done in the same service file used to start mythbackend, for example:

Create a service file (/etc/systemd/system/mythbackend.service):
```
[Unit]
Description=MythTV backend service
Wants=dev-hvr2250_1.device dev-hvr2250_2.device
After=full-internet.target mysqld.service dev-hvr2250_1.device dev-hvr2250_2.device
OnFailure=notify-email@%i.service

[Service]
Type=simple
Environment=MYTHCONFDIR=/home/mythtv/.mythtv
Environment=HOME=/home/mythtv
LimitCORE=infinity
User=mythtv
PermissionsStartOnly=true
ExecStartPre=/usr/local/bin/magewell2ts --wait-for 4 -i 1 -w /home/mythtv/etc/EDID/Magewell-1080p-Default+Atmos.bin
ExecStartPre=/usr/local/bin/magewell2ts --wait-for 4 -i 2 -w /home/mythtv/etc/EDID/Magewell-1080p-Default+Atmos.bin
ExecStartPre=/usr/local/bin/magewell2ts --wait-for 4 -i 3 -w /home/mythtv/etc/EDID/Magewell-1080p-Default+Atmos.bin
ExecStartPre=/usr/local/bin/magewell2ts --wait-for 4 -i 4 -w /home/mythtv/etc/EDID/Magewell-1080p-Default+Atmos.bin

ExecStart=/usr/local/bin/mythbackend -q --syslog none --logpath /var/log/mythtv -v channel,record
RestartSec=5
Restart=on-failure

[Install]
WantedBy=multi-user.target
```
That will load the eac3 EDID.


## Multi GPU systems

If you have both Intel and nVidia GPUs enabled in the same system, it can result in confusion:

```bash
$ vainfo
Trying display: wayland
libva info: VA-API version 1.16.0
libva info: User environment variable requested driver 'nvidia'
libva info: Trying to open /usr/lib64/dri/nvidia_drv_video.so
libva info: va_openDriver() returns -1
vaInitialize failed with error code -1 (unknown libva error),exit
```
To get it to show for the intel, you must use an env variable of either i965 or iHD:
```bash
$ LIBVA_DRIVER_NAME=iHD vainfo
Trying display: wayland
libva info: VA-API version 1.16.0
libva info: User environment variable requested driver 'iHD'
libva info: Trying to open /usr/lib64/dri/iHD_drv_video.so
libva info: Found init function __vaDriverInit_1_16
libva info: va_openDriver() returns 0
vainfo: VA-API version: 1.16 (libva 2.16.0)
vainfo: Driver version: Intel iHD driver for Intel(R) Gen Graphics - 22.5.4 ()
vainfo: Supported profile and entrypoints
      VAProfileNone                   : VAEntrypointVideoProc
      VAProfileNone                   : VAEntrypointStats
... etc.
```

## Intel Arc
The Intel Arc GPUs may need a new version of the linux firmware
```bash
git clone git://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git
```

If you want to update the firmware on the Arc itself, this might help: [https://forum.level1techs.com/t/remember-to-update-your-intel-arc-firmware-on-linux/208736](https://forum.level1techs.com/t/remember-to-update-your-intel-arc-firmware-on-linux/208736)

# Optimizing

## Real Time Threads
If you want to use the --realtime option, the user running magewell2ts needs to be configure with "real time" priority. For example, create the file
```
/etc/security/limits.d/99-mythtv-realtime.conf
```
with
```
mythtv   soft   rtprio   99
mythtv   hard   rtprio   99
mythtv   soft   nice     -20
mythtv   hard   nice     -20
```
to allow the mythtv user to request real time priority.

By default the two threads (audio and video) responsible for capturing from the Magewell card are given a higher than normal priority with the --realtime option.

## CPU cores
If you are capturing multiple streams at the same time, it can be beneficial to make sure the load is well balanced across the CPU cores.

Allow all cores to handle interrupts:
```
sudo systemctl enable --now irqbalance
```

Pin each capture to a (set) of cores to make sure they don't all try to use the same:
```
# Instance 1: Runs all its threads only on Cores 2 and 3
taskset -c 2,3 ./magewell2ts -i 1 -m

# Instance 2: Runs all its threads only on Cores 4 and 5
taskset -c 4,5 ./magewell2ts -i 2 -m

# Instance 3: Runs all its threads only on Cores 6 and 7
taskset -c 6,7 ./magewell2ts -i 3 -m

# Instance 4: Runs all its threads only on Cores 8 and 9
taskset -c 8,9 ./magewell2ts -i 4 -m
```
In this example, cores 0,1 are left to the operating system. Note that when choosing cores, avoid E(fficiency) cores.

Optionally promote all the the magewell2ts threads to a higher priority:
```
sudo nice -n -10 taskset -c 0,1 ./magewell2ts -i 1 -m
```
or, if you have given the user real-time permissions:
```
nice -n -10 taskset -c 0,1 ./magewell2ts -i 1 -m
```
