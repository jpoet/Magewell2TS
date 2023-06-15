#!/usr/bin/env -S retext --preview

[//]: # (Install retext from your distribution then ./README.md will look prettier.)
 
# Magewell PRO capture to Transport Stream

This application reads audio and video from a Magewell PRO capture card and muxes them into a Transport Stream.

The audio is scanned for a S/PDIF header and if found then the audio is treated as a bitstream which is muxed directly into the resulting Transport Stream. If the S/PDIF header is not found, then the audio is assumed to be LPCM and is encoded as AAC which is then muxed into the Transport Stream

Both AC3 and EAC3 are supported if the source device outputs them as a bitstream.

More than two channels of PCM are not currently supported. Adding such support should be easy but I do not have a source device to test with.

The Magewell driver provides V4L2 and ALSA interfaces to the
card. This application by-passes those interfaces and talks directly
to it via the Magewell API. A big advantage to this is you don't have
to figure out which /dev/videoX or ALSA "device" is needed to make it
work. The other advantage is that a raw bitstream can be captured. Unfortunately, the Magewell API depends on ALSA so we have to link it even though it is not used.

----
## Caveats

The Magewell PRO capture cards capture raw audio and video. The video (at least) needs compressed and it is up to the Linux PC to do that. The only practical way of accomplishing this is with GPU assist. Intel VAAPI and nVidia nvenc are supported.

nVidia arbitrarily only allows two simultaneous encoders on "consumer" level cards. Fortunately that limitation can be removed:

[https://github.com/keylase/nvidia-patch](https://github.com/keylase/nvidia-patch)

***
## Magewell driver
The Magewell driver can be found here:
[https://www.magewell.com/downloads/pro-capture#/driver/linux-x86](https://www.magewell.com/downloads/pro-capture#/driver/linux-x86)

Install the driver:
----
```bash
mkdir -p ~/src/Magewell
cd ~/src/Magewell
gtar -xzvf ~/Downloads/ProCaptureForLinux_4236.tar.gz
cd ProCaptureForLinux_4236/
sudo ./install.sh
```

### Testing the Magewell driver

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
## Installing this application

## Building
The Magewell SDK can be found here:
[https://www.magewell.com/sdk](https://www.magewell.com/sdk)

Download the Linux version. Then unpacket it
```bash
mkdir -p ~/src/Magewell/
cd ~/src/Magewell/
gtar -xzvf ~/Download/Magewell_Capture_SDK_Linux_3.3.1.1313.tar.gz
```

In the Magewell SDK directory, grab the source for this application:
```bash
cd ~/src/Magewell/Magewell_Capture_SDK_Linux_3.3.1.131
git clone https://github.com/jpoet/Magewell2TS.git
```
If you place the Magewell2TS source somewhere else, you will need to edit Magewell2TS/helpers/FindMagewell.cmake and teach it how to find the Magewell SDK.

### Dependencies
Fedora:
```bash
sudo dnf install -y make gcc gcc-c++ libstdc++-devel libv4l-devel patch kernel-devel alsa-lib-devel v4l-utils-devel-tools systemd-devel
```

Ubuntu:
```bash
sudo apt-get install build-essential libv4l-dev cmake libudev-dev nvidia-cuda-toolkit
```

FFmpeg is also required. Due to this project avoiding deprecated elements in FFmpeg, at least version 5.1 is required. If your platform supplies a development package of the 5.1 version then you can use it, otherwise you will have to build from source. You will need to build from source if you want EAC3 detection to work unless your distrubtions provides a package for the 'master' or 'trunk' version of FFmpeg.

### Building FFmpeg

Remember to include support for nVidia nvenc and/or Intel vaapi when building FFmpeg.

#### nvenc
Fedora has a headers package which makes it easy to install the nvenc headers:
```bash
sudo dnf install -y nv-codec-headers
```

#### vaapi
It is probably best to build Intel vaapi libs from source:
```bash
mkdir -p ~/src/Intel
cd ~/src/Intel
sudo dnf install -y libva-devel libva-intel-driver \
     libva-vdpau-driver libdrm-devel intel-media-driver
git clone git@github.com:intel/intel-vaapi-driver.git
cd intel-vaapi-driver
./autogen.sh
./configure
make -j16
sudo make install
```

#### ffmpeg
Instruction for building FFmpeg can be found here:
[https://trac.ffmpeg.org/wiki/CompilationGuide](https://trac.ffmpeg.org/wiki/CompilationGuide)

For example, under Fedora:
```bash
mkdir -p ~/src/FFmpeg
cd ~/src/FFmpeg

sudo dnf install -y nasm yasm-devel x264-devel x265-devel \
     fdk-aac-devel opus-devel libogg-devel lame-libs lame-devel \
     libvorbis-devel libvpx-devel numactl-devel faac-devel \
     ladspa-devel libass-devel libbluray-devel gsm-devel \
     opencv-devel openjpeg2-devel soxr-devel libtheora-devel \
     opencl-headers gstreamer1-vaapi-devel libchromaprint-devel \
     frei0r-devel \
     ccache gcc

sudo dnf install oneVPL-devel oneVPL-samples intel-media-driver

sudo dnf install libvdpau-devel

sudo dnf install -y intel-media-driver libva libva-utils intel-gpu-tools mesa-dri-drivers

git clone https://git.videolan.org/git/ffmpeg.git
cd ffmpeg

PKG_CONFIG_PATH="/opt/ffmpeg/lib/pkgconfig" \
    ./configure \
  --prefix="/opt/ffmpeg" \
  --pkg-config-flags="--static" \
  --extra-cflags="-I/opt/ffmpeg/include" \
  --extra-ldflags="-L/opt/ffmpeg/lib" \
  --extra-libs="-lpthread -lm" \
  --bindir="/opt/ffmpeg/bin" \
  --cc="ccache gcc" \
  --cpu=native \
  --enable-nonfree \
  --enable-gpl \
  --enable-version3 \
  --enable-libass \
  --enable-libbluray \
  --enable-libmp3lame \
  --enable-libopenjpeg \
  --enable-libopus \
  --enable-libfdk_aac \
  --enable-libtheora \
  --enable-libvpx \
  --enable-nvenc \
  --enable-vaapi \
  --enable-vdpau  \
  --enable-ladspa \
  --enable-libass  \
  --enable-libgsm \
  --enable-libsoxr \
  --enable-libx264 \
  --enable-libx265 \
  --enable-openssl \
  --enable-libvpl

  --enable-cuda \

make -j16
sudo make install

```

### Building the application
```bash
cd ~/src/Magewell/Magewell_Capture_SDK_Linux_3.3.1.131/Magewell2TS
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
magewellpro2ts -h
magewellpro2ts -i 1 -m -n | mpv -
```

----
## MythTV
The easiest way to use this with MythTV is to create an "External Recorder" configuration file. Something like (/home/mythtv/etc/magewell-2.conf):
```
[RECORDER]
# The recorder command to execute.  %URL% is optional, and
# will be replaced with the channel's "URL" as defined in the
# [TUNER/channels] (channel conf) configuration file

command="/usr/local/bin/magewellpro2ts --input 1 --mux"
cleanup="/usr/local/bin/stb-control.py --debug --stb stb1 --reset"

# Used in logging events, %ARG% are replaced from the channel info
desc="STB1-2"

[TUNER]
# An optional CONF file which provides channel details.  If it does not
# exist, then channel changes are not supported.
#channels=/home/mythtv/etc/stb-channels.conf

# If [TUNER/command] is provided, it will be executed to "tune" the
# channel. A %URL% parameter will be substituted with the "URL" as
# defined in the [TUNER/channels] configuration file

command="/usr/local/bin/stb-control.py --stb stb1 --channum %CHANNUM%"
#ondatastart="/usr/local/bin/stb-control.py --stb stb1 --left
newepisodecommand="/usr/local/bin/stb-control.py --stb stb1 --touch --channum %CHANNUM%"
```

Then configure a MythTV External Recorder capture card with an appropriate command:
```bash
/usr/local/bin/mythexternrecorder --conf /home/myth/etc/magewell-2.conf
```

----
## EDID
If you want to allow AC3 and/or EAC3 then a different EDID needs written to the Magewell card. This data does not survive a reboot, though, so you may want to setup systemd to load the EDID on each boot.

Create a service file (/etc/systemd/system/MagewellEDID.service):
```
[Unit]
Description=Load EDID and volume information into Magewell card inputs

[Service]
Type=oneshot
ExecStart=/usr/local/bin/magewellpro2ts -i 1 -s 85 -w /home/mythtv/etc/EDID/ProCaptureHDMI-EAC3.bin
ExecStart=/usr/local/bin/magewellpro2ts -i 2 -s 85 -w /home/mythtv/etc/EDID/ProCaptureHDMI-EAC3.bin
ExecStart=/usr/local/bin/magewellpro2ts -i 3 -s 85 -w /home/mythtv/etc/EDID/ProCaptureHDMI-EAC3.bin
ExecStart=/usr/local/bin/magewellpro2ts -i 4 -s 85 -w /home/mythtv/etc/EDID/ProCaptureHDMI-EAC3.bin
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
```
That will load the eac3 EDID and set the volume level to 85. Enable it:
```bash
systemctl enable MagewellEDID.service
```
Those commands will now be invoked on each boot.

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