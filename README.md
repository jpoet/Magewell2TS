#!/usr/bin/env -S retext --preview

[//]: # (Install retext from your distribution then ./README.md will look prettier.)
 
# Magewell PRO capture to Transport Stream

This application reads audio and video from a Magewell PRO capture card and muxes them into a Transport Stream.

If bitstream audio is detected it will be muxed directly into the resulting Transport Stream. LPCM audio will be encoded as AC3 and then muxed.

Both AC3 and EAC3 (5.1) are supported if the source device outputs them as a bitstream. EAC3 7.1 is not supported because FFmpeg does not understand >6 channels of audio in EAC3.

More than two channels of LPCM are not currently supported. Adding such support is possible but I do not have a source device to test with.

The Magewell driver provides V4L2 and ALSA interfaces to the card. This application by-passes those interfaces and talks directly to it via the Magewell API. A big advantage to this is you don't have to figure out which /dev/videoX or ALSA "device" is needed to make it work. The other advantage is that a raw bitstream can be captured. Unfortunately, the Magewell API depends on ALSA so we have to link it even though it is not used.
 
----
## Caveats

The Magewell PRO capture cards capture raw audio and video. The video (at least) needs compressed and it is up to the Linux PC to do that. The only practical way of accomplishing this is with GPU assist. Intel QSV and nVidia nvenc are supported. I don't test with nVidia very often, so there may be times when that is broken -- please let me know.

***
## Magewell driver
The Magewell driver can be found here:
[https://www.magewell.com/downloads/pro-capture#/driver/linux-x86](https://www.magewell.com/downloads/pro-capture#/driver/linux-x86)

The drivers listed on the official Magewell download page are for Ubuntu kernels and may or may not work with other distributions like Fedora. However, I have found Magewell to be very responsive with driver requests for Fedora when the official driver doesn't work. They usually give me a new Fedora driver within 24 hours of opening a ticket, but it will sometimes take 48 hours.


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
sudo grubby --update-kernel=All --args="ibt=off"
sudo grub2-mkconfig -o /boot/grub2/grub.cfg
```

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
gtar -xzvf ~/Download/Magewell_Capture_SDK_Linux_3.3.1.1505.tar.gz

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
sudo dnf install -y make gcc gcc-c++ libstdc++-devel libv4l-devel patch kernel-devel alsa-lib-devel v4l-utils-devel-tools systemd-devel
```
FFmpeg
```bash
sudo dnf install ffmpeg-devel
```
For Intel GPU and oneVPL:
```bash
sudo dnf install libvpl-devel intel-media-driver
```
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
For Intel GPU and onvVPL:
```bash
sudo apt-get install intel-media-va-driver-non-free libmfx1 intel-opencl-icd libmfx-gen1.2 libvpl-dev
```
For nVidia GPU:
```
sudo apt-get install nvidia-cuda-toolkit
```


If you have trouble with oneVPL, check out [https://www.intel.com/content/www/us/en/developer/articles/guide/onevpl-installation-guide.html](https://www.intel.com/content/www/us/en/developer/articles/guide/onevpl-installation-guide.html) for more information.


If you want bitstream audio to work, then a minimum of FFmpeg 6.1 is needed. At least 7.1 is prefered. You may need to build FFmpeg from source if your distribution does not provide recent enough packages.


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
ExecStartPre=/usr/local/bin/magewell2ts --wait-for 4 -i 1 -s 100 -w /home/mythtv/etc/EDID/ProCaptureHDMI-EAC3.bin
ExecStartPre=/usr/local/bin/magewell2ts --wait-for 4 -i 2 -s 100 -w /home/mythtv/etc/EDID/ProCaptureHDMI-EAC3.bin
ExecStartPre=/usr/local/bin/magewell2ts --wait-for 4 -i 3 -s 100 -w /home/mythtv/etc/EDID/ProCaptureHDMI-EAC3.bin
ExecStartPre=/usr/local/bin/magewell2ts --wait-for 4 -i 4 -s 100 -w /home/mythtv/etc/EDID/ProCaptureHDMI-EAC3.bin

ExecStart=/usr/local/bin/mythbackend -q --syslog none --logpath /var/log/mythtv -v channel,record
RestartSec=5
Restart=on-failure

[Install]
WantedBy=multi-user.target
```
That will load the eac3 EDID and set the volume level to 100.


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
