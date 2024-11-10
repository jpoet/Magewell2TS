#!/bin/bash

# Here is template code for using ffmpeg with intel QSV AV1. The three
# lines starting with -b:v 3M \ control the resultant file size. Buff
# needs to be double that and init needs to be half. I found 3M to
# produce equivalent 1080p quality (in my eyes) to the best HEVC. You
# will need more bandwidth for bigger frames like 4K.

# av1 does not work in a .ts ?

ffmpeg -hide_banner -stats -y \
-probesize 1G -analyzeduration 1G  \
-init_hw_device vaapi=va:,driver=iHD,kernel_driver=i915 \
-init_hw_device qsv=qs@va \
-hwaccel qsv \
-hwaccel_output_format qsv \
-qsv_device /dev/dri/renderD129 \
-i "$1" \
-vf 'format=nv12|qsv,hwupload=extra_hw_frames=40,vpp_qsv=async_depth=4:denoise=10:detail=10' \
-map 0:v \
-map 0:a \
-c:v av1_qsv \
-preset veryslow \
-look_ahead_depth 40 \
-b:v 3M \
-bufsize 6M \
-rc_init_occupancy 1.5M \
-adaptive_i 1 \
-adaptive_b 1 \
-b_strategy 1 -bf 7 \
-c:a copy \
-f matroska "Output File - AV1.mkv"

# -f mpegts "Output File - AV1.ts"
