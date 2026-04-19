#!/usr/bin/env -S retext --preview
[//]: # (Install retext from your distribution then ./README.md will look prettier.)

# release/v1
* Supports h264_nvenc and hevc_nvenc for video encoding
* If the encoder cannot keep up, video frames are replicated by nvenc
* Does not support audio or video changes midstream

# release/v2
* Adds support for h264_qsv and hevc_qsv for video encoding
* A buffer use used for video frames which eliminates any dropped frames
* Adds support for handling audio and video changes midstream. Video changes are not well tested. Some audio changes can still result in glitches.
* QSV support is far better than NVENC support. NVENC can keep application from terminating.

# release/v2.1
* Much better support for midstream audio changes.
* Issues with NVENC not allowing application to terminate cleanly still exists.

# release/v3
* Adds (basic) support for Magewell Eco capture cards.

# release/v4
* Significant rewrite of video buffer management.
* With Intel GPUs, a memcpy has been eliminated resulting in significantly lower CPU usage. This allows for 4Kp60 HDR even at high bitrates.
* nVidia GPUs still work, but still use the old buffer management and only work at lower quality profiles.
* This version does a very good job at detecting if any frames are being dropped, but that does not happen very often with Intel GPUs.
* NOTE: 4kp60 HDR at higher bitrates does not work well with the current mythexternrecorder which ships with MythTV v36 (which I wrote). Instead use https://github.com/jpoet/myth-genericrecorder.
