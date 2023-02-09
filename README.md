 
# Magewell PRO capture to Transport Stream

This application reads audio and video from a Magewell PRO capture card and muxes them into a Transport Stream.

The audio is scanned for a S/PDIF header and if found then the audio is treated as a bitstream which is muxed directly into the resulting Transport Stream. If the S/PDIF header is not found, then the audio is assumed to be LPCM and is encoded as AAC which is then muxed into the Transport Stream

Both AC3 and EAC3 are supported if the source device outputs them as a bitstream.

More than two channels of PCM are not currently supported. Adding such support should be easy but I do not have a source device to test with.

The Magewell driver provides V4L2 and ALSA interfaces to the
card. This application by-passes those interfaces and talks directly
to it via the Magewell API. A big advantage to this is you don't have
to figure out which /dev/videoX or ALSA "device" is needed to make it
work. The other advantage is that a raw bitstream can be captured.

----
## Caveats

The Magewell PRO capture cards capture raw audio and video. The video (at least) needs compressed and it is up to the Linux PC to do that. The only practical way of accomplishing this is with GPU assist. Currently, only yuv420p is supported, which generally means only nVidia GPUs.

nVidia arbitrarily only allows two simultaneous encoders on "consumer" level cards. Fortunately that limitation can be removed:

https://github.com/keylase/nvidia-patch

----
## Magewell driver
The Magewell driver can be found here:
```
https://www.magewell.com/downloads/pro-capture#/driver/linux-x86
```

Install the driver:
```
gtar -xzvf ~/Downloads/ProCaptureForLinux_4236.tar.gz
cd ProCaptureForLinux_4236/
sudo ./install.sh
```

#### Testing the Magewell driver

Find ALSA audio input:
```
arecord -L
```
Pick an audio input
```
export AUDIO=sysdefault:CARD=HDMI_3
```
After you figure out which video device is appropriate, select it:
```
export VIDEO=/dev/video3
```

The following command should work to capture audio and video even with AC3 as long as you have loaded an appropriate EDID onto the Magewell card:
```
ffmpeg -ac 2 -f alsa -i $AUDIO -c:a copy -f wav - | ffmpeg -f wav -i pipe:0 -f v4l2 -i $VIDEO -c:v libx264 -map 0:a -map 1:v cap.ts
```
----
## Installing this application

## Building
The Magewell SDK can be found here:
```
https://www.magewell.com/sdk
```
Download the Linux version. Then unpacket it
```
mkdir -p ~/src/Magewell/
cd ~/src/Magewell/
gtar -xzvf ~/Download/Magewell_Capture_SDK_Linux_3.3.1.1313.tar.gz
```

In the Magewell SDK directory, grab the source for this application:
```
cd ~/src/Magewell/Magewell_Capture_SDK_Linux_3.3.1.131
git clone https://github.com/jpoet/Magewell2TS.git
```

----
## Dependencies
Fedora:
```
sudo dnf install -y make gcc gcc-c++ libstdc++-devel libv4l-devel patch kernel-devel alsa-lib-devel v4l-utils-devel-tools systemd-devel
```

Ubuntu:
```
sudo apt-get install build-essential libv4l-dev cmake libudev-dev nvidia-cuda-toolkit
```

FFmpeg is also required. Due to this project avoiding deprecated elements in FFmpeg, at least version 5.1 is required. If your platform supplies a development package of the 5.1 version then you can use it, otherwise you will have to build from source. You will need to build from source if you want EAC3 detection to work since a patch must be applied.

### Building FFmpeg

Remember to include support for nVidia GPUs when building FFmpeg.

Fedora has a headers package which makes this easy:
```
sudo dnf install -y nv-codec-headers
```

Instruction for building FFmpeg can be found here:
```
https://trac.ffmpeg.org/wiki/CompilationGuide
```

If your source provides eac3 then you will need to patch the FFmpeg source before compiling to enable detection:
```
cd ffmpeg
patch -p1 < ~/src/Magewell/Magewell_Capture_SDK_Linux_3.3.1.131/Magewell2TS/Patches/ffmpeg_IEC61937_EAC3.patch

```

### Building the application
```
cd ~/src/Magewell/Magewell_Capture_SDK_Linux_3.3.1.131/Magewell2TS
```

Use CMake to compile and install:
```
mkdir build
cd build
cmake ..
make
sudo make install
```

If you want to use a different tree structure, edit Magewell2TS/helpers/FindMagewell.cmake and teach it how to find the Magewell SDK.

----
## Running
The application provides help via --help or -h:
```
magewellpro2ts -h
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
```
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
```
systemctl enable MagewellEDID.service
```
Those commands will now be invoked on each boot.
