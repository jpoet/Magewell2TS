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
