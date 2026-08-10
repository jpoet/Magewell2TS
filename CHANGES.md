#!/usr/bin/env -S retext --preview
[//]: # (Install retext from your distribution then ./README.md will look prettier.)

# release/v1

* Supports `h264_nvenc` and `hevc_nvenc` for video encoding.
* If the encoder cannot keep up, video frames are replicated by `nvenc`.
* Does not support audio or video changes midstream.

# release/v2

* Adds support for `h264_qsv` and `hevc_qsv` for video encoding.
* A buffer is used for video frames, which eliminates dropped frames.
* Adds support for handling audio and video changes midstream. (Note: Video changes are not well-tested; some audio changes can still result in glitches).
* QSV support is significantly better than NVENC support. NVENC can occasionally prevent the application from terminating cleanly.

# release/v2.1

* Much better support for midstream audio changes.
* Issues with NVENC not allowing the application to terminate cleanly still exist.

# release/v3

* Adds basic support for Magewell ECO capture cards.

# release/v4

* Significant rewrite of video buffer management.
* With Intel GPUs, a `memcpy` operation has been eliminated, resulting in significantly lower CPU usage. This allows for 4Kp60 HDR even at high bitrates.
* nVidia GPUs still function but use the legacy buffer management layout and only work at lower quality profiles.
* This version accurately detects if any frames are being dropped, though that rarely occurs with Intel GPUs.
* **NOTE:** 4Kp60 HDR at higher bitrates does not perform well with the current `mythexternrecorder` utility shipped with MythTV v36. Instead, please use [myth-genericrecorder](https://github.com/jpoet/myth-genericrecorder).

# release/v5

* Significant architectural rewrite of almost all components.
* HDR color parameters are now handled more correctly.
* Instead of using FFmpeg to detect the incoming audio stream, a custom IEC61937 and E/AC3 parser is used, allowing for much faster start-up times.
* More threads are utilized to optimize CPU load balance. ECO capture cards running on lower-end hardware are now more likely to maintain pace with 4Kp60 HDR.
* Using the `-v 4` flag will log buffer usage statistics.
