/*
 * magewellpro2ts
 * Copyright (c) 2022 John Patrick Poet
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *
 * Based on AudioCapture.cpp,CaptureByInput.cpp and ReadWriteEDID.cpp
 * by magewell:
 *
 * THE SOFTWARE PROVIDED BY MAGEWELL “AS IS” AND ANY EXPRESS, INCLUDING
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL
 * MAGEWELL BE LIABLE
 *
 * FOR ANY CLAIM, DIRECT OR INDIRECT DAMAGES OR OTHER LIABILITY, WHETHER
 * IN CONTRACT, TORT OR OTHERWISE, ARISING IN ANY WAY OF USING THE
 * SOFTWARE.
 *
 * CONTACT INFORMATION:
 * SDK@magewell.net
 * http://www.magewell.com/
 */

#include <chrono>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <thread>
#include <charconv>
#include <string_view>

#include <mutex>
#include <condition_variable>

#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <cerrno>
#include <csignal>

#include <MWFOURCC.h>
#include <LibMWCapture/MWCapture.h>
#include <libavutil/rational.h>
#include <libavutil/pixfmt.h>

#include "OutputTS.h"
#include "version.h"

using namespace std;

OutputTS*         g_out2ts = nullptr;
int               g_verbose   = 1;
std::atomic<bool> g_running(true);
std::atomic<bool> g_reset(false);
int  g_frame_ms = 17;

void signal_handler(int signum)
{
    if (signum == SIGHUP || signum == SIGUSR1)
    {
        cerr << "\n\nResetting audio.\n" << endl;
        g_reset.store(true);
    }
    else if (signum == SIGINT || signum == SIGTERM)
    {
        if (g_out2ts)
            g_out2ts->Shutdown();
        g_running.store(false);
        g_reset.store(true);
    }
    else
        cerr << "Unhandled interrupt." << endl;
}

int get_id(char c)
{
    if(c >= '0' && c <= '9')
        return (int)(c - '0');
    if(c >= 'a' && c <= 'f')
        return (int)(c - 'a' + 10);
    if(c >= 'A' && c <= 'F')
        return (int)(c - 'F' + 10);
    return 0;
}

HCHANNEL open_channel(int devIndex, double boardId)
{
    HCHANNEL hChannel = nullptr;
    int nChannelCount = MWGetChannelCount();

    if (0 == nChannelCount)
    {
        cerr << "ERROR: Can't find channels!\n";
        return nullptr;
    }

    int proDevCount = 0;
    int proDevChannel[32] = {-1};
    for (int idx = 0; idx < nChannelCount; ++idx)
    {
        MWCAP_CHANNEL_INFO info;
        MW_RESULT mr = MWGetChannelInfoByIndex(idx, &info);

        if (0 == (strcmp(info.szFamilyName, "Pro Capture")))
        {
            proDevChannel[proDevCount] = idx;
            proDevCount++;
        }
    }

    if (proDevCount <= 0)
    {
        cerr << "ERROR: Can't find pro channels!\n";
        return nullptr;
    }

    if(devIndex >= proDevCount)
    {
        cerr << "ERROR: just have " << proDevCount << " inputs!\n";
        return nullptr;
    }

    if (g_verbose > 0)
        cerr << "Found " << proDevCount << " pro inputs. ";

    // Get <board id > <channel id> or <channel index>
    // Open channel
    if (boardId >= 0)
        hChannel = MWOpenChannel(boardId, devIndex);
    else
    {
        char path[128] = {0};
        MWGetDevicePath(proDevChannel[devIndex], path);
        hChannel = MWOpenChannelByPath(path);
    }

    if (hChannel == nullptr)
    {
        if (g_verbose > 0)
        {
            cerr << "Error: Failed to open ";
            if (boardId >= 0)
                cerr << "board " << hex << boardId << dec << " ";
            cerr << "index " << devIndex << endl;
        }

        exit(2);
    }

    if (g_verbose > 0)
    {
        cerr << "Opened ";
        if (boardId >= 0)
            cerr << "board " << boardId << " ";
        cerr << "index " << devIndex + 1 << endl;
    }

    return hChannel;
}

string GetVideoSignal(int state)
{
    switch(state)
    {
        case MWCAP_VIDEO_SIGNAL_LOCKED:
          return "LOCKED";
        case MWCAP_VIDEO_SIGNAL_LOCKING:
          return "LOCKING";
        case MWCAP_VIDEO_SIGNAL_UNSUPPORTED:
          return "UNSUPPORTED";
        case MWCAP_VIDEO_SIGNAL_NONE:
          return "None";
        default:
          return "UNKNOWN";
    }
}

string GetVideoInputName(DWORD dwVideoInput)
{
    switch (INPUT_TYPE(dwVideoInput))
    {
        case MWCAP_VIDEO_INPUT_TYPE_NONE:
          return "None";
        case MWCAP_VIDEO_INPUT_TYPE_HDMI:
          return "HDMI";
        case MWCAP_VIDEO_INPUT_TYPE_VGA:
          return "VGA";
        case MWCAP_VIDEO_INPUT_TYPE_SDI:
          return "SDI";
        case MWCAP_VIDEO_INPUT_TYPE_COMPONENT:
          return "Component";
        case MWCAP_VIDEO_INPUT_TYPE_CVBS:
          return "CVBS";
        case MWCAP_VIDEO_INPUT_TYPE_YC:
          return "YC";
        default:
          return "Unknown";
    }
}

string GetAudioInputName(DWORD dwAudioInput)
{
    switch (INPUT_TYPE(dwAudioInput))
    {
        case MWCAP_AUDIO_INPUT_TYPE_NONE:
          return "None";
        case MWCAP_AUDIO_INPUT_TYPE_HDMI:
          return "HDMI";
        case MWCAP_AUDIO_INPUT_TYPE_SDI:
          return "SDI";
        case MWCAP_AUDIO_INPUT_TYPE_LINE_IN:
          return "Line In";
        case MWCAP_AUDIO_INPUT_TYPE_MIC_IN:
          return "Mic In";
        default:
          return "Unknown";
    }
}

string GetVideoColorName(MWCAP_VIDEO_COLOR_FORMAT color)
{
    switch (color)
    {
        case MWCAP_VIDEO_COLOR_FORMAT_RGB:
          return "RGB";
        case MWCAP_VIDEO_COLOR_FORMAT_YUV601:
          return "YUV BT.601";
        case MWCAP_VIDEO_COLOR_FORMAT_YUV709:
          return "YUV BT.709";
        case MWCAP_VIDEO_COLOR_FORMAT_YUV2020:
          return "YUV BT.2020";
        case MWCAP_VIDEO_COLOR_FORMAT_YUV2020C:
          return "YUV BT.2020C";
        case MWCAP_VIDEO_COLOR_FORMAT_UNKNOWN:
        default:
          return "Unknown";
    }
}

//sdi
string GetVideoSDIType(SDI_TYPE type)
{
    switch (type)
    {
        case SDI_TYPE_SD:
          return "SD";
        case SDI_TYPE_HD:
          return "HD";
        case SDI_TYPE_3GA:
          return "3GA";
        case SDI_TYPE_3GB_DL:
          return "3GB_DL";
        case SDI_TYPE_3GB_DS:
          return "3GB_DS";
        case SDI_TYPE_DL_CH1:
          return "DL_CH1";
        case SDI_TYPE_DL_CH2:
          return "DL_CH2";
        case SDI_TYPE_6G_MODE1:
          return "6G_MODE1";
        case SDI_TYPE_6G_MODE2:
          return "6G_MODE2";
        default:
          return "Unknown";
    }
}

string GetVideoScanFmt(SDI_SCANNING_FORMAT type)
{
    switch (type)
    {
        case SDI_SCANING_INTERLACED:
          return "INTERLACED";
        case SDI_SCANING_SEGMENTED_FRAME:
          return "SEGMENTED_FRAME";
        case SDI_SCANING_PROGRESSIVE:
          return "PROGRESSIVE";
        default:
          return "Unknown";
    }
}

string GetVideoSamplingStruct(SDI_SAMPLING_STRUCT type)
{
    switch (type)
    {
        case SDI_SAMPLING_422_YCbCr:
          return "422_YCbCr";
        case SDI_SAMPLING_444_YCbCr:
          return "444_YCbCr";
        case SDI_SAMPLING_444_RGB:
          return "444_RGB";
        case SDI_SAMPLING_420_YCbCr:
          return "420_YCbCr";
        case SDI_SAMPLING_4224_YCbCrA:
          return "4224_YCbCrA";
        case SDI_SAMPLING_4444_YCbCrA:
          return "4444_YCbCrA";
        case SDI_SAMPLING_4444_RGBA:
          return "4444_RGBA";
        case SDI_SAMPLING_4224_YCbCrD:
          return "4224_YCbCrD";
        case SDI_SAMPLING_4444_YCbCrD:
          return "4444_YCbCrD";
        case SDI_SAMPLING_4444_RGBD:
          return "4444_RGBD";
        case SDI_SAMPLING_444_XYZ:
          return "444_XYZ";
        default:
          return "Unknown";
    }
}

string GetVideoBitDepth(SDI_BIT_DEPTH type)
{
    switch (type)
    {
        case SDI_BIT_DEPTH_8BIT:
          return "8bit";
        case SDI_BIT_DEPTH_10BIT:
          return "10bit";
        case SDI_BIT_DEPTH_12BIT:
          return "12bit";
        default:
          return "Unknown";
    }
}

//vga
string GetVideoSyncType(BYTE type)
{
    switch (type)
    {
        case VIDEO_SYNC_ALL:
          return "ALL";
        case VIDEO_SYNC_HS_VS:
          return "HS_VS";
        case VIDEO_SYNC_CS:
          return "CS";
        case VIDEO_SYNC_EMBEDDED:
          return "EMBEDDED";
        default:
          return "Unknown";
    }
}

//cvbs
string GetVideoSDStandard(MWCAP_SD_VIDEO_STANDARD type)
{
    switch (type)
    {
        case MWCAP_SD_VIDEO_NONE:
          return "NONE";
        case MWCAP_SD_VIDEO_NTSC_M:
          return "NTSC_M";
        case MWCAP_SD_VIDEO_NTSC_433:
          return "NTSC_433";
        case MWCAP_SD_VIDEO_PAL_M:
          return "PAL_M";
        case MWCAP_SD_VIDEO_PAL_60:
          return "PAL_60";
        case MWCAP_SD_VIDEO_PAL_COMBN:
          return "PAL_COMBN";
        case MWCAP_SD_VIDEO_PAL_BGHID:
          return "PAL_BGHID";
        case MWCAP_SD_VIDEO_SECAM:
          return "SECAM";
        case MWCAP_SD_VIDEO_SECAM_60:
          return "SECAM_60";
        default:
          return "Unknown";
    }
}

string GetVideoInputType(DWORD type)
{
    switch (type)
    {
        case MWCAP_VIDEO_INPUT_TYPE_NONE:
          return "None";
        case MWCAP_VIDEO_INPUT_TYPE_HDMI:
          return "HDMI";
        case MWCAP_VIDEO_INPUT_TYPE_VGA:
          return "VGA";
        case MWCAP_VIDEO_INPUT_TYPE_SDI:
          return "SDI";
        case MWCAP_VIDEO_INPUT_TYPE_COMPONENT:
          return "COMPONENT";
        case MWCAP_VIDEO_INPUT_TYPE_CVBS:
          return "CVBS";
        case MWCAP_VIDEO_INPUT_TYPE_YC:
          return "YC";
        default:
          return "Unknown";
    }
}

void ListInputs(void)
{
    HCHANNEL hChannel = NULL;

    MWRefreshDevice();
    int num_channels = MWGetChannelCount();
    int board = -1;
    int idx;

    for (idx = 0; idx < num_channels; ++idx)
    {
        char path[128] = { 0 };

        if (hChannel != NULL)
            MWCloseChannel(hChannel);

        MWGetDevicePath(idx, path);
        hChannel = MWOpenChannelByPath(path);
        if (hChannel == NULL)
        {
            cerr << "ERROR: failed to open input " << idx << "\n";
            continue;
        }

        MWCAP_CHANNEL_INFO videoInfo = { 0 };

        if (MW_SUCCEEDED != MWGetChannelInfo(hChannel, &videoInfo))
        {
            cerr << "ERROR: failed to get channel info for input "
                 << idx << "\n";
            continue;
        }

        if (videoInfo.byBoardIndex != board)
        {
            cerr << "Board: " << static_cast<int>(videoInfo.byBoardIndex)
                 << ", Product: " << videoInfo.szProductName
                 << ", SerialNo: " << videoInfo.szBoardSerialNo
                 << "\n";
            board = videoInfo.byBoardIndex;
        }
        cerr << "[" << static_cast<int>(videoInfo.byChannelIndex) + 1
             << "] ";

        MW_RESULT xr;

        MWCAP_VIDEO_SIGNAL_STATUS vStatus;
        MWCAP_INPUT_SPECIFIC_STATUS status;
        xr = MWGetInputSpecificStatus(hChannel, &status);

        if (xr == MW_SUCCEEDED &&
            MWGetVideoSignalStatus(hChannel, &vStatus) == MW_SUCCEEDED)
        {
            if (!status.bValid)
            {
                cerr << "No signal\n";
                continue;
            }

            cerr << "Video Signal " << GetVideoSignal(vStatus.state);
            cerr << ": " << GetVideoInputType(status.dwVideoInputType);

            if (status.dwVideoInputType == MWCAP_VIDEO_INPUT_TYPE_HDMI)
            {
                cerr << ", HDCP: " << (status.hdmiStatus.bHDCP ? "Yes"
                                      : "No")

                     << ", Mode: "
                     << static_cast<int>(status.hdmiStatus.bHDMIMode)
                     << ", Bit Depth: "
                     << static_cast<int>(status.hdmiStatus.byBitDepth)
                     << "\n";
            }
            else if (status.dwVideoInputType == MWCAP_VIDEO_INPUT_TYPE_SDI)
            {
                cerr << ", Type: " << GetVideoSDIType(status.sdiStatus.sdiType)
                     << ", Scan Fmt: "
                     << GetVideoScanFmt(status.sdiStatus.sdiScanningFormat)
                     << ", Bit depth: "
                     << GetVideoBitDepth(status.sdiStatus.sdiBitDepth)
                     << ", Sampling: "
                     << GetVideoSamplingStruct(status.sdiStatus.sdiSamplingStruct)
                     << "\n";
            }
            else if (status.dwVideoInputType == MWCAP_VIDEO_INPUT_TYPE_VGA)
            {
                double dFrameDuration =
                    (status.vgaComponentStatus.syncInfo.bInterlaced == TRUE)
                    ? (double)20000000
                    / status.vgaComponentStatus.syncInfo.dwFrameDuration
                    : (double)10000000
                    / status.vgaComponentStatus.syncInfo.dwFrameDuration;
                dFrameDuration = static_cast<int>(dFrameDuration * 100)
                                 / 100.0;

                cerr << ", ScanType: "
                     << GetVideoSyncType(status.vgaComponentStatus.syncInfo.bySyncType)
                     << ", bHSPolarity: "
                     << status.vgaComponentStatus.syncInfo.bHSPolarity
                     << ", bVSPolarity: "
                     << status.vgaComponentStatus.syncInfo.bVSPolarity
                     << ", Interlaced: "
                     << status.vgaComponentStatus.syncInfo.bInterlaced
                     << ", FrameDuration: " << dFrameDuration
                     << "\n";
            }
            else if (status.dwVideoInputType == MWCAP_VIDEO_INPUT_TYPE_CVBS) {
                cerr << ", Standard: "
                     << GetVideoSDStandard(status.cvbsYcStatus.standard)
                     << ", b50Hz: " << status.cvbsYcStatus.b50Hz
                     << "\n";
            }

            double dFrameDuration = (vStatus.bInterlaced == TRUE)
                                    ? (double)20000000
                                    / vStatus.dwFrameDuration
                                    : (double)10000000
                                    / vStatus.dwFrameDuration;
            dFrameDuration = static_cast<int>(dFrameDuration * 100)
                             / 100.0;

            cerr << "    " << vStatus.cx << "x" << vStatus.cy
                 << (vStatus.bInterlaced ? "i" : "p")
                 << dFrameDuration;

            cerr << " [x:" << vStatus.x
                 << ", y:" << vStatus.y << "] "
                 << "total (" << vStatus.cxTotal << "x"
                 << vStatus.cyTotal << ") "
                 << "aspect " << vStatus.nAspectX << ":"
                 << vStatus.nAspectY
                 << " " << GetVideoColorName(vStatus.colorFormat)
                 << "\n";
        }


        // Audio Signal Status
        MWCAP_AUDIO_SIGNAL_STATUS aStatus;
        xr = MWGetAudioSignalStatus(hChannel, &aStatus);
        if (xr == MW_SUCCEEDED)
        {
            cerr << "    Audio Signal "
                 << (aStatus.bChannelStatusValid ? "Valid" : "Invalid");
            if (!aStatus.bChannelStatusValid)
            {
                cerr << "\n";
                continue;
            }

            cerr << ", Channels:";
            for (int i = 0; i < 4; ++i)
            {
                if (aStatus.wChannelValid & (0x01 << i))
                    cerr << " " << (i * 2 + 1) << "&" << (i * 2 + 2);
            }
            cerr << ", LPCM: " << (aStatus.bLPCM ? "Yes" : "No")
                 << ", BPSample: " << static_cast<int>(aStatus.cBitsPerSample)
                 << ", Sample Rate: "
                 << aStatus.dwSampleRate
                 << "\n";
        }
    }

    if (hChannel != NULL)
        MWCloseChannel(hChannel);
}

bool ReadEDID(HCHANNEL hChannel, const string & edid_file)
{
    DWORD dwVideoSource = 0;
    DWORD dwAudioSource = 0;

    if (MW_SUCCEEDED != MWGetVideoInputSource(hChannel, &dwVideoSource))
    {
        cerr << "ERROR: Can't get video input source!\n";
        return false;
    }

    if (MW_SUCCEEDED != MWGetAudioInputSource(hChannel, &dwAudioSource))
    {
        cerr << "ERROR: Can't get audio input source!\n";
        return false;
    }

    if (INPUT_TYPE(dwVideoSource) != MWCAP_VIDEO_INPUT_TYPE_HDMI ||
        INPUT_TYPE(dwAudioSource) != MWCAP_AUDIO_INPUT_TYPE_HDMI)
    {
        cerr << "Type of input source is not HDMI !\n";
        return false;
    }

    MW_RESULT xr;

    FILE* pFile = nullptr;
    pFile=fopen(edid_file.c_str(), "wb");
    if (pFile == nullptr)
    {
        cerr << "ERROR: Read EDID file!\n";
        return false;
    }

    ULONG ulSize = 256;
    BYTE byData[256];

    xr = MWGetEDID(hChannel, byData, &ulSize);
    if (xr == MW_SUCCEEDED)
    {
        int nWriteSize = (int)fwrite(byData, 1, 256, pFile);

        if (nWriteSize == ulSize)
        {
            cerr << "Wrote EDID to '" << edid_file << "'\n";
        }
        else
        {
            cerr << "ERROR: Failed to write to '" << edid_file << "'"
                 << strerror(errno) << "\n";
        }
    }
    else
    {
        cerr << "ERROR: Get EDID Info!\n";
    }

    fclose(pFile);
    pFile = NULL;

    return true;
}

bool WriteEDID(HCHANNEL hChannel, const string & edid_file)
{
    DWORD dwVideoSource = 0;
    DWORD dwAudioSource = 0;

    if (MW_SUCCEEDED != MWGetVideoInputSource(hChannel, &dwVideoSource))
    {
        cerr << "ERROR: Can't get video input source!\n";
        return false;
    }

    if (MW_SUCCEEDED != MWGetAudioInputSource(hChannel, &dwAudioSource))
    {
        cerr << "ERROR: Can't get audio input source!\n";
        return false;
    }

    if (INPUT_TYPE(dwVideoSource) != MWCAP_VIDEO_INPUT_TYPE_HDMI ||
        INPUT_TYPE(dwAudioSource) != MWCAP_AUDIO_INPUT_TYPE_HDMI)
    {
        cerr << "Type of input source is not HDMI !\n";
        return false;
    }

    MW_RESULT xr;

    FILE* pFile = nullptr;
    pFile=fopen(edid_file.c_str(), "rb");
    if (pFile == nullptr)
    {
        cerr << "ERROR: write EDID file!\n";
        return false;
    }

    BYTE byData[1024];
    int nSize = (int)fread(byData, 1, 1024, pFile);

    xr = MWSetEDID(hChannel, byData, nSize);
    if (xr == MW_SUCCEEDED)
        cerr << "EDID written successfully.\n";
    else
        cerr << "Failed to write EDID!\n";

    fclose(pFile);
    pFile = NULL;

    return true;
}


void* audio_capture(void* param1, int param2)
{
    bool      lpcm = false;
    int       bytes_per_sample = 0;
    int       sample_rate  = 0;
    WORD      valid_channels = 0;
    MWCAP_PTR notify_event = 0;
    HNOTIFY   notify_audio = 0;
    DWORD     input_count = 0;
    int       cur_channels;
    int       channel_offset;
    MWCAP_AUDIO_SIGNAL_STATUS audio_signal_status;
    int err_cnt = 0;
    int cnt     = 0;

    HCHANNEL* channel_handle = reinterpret_cast<HCHANNEL* >(param1);

    uint8_t* capture_buf     = nullptr;
    int      frame_idx       = 0;
    int      buf_idx         = 0;
    const int audio_buf_sz   = 384;
    size_t   frame_size      = 0;
    size_t   capture_buf_size = 0;

    int64_t* audio_timestamps = nullptr;

    ULONGLONG notify_status = 0;
    MWCAP_AUDIO_CAPTURE_FRAME macf;

    notify_event = MWCreateEvent();
    if (notify_event == 0)
    {
        cerr << "ERROR: create notify_event fail\n";
        goto audio_capture_stoped;
    }

    MWGetAudioInputSourceArray(channel_handle, nullptr, &input_count);
    if (input_count == 0)
    {
        cerr << "ERROR: can't find audio input\n";
        goto audio_capture_stoped;
    }

    if (MW_SUCCEEDED != MWStartAudioCapture(channel_handle))
    {
        cerr << "ERROR: start audio capture fail!\n";
        goto audio_capture_stoped;
    }

    if (g_verbose > 1)
        cerr << "Audio capture starting\n";

    notify_audio  = MWRegisterNotify(channel_handle, notify_event,
                                     (DWORD)MWCAP_NOTIFY_AUDIO_FRAME_BUFFERED |
                                     (DWORD)MWCAP_NOTIFY_AUDIO_SIGNAL_CHANGE |
                                     (DWORD)MWCAP_NOTIFY_AUDIO_INPUT_RESET |
                                     (DWORD)MWCAP_NOTIFY_HDMI_INFOFRAME_AUDIO
                                     );

    if (notify_audio == 0)
    {
        cerr << "EROR: Register Notify audio fail\n";
        goto audio_capture_stoped;
    }

    while (g_running.load() == true)
    {
        if (MW_SUCCEEDED != MWGetAudioSignalStatus(channel_handle,
                                                   &audio_signal_status))
        {
            if (err_cnt++ % 50 == 0 && g_verbose > 0)
                cerr << "WARNING (cnt: " << err_cnt
                     << ") can't get audio signal status\n";
            this_thread::sleep_for(chrono::milliseconds(g_frame_ms));
            continue;
        }

        if (!audio_signal_status.wChannelValid)
        {
            if (err_cnt++ % 50 == 0 && g_verbose > 0)
                cerr << "WARNING (cnt: " << err_cnt
                     << ") can't get audio, signal is invalid\n";

            this_thread::sleep_for(chrono::milliseconds(g_frame_ms));
            continue;
        }

        if (g_reset.load() == true ||
            lpcm != audio_signal_status.bLPCM ||
            sample_rate != audio_signal_status.dwSampleRate ||
            bytes_per_sample != audio_signal_status.cBitsPerSample / 8 ||
            valid_channels != audio_signal_status.wChannelValid)
        {
            lpcm = audio_signal_status.bLPCM;
            sample_rate = audio_signal_status.dwSampleRate;
            bytes_per_sample = audio_signal_status.cBitsPerSample / 8;
            if (bytes_per_sample > 2)
                bytes_per_sample = 4;
            valid_channels = audio_signal_status.wChannelValid;

            cur_channels = 0;
            for (int idx = 0; idx < (MWCAP_AUDIO_MAX_NUM_CHANNELS / 2); ++idx)
            {
                cur_channels +=
                    (valid_channels & (0x01 << idx)) ? 2 : 0;
            }

            if (0 == cur_channels)
            {
                if (err_cnt++ % 25 == 0 && g_verbose > 0)
                    cerr << "WARNING [" << err_cnt
                         << "] Invalid audio channel count: "
                         << cur_channels << endl;

                this_thread::sleep_for(chrono::milliseconds(g_frame_ms));
                continue;
            }

            channel_offset = cur_channels / 2;

            // NOTE: capture_buf/audio_timestamps will be freed by AudioIO class

            frame_idx = 0;
            buf_idx = 0;
            frame_size = MWCAP_AUDIO_SAMPLES_PER_FRAME
                         * cur_channels * bytes_per_sample;

            capture_buf_size = audio_buf_sz * frame_size;

            capture_buf = new uint8_t[capture_buf_size];
            if (nullptr == capture_buf)
            {
                cerr << "ERROR: audio capture_buf alloc failed\n";
                break;
            }

            audio_timestamps = new int64_t[audio_buf_sz];
            if (nullptr == audio_timestamps)
            {
                cerr << "ERROR: audio timestamp buf alloc failed\n";
                break;
            }

            g_out2ts->setAudioParams(capture_buf, capture_buf_size,
                                   cur_channels, lpcm,
                                   bytes_per_sample,
                                   sample_rate,
                                   MWCAP_AUDIO_SAMPLES_PER_FRAME,
                                   frame_size, audio_timestamps);

            g_reset.store(false);
        }

        cnt = 0;
        err_cnt = 0;

        while (g_reset.load() == false)
        {
            if (MWWaitEvent(notify_event, 1000) <= 0)
            {
                if (g_verbose > 1)
                    cerr << "Audio wait notify error or timeout\n";
                continue;
            }

            if (MW_SUCCEEDED != MWGetNotifyStatus(channel_handle,
                                                  notify_audio,
                                                  &notify_status))
                continue;

            if (notify_status & MWCAP_NOTIFY_AUDIO_SIGNAL_CHANGE)
            {
                if (g_verbose > 0)
                    cerr << "WARNING: Audio signal CHANGED!\n";
                this_thread::sleep_for(chrono::milliseconds(g_frame_ms));
                break;
            }

            if (notify_status & MWCAP_NOTIFY_AUDIO_INPUT_RESET)
            {
                if (g_verbose > 0)
                    cerr << "WARNING: Audio input RESET!\n";
                this_thread::sleep_for(chrono::milliseconds(g_frame_ms));
                break;
            }

            if (notify_status & MWCAP_NOTIFY_HDMI_INFOFRAME_AUDIO)
            {
                if (g_verbose > 0)
                    cerr << "WARNING: Audio HDMI INFOFRAME AUDIO -- unhandled!\n";
            }

            if (!(notify_status & MWCAP_NOTIFY_AUDIO_FRAME_BUFFERED))
                continue;

            if (MW_ENODATA == MWCaptureAudioFrame(channel_handle, &macf))
            {
                this_thread::sleep_for(chrono::milliseconds(g_frame_ms));
                continue;
            }

            /*
              L1L2L3L4R1R2R3R4L5L6L7L8R5R6R7R8(4byte)
              to 2channel 16bit
              L1R1L5R5(2byte)
            */
            for (int j = 0; j < (cur_channels/2); ++j)
            {
                for (int i = 0 ; i < MWCAP_AUDIO_SAMPLES_PER_FRAME; ++i)
                {
                    int write_pos = (i * cur_channels + j * 2) *
                                    bytes_per_sample;
                    int read_pos = (i * MWCAP_AUDIO_MAX_NUM_CHANNELS + j);
                    int read_pos2 = (i * MWCAP_AUDIO_MAX_NUM_CHANNELS + j +
                                     MWCAP_AUDIO_MAX_NUM_CHANNELS / 2);
                    DWORD left = macf.adwSamples[read_pos]
                                 >> (32 - audio_signal_status.cBitsPerSample);
                    DWORD right = macf.adwSamples[read_pos2]
                                  >> (32 - audio_signal_status.cBitsPerSample);
                    memcpy(&capture_buf[frame_idx + write_pos], &left, bytes_per_sample);
                    memcpy(&capture_buf[frame_idx + write_pos + bytes_per_sample],
                           &right, bytes_per_sample);
                }
            }

#if 0
            cerr << " FRAME_IDX " << frame_idx
                 << " @ " << (uint64_t)(&capture_buf[frame_idx]) << endl;
#endif
            audio_timestamps[buf_idx] = macf.llTimestamp;
            if (g_out2ts->addAudio(&capture_buf[frame_idx], frame_size,
                                 macf.llTimestamp))
            {
                if (++buf_idx == audio_buf_sz)
                    buf_idx = 0;
                frame_idx = frame_size * buf_idx;
            }
        }
    }

  audio_capture_stoped:
    g_running.store(false);

    if(notify_audio)
    {
        MWUnregisterNotify(channel_handle, notify_audio);
        notify_audio = 0;
    }

    MWStopAudioCapture(channel_handle);

    if(notify_event!= 0)
    {
        MWCloseEvent(notify_event);
        notify_event = 0;
    }

    return nullptr;
}

using imageque_t = deque<uint8_t*>;
imageque_t image_buffers;
imageque_t avail_image_buffers;
mutex image_buffer_mutex;

bool add_image_buffer(HCHANNEL hChannel, DWORD dwImageSize)
{
    unique_lock<mutex> lock(image_buffer_mutex);

    uint8_t* pbImage =  new uint8_t[dwImageSize];
    if (pbImage == nullptr)
    {
        cerr << "ERROR: image buffer alloc fail!\n";
        return false;
    }

    MWPinVideoBuffer(hChannel, (MWCAP_PTR)pbImage, dwImageSize);

    image_buffers.push_back(pbImage);
    avail_image_buffers.push_back(pbImage);
    return true;
}

void free_image_buffers(HCHANNEL hChannel)
{
    unique_lock<mutex> lock(image_buffer_mutex);
    imageque_t::iterator Iimage;

    g_out2ts->ClearImageQueue();

    for (Iimage = image_buffers.begin();
         Iimage != image_buffers.end(); ++Iimage)
    {
        MWUnpinVideoBuffer(hChannel, (LPBYTE)(*Iimage));
        delete[] (*Iimage);
    }
    image_buffers.clear();
    avail_image_buffers.clear();
}

void image_buffer_available(uint8_t* pbImage)
{
    unique_lock<mutex> lock(image_buffer_mutex);
    avail_image_buffers.push_back(pbImage);
}

void set_notify(HNOTIFY&  notify,
                HCHANNEL  hChannel,
                MWCAP_PTR hNotifyEvent,
                DWORD     flags)
{
    if (notify)
        MWUnregisterNotify(hChannel, notify);
    notify = MWRegisterNotify(hChannel, hNotifyEvent, flags);
}

bool video_capture_loop(HCHANNEL  hChannel,
                        HNOTIFY   notify_video,
                        MWCAP_PTR hNotifyEvent,
                        MWCAP_PTR hCaptureEvent)
{
    MWCAP_VIDEO_DEINTERLACE_MODE mode;
    MWCAP_VIDEO_BUFFER_INFO videoBufferInfo;
    DWORD notify_mode;
    LONGLONG llTotalTime = 0LL;
    DWORD dwFourcc = MWFOURCC_I420;

    int64_t timestamp;
    int     width  = 0;
    int     height = 0;
    bool    interlaced = false;
    double  frame_duration = 0;
    DWORD   dwMinStride = 0;
    DWORD   dwImageSize = 0;

    uint8_t* pbImage = nullptr;
    int buffer_cnt = 2;
    int frame_idx = -1;
    int frame_wrap_idx = 4;
    int idx;

    int slow_cnt = 0;

    if (g_out2ts->encoderType() == OutputTS::QSV ||
        g_out2ts->encoderType() == OutputTS::VAAPI)
        dwFourcc = MWFOURCC_NV12;
    else if (g_out2ts->encoderType() == OutputTS::NV)
        dwFourcc = MWFOURCC_I420;
    else
    {
        cerr << "ERROR: Failed to determine best magewell pixel format.\n";
        return false;
    }

    DWORD notify_changes = MWCAP_NOTIFY_VIDEO_SAMPLING_PHASE_CHANGE |
                           MWCAP_NOTIFY_VIDEO_SMPTE_TIME_CODE |
                           MWCAP_NOTIFY_VIDEO_SIGNAL_CHANGE;

    while (g_running.load() == true)
    {
#if 0
        while (!g_out2ts->WaitForAudio())
        {
            if (g_running.load() == false)
                break;
        }
        if (g_running.load() == false)
            break;
#endif

        MWCAP_VIDEO_FRAME_INFO videoFrameInfo;
        MWCAP_VIDEO_SIGNAL_STATUS videoSignalStatus;

        MWGetVideoSignalStatus(hChannel, &videoSignalStatus);

        if (videoSignalStatus.state == MWCAP_VIDEO_SIGNAL_UNSUPPORTED)
        {
            // This often doesn't recover, so use ERROR
            cerr << "ERROR: Input signal status: Unsupported\n";
            this_thread::sleep_for(chrono::milliseconds(g_frame_ms * 10));
            continue;
        }

        switch (videoSignalStatus.state)
        {
            case MWCAP_VIDEO_SIGNAL_NONE:
              if (g_verbose > 0)
                  cerr << "WARNING: Input signal status: NONE\n";
              this_thread::sleep_for(chrono::milliseconds(g_frame_ms * 5));
              continue;
            case MWCAP_VIDEO_SIGNAL_LOCKING:
              if (g_verbose > 0)
                  cerr << "WARNING: Input signal status: Locking\n";
              this_thread::sleep_for(chrono::milliseconds(g_frame_ms * 5));
              continue;
            case MWCAP_VIDEO_SIGNAL_LOCKED:
              if (g_verbose > 0)
                  cerr << "INFO: Input signal status: Locked\n";
              break;
            default:
              if (g_verbose > 0)
                  cerr << "WARNING: Video signal not locked.\n";
              this_thread::sleep_for(chrono::milliseconds(g_frame_ms * 5));
              continue;
        }

        if (width != videoSignalStatus.cx ||
            height != videoSignalStatus.cy ||
            frame_duration != videoSignalStatus.dwFrameDuration ||
            interlaced != static_cast<bool>(videoSignalStatus.bInterlaced))
        {
            width  = videoSignalStatus.cx;
            height = videoSignalStatus.cy;
            interlaced = static_cast<bool>(videoSignalStatus.bInterlaced);

            dwMinStride = FOURCC_CalcMinStride(dwFourcc,
                                               width, 4);
            dwImageSize = FOURCC_CalcImageSize(dwFourcc,
                                               width,
                                               height,
                                               dwMinStride);

            frame_duration = videoSignalStatus.dwFrameDuration;
            g_frame_ms = frame_duration / 10000;

            AVRational frame_rate, time_base;
            if (interlaced)
            {
                frame_rate = (AVRational){20000000LL, (int)frame_duration};
                time_base = (AVRational){1, 20000000LL};
            }
            else
            {
                frame_rate = (AVRational){10000000LL, (int)frame_duration};
                time_base = (AVRational){1, 10000000LL};
            }

            // 100ns / frame_duration
            if (g_verbose > 2)
            {
                cerr << "========\n";
                double fps = (interlaced) ?
                             (double)20000000LL / frame_duration :
                             (double)10000000LL / frame_duration;
                cerr << "Input signal resolution: " << width
                     << "x" << height << "\n";
                cerr << "Input signal fps: " << fps
                     << "  " << frame_rate.num << "/" << frame_rate.den << "\n";
                cerr << "Frame duration: " << frame_duration << "\n";
                cerr << "Time base: " << time_base.num << "/"
                     << time_base.den << "\n";
                cerr << "Input signal " << (interlaced
                                            ? "Interlaced" : "Progressive")
                                            << "\n";
                cerr << "Input signal frame segmented: "
                     << static_cast<bool>(videoSignalStatus.bSegmentedFrame)
                     << "\n";
                cerr << "========\n";
            }

            free_image_buffers(hChannel);
            if (!add_image_buffer(hChannel, dwImageSize))
                return false;
            if (!add_image_buffer(hChannel, dwImageSize))
                return false;

            g_out2ts->setVideoParams(width, height, interlaced,
                                  time_base, frame_duration, frame_rate);

            if (MWGetVideoBufferInfo(hChannel, &videoBufferInfo) != MW_SUCCEEDED)
            {
                continue;
            }

            frame_wrap_idx = videoBufferInfo.cMaxFrames;

            notify_mode = notify_changes;
            if(interlaced)
            {
                notify_mode |= MWCAP_NOTIFY_VIDEO_FIELD_BUFFERED;
                if (0 == videoBufferInfo.iBufferedFieldIndex)
                    mode = MWCAP_VIDEO_DEINTERLACE_TOP_FIELD;
                else
                    mode = MWCAP_VIDEO_DEINTERLACE_BOTTOM_FIELD;
            }
            else
            {
                notify_mode |= MWCAP_NOTIFY_VIDEO_FRAME_BUFFERED;
                mode = MWCAP_VIDEO_DEINTERLACE_BLEND;
            }
        }

        set_notify(notify_video, hChannel, hNotifyEvent, notify_mode);

        if (notify_video == 0)
        {
            if (g_verbose > 0)
                cerr << "WARNING: Register Notify error.\n";
            return false;
        }

        g_out2ts->VideoReady(true);
        frame_idx = -1;
        while (g_running.load() == true)
        {
            if (MWWaitEvent(hNotifyEvent, 1000) <= 0)
            {
                if (g_verbose > 0)
                    cerr << "Video wait notify error or timeout\n";
                continue;
            }

            ULONGLONG ullStatusBits = 0;
            if (MW_SUCCEEDED != MWGetNotifyStatus(hChannel, notify_video,
                                                  &ullStatusBits))
            {
                if (g_verbose > 0)
                    cerr << "WARNING: Failed to get Notify status.\n";
                continue;
            }

            if (notify_video & MWCAP_NOTIFY_VIDEO_SIGNAL_CHANGE)
            {
                if (g_verbose > 0)
                    cerr << "WARNING: Video signal CHANGED.\n";
                this_thread::sleep_for(chrono::milliseconds(5));
                break;
            }

            MWGetVideoSignalStatus(hChannel, &videoSignalStatus);
            if (videoSignalStatus.state != MWCAP_VIDEO_SIGNAL_LOCKED)
            {
                if (g_verbose > 0)
                    cerr << "WARNING: Video signal lost lock.\n";
                this_thread::sleep_for(chrono::milliseconds(5));
                break;
            }

            if (0 == (ullStatusBits & notify_mode))
                continue;

            if (MW_SUCCEEDED != MWGetVideoBufferInfo(hChannel,
                                                     &videoBufferInfo))
            {
                if (g_verbose > 0)
                    cerr << "WARNING: Failed to get video buffer info.\n";
                continue;
            }

            if (frame_idx == -1)
            {
                frame_idx = videoBufferInfo.iNewestBufferedFullFrame;
            }
            else
            {
                if (frame_idx == videoBufferInfo.iNewestBufferedFullFrame)
                {
                    if (g_verbose > 0)
                        cerr << "WARNING: Already processed MW video buffer "
                             << frame_idx << " -- Skipping\n";
                    continue;
                }
                if (++frame_idx == frame_wrap_idx)
                    frame_idx = 0;
                if (frame_idx != videoBufferInfo.iNewestBufferedFullFrame)
                {
                    if (g_verbose > 0)
                        cerr << "WARNING: Expected MW video buffer " << frame_idx
                             << " but current is "
                             << (int)videoBufferInfo.iNewestBufferedFullFrame
                             << ". Frame lost."
                             << endl;
                    frame_idx = videoBufferInfo.iNewestBufferedFullFrame;
                }
            }

            if (MWGetVideoFrameInfo(hChannel, frame_idx,
                                    &videoFrameInfo) != MW_SUCCEEDED)
            {
                if (g_verbose > 0)
                    cerr << "WARNING: Failed to get video frame info.\n";
                continue;
            }

            image_buffer_mutex.lock();
            if (avail_image_buffers.empty())
            {
                image_buffer_mutex.unlock();
                add_image_buffer(hChannel, dwImageSize);
                image_buffer_mutex.lock();
                if (g_verbose > 0)
                    cerr << "WARNING: video encoder is "
                         << ++buffer_cnt << " frames behind.\n";
            }

            pbImage = avail_image_buffers.front();
            avail_image_buffers.pop_front();
            image_buffer_mutex.unlock();

            MW_RESULT ret = MWCaptureVideoFrameToVirtualAddress
                            (hChannel,
                             frame_idx,
                             reinterpret_cast<MWCAP_PTR>(pbImage),
                             dwImageSize,
                             dwMinStride,
                             0,
                             0,
                             dwFourcc,
                             width,
                             height);

            if (ret != MW_SUCCEEDED)
            {
                image_buffer_available(pbImage);
                continue;
            }
            if (MWWaitEvent(hCaptureEvent, 1000) <= 0)
            {
                if (g_verbose > 0)
                    cerr << "WARNING: wait capture event error or timeout\n";
                image_buffer_available(pbImage);
                break;
            }

            MWCAP_VIDEO_CAPTURE_STATUS captureStatus;
            MWGetVideoCaptureStatus(hChannel, &captureStatus);

            timestamp = interlaced
                        ? videoFrameInfo.allFieldBufferedTimes[1]
                        : videoFrameInfo.allFieldBufferedTimes[0];

            g_out2ts->AddVideoFrame(pbImage, dwImageSize, timestamp);
        }
    }

    g_running.store(false);
    free_image_buffers(hChannel);
    return true;
}

bool video_capture(HCHANNEL hChannel)
{
    HNOTIFY   notify_video  = 0;
    MWCAP_PTR hNotifyEvent  = 0;
    MWCAP_PTR hCaptureEvent = 0;

    hCaptureEvent = MWCreateEvent();
    if (hCaptureEvent == 0)
    {
        if (g_verbose > 0)
            cerr << "ERROR: Create timer event error\n";
        return false;
    }

    hNotifyEvent = MWCreateEvent();
    if (hNotifyEvent == 0)
    {
        if (g_verbose > 0)
            cerr << "ERROR: Create notify event error\n";
        return false;
    }

    if (MW_SUCCEEDED != MWStartVideoCapture(hChannel, hCaptureEvent))
    {
        if (g_verbose > 0)
            cerr << "ERROR: Start Video Capture error!\n";
        return false;
    }

    video_capture_loop(hChannel, notify_video,
                       hNotifyEvent, hCaptureEvent);

    MWUnregisterNotify(hChannel, notify_video);
    notify_video=0;
    MWStopVideoCapture(hChannel);
    if (g_verbose > 2)
        cerr << "Capture finished.\n";

    if(hNotifyEvent != 0)
    {
        MWCloseEvent(hNotifyEvent);
        hNotifyEvent = 0;
    }

    if(hCaptureEvent != 0)
    {
        MWCloseEvent(hCaptureEvent);
        hCaptureEvent = 0;
    }

    return true;
}

bool capture(HCHANNEL channel_handle, bool no_audio)
{

    MWCAP_CHANNEL_INFO channel_info;
    MWRefreshDevice();
    if (MW_SUCCEEDED != MWGetChannelInfo(channel_handle, &channel_info)) {
        if (g_verbose > 0)
            cerr << "ERROR: Can't get channel info!\n";
        return false;
    }

    if (g_verbose > 1)
    {
        cerr << "Open channel - BoardIndex = "
             << channel_info.byBoardIndex << ", "
             << "ChannelIndex = "
             << channel_info.byChannelIndex << "\n";
        cerr << "Product Name: "
             << channel_info.szProductName << "\n";
        cerr << "Board SerialNo: "
             << channel_info.szBoardSerialNo << "\n";

        if (!no_audio)
            cerr << "Start audio thread.\n";
    }

    thread audio_thr;

    if (!no_audio)
    {
        audio_thr = thread(audio_capture, channel_handle,
                                g_verbose);
    }

    video_capture(channel_handle);

    if (!no_audio)
    {
        audio_thr.join();
    }

    if (channel_handle)
        MWCloseChannel(channel_handle);

    MWCaptureExitInstance();

    return 0;
}


void print_version_and_useage()
{
    cerr << " Version " << magewellpro2ts_VERSION_MAJOR << "."
         << magewellpro2ts_VERSION_MINOR << "\n";

    BYTE byMaj, byMin;
    WORD wBuild;
    MWGetVersion(&byMaj, &byMin, &wBuild);
    cerr << "Magewell MWCapture SDK V" << byMaj
         << "." << byMin << ".1." << wBuild << " - AudioCapture\n";
    cerr << "USB Devices are not supported\n";
    cerr << "Usage:\n"
         << "\tmagewell2ts <channel index>\n"
         << "\tmagewell2ts <board id>:<channel id>\n";

}

#if 0
void set_fourcc(void)
{
    if(CAPTURE_FOURCC == MWFOURCC_NV12)
    {
        printf("set fourcc to nv12\n");
    }
    else if(CAPTURE_FOURCC == MWFOURCC_YV12)
    {
        printf("set fourcc to yv12\n");
        RENDER_FOURCC = RENDER_YV12;
    }
    else if(CAPTURE_FOURCC == MWFOURCC_I420)
    {
        printf("set fourcc to i420\n");
        RENDER_FOURCC = RENDER_I420;
    }
    else if(CAPTURE_FOURCC == MWFOURCC_NV21)
    {
        printf("set fourcc to nv21\n");
        RENDER_FOURCC = RENDER_NV21;
    }
    else if(CAPTURE_FOURCC == MWFOURCC_YUY2)
    {
        printf("set fourcc to yuy2\n");
        RENDER_FOURCC = RENDER_YUY2;
    }
    else if(CAPTURE_FOURCC == MWFOURCC_RGB24)
    {
        printf("set fourcc to rgb24\n");
        RENDER_FOURCC = RENDER_RGB24;
    }
    else if(CAPTURE_FOURCC == MWFOURCC_BGR24)
    {
        printf("set fourcc to bgr24\n");
        RENDER_FOURCC = RENDER_BGR24;
    }
    else if(CAPTURE_FOURCC == MWFOURCC_RGBA)
    {
        printf("set fourcc to rgba\n");
        RENDER_FOURCC = RENDER_RGBA;
    }
    else if(CAPTURE_FOURCC == MWFOURCC_BGRA)
    {
        printf("set fourcc to bgra\n");
        RENDER_FOURCC = RENDER_BGRA;
    }
    else if(CAPTURE_FOURCC == MWFOURCC_ARGB)
    {
        printf("set fourcc to argb\n");
        RENDER_FOURCC = RENDER_ARGB;
    }
    else if(CAPTURE_FOURCC == MWFOURCC_ABGR)
    {
        printf("set fourcc to abgr\n");
        RENDER_FOURCC = RENDER_ABGR;
    }
    else
    {
        printf("not support fourcc, set to default nv12\n");
        CAPTURE_FOURCC == MWFOURCC_NV12;
    }
}

void parse_cmd(int argc, char* argv[])
{
    for(int i = 1; i < argc-1; i += 2)
    {
        if(!memcmp(argv[i],"-width", strlen("-width")))
        {
            CAPTURE_WIDTH = atoi(argv[i+1]);
            printf("set width to %d\n",CAPTURE_WIDTH);
        }
        else if(!memcmp(argv[i],"-height", strlen("-height")))
        {
            CAPTURE_HEIGHT = atoi(argv[i+1]);
            printf("set height to %d\n",CAPTURE_HEIGHT);
        }
        else if(!memcmp(argv[i],"-fourcc", strlen("-fourcc")))
        {
            if(!memcmp(argv[i+1],"nv12", strlen("nv12")))
            {
                CAPTURE_FOURCC = MWFOURCC_NV12;
            }
            else if(!memcmp(argv[i+1],"yv12", strlen("yv12")))
            {
                CAPTURE_FOURCC = MWFOURCC_YV12;
            }
            else if(!memcmp(argv[i+1],"nv21", strlen("nv21")))
            {
                CAPTURE_FOURCC = MWFOURCC_NV21;
            }
            else if(!memcmp(argv[i+1],"i420", strlen("i420")))
            {
                CAPTURE_FOURCC = MWFOURCC_I420;
            }
            else if(!memcmp(argv[i+1],"yuy2", strlen("yuy2")))
            {
                CAPTURE_FOURCC = MWFOURCC_YUY2;
            }
            else if(!memcmp(argv[i+1],"rgb", strlen("rgb")))
            {
                CAPTURE_FOURCC = MWFOURCC_RGB24;
            }
            else if(!memcmp(argv[i+1],"bgr", strlen("bgr")))
            {
                CAPTURE_FOURCC = MWFOURCC_BGR24;
            }
            else if(!memcmp(argv[i+1],"argb", strlen("argb")))
            {
                CAPTURE_FOURCC = MWFOURCC_ARGB;
            }
            else if(!memcmp(argv[i+1],"abgr", strlen("abgr")))
            {
                CAPTURE_FOURCC = MWFOURCC_ABGR;
            }
            else if(!memcmp(argv[i+1],"rgba", strlen("rgba")))
            {
                CAPTURE_FOURCC = MWFOURCC_RGBA;
            }
            else if(!memcmp(argv[i+1],"bgra", strlen("bgra")))
            {
                CAPTURE_FOURCC = MWFOURCC_BGRA;
            }
            set_fourcc();
        }
        else if(!memcmp(argv[i],"-sample_rate", strlen("-sample_rate")))
        {
            CAPTURE_SAMPLE_RATE = atoi(argv[i+1]);
            printf("set sample rate to %d\n",CAPTURE_SAMPLE_RATE);
        }
        else if(!memcmp(argv[i],"-frame_rate", strlen("-frame_rate")))
        {
            CAPTURE_FRAME_RATE = atoi(argv[i+1]);
            printf("set frame rate to %d\n",CAPTURE_FRAME_RATE);
        }
    }
}
#endif

void display_volume(HCHANNEL channel_handle)
{
    MWCAP_AUDIO_VOLUME volume;
    _MWCAP_AUDIO_NODE node = MWCAP_AUDIO_EMBEDDED_CAPTURE;
    MWGetAudioVolume(channel_handle, node, &volume);

    cerr << "VolumeMin: " << volume.sVolumeMin << "\n"
         << "VolumeMax: " << volume.sVolumeMax << "\n"
         << "VolumeStep: " << volume.sVolumeStep << "\n";

    // MWCAP_MAX_NUM_AUDIO_CHANNEL
    for (int idx = 0; idx < 8; ++idx)
    {
        cerr << "[" << idx << "] Mute: "
             << (volume.abMute[idx] ? "Yes" : "No")
             << ", Volume: " << volume.asVolume[idx] << "\n";
    }
}

void save_volume(HCHANNEL channel_handle, int volume_level)
{
    MWCAP_AUDIO_VOLUME volume;
    _MWCAP_AUDIO_NODE  node = MWCAP_AUDIO_EMBEDDED_CAPTURE;

    MWGetAudioVolume(channel_handle, node, &volume);

    for(int i=0; i<MWCAP_MAX_NUM_AUDIO_CHANNEL; ++i)
    {
        volume.asVolume[i] = volume_level;
    }

    MWSetAudioVolume(channel_handle, node, &volume);

    if (g_verbose > 0)
        cerr << "Volume set to " << volume_level << " for all channels.\n";
}

bool wait_for_inputs(int cnt)
{
    int idx = 10;

    do
    {
        if (MWCaptureInitInstance())
        {
            if (MWGetChannelCount() >= cnt)
            {
                MWCaptureExitInstance();
                return true;
            }
            MWCaptureExitInstance();
        }
        sleep(1);
    }
    while (--idx);

    return false;
}

void show_help(string_view app)
{
    cerr << app << endl;

    cerr << "\n"
         << "Defaults in []:\n"
         << "\n";

    cerr << "--board (-b)       : board id, if you have more than one [0]\n"
         << "--device (-d)      : vaapi/qsv device (e.g. renderD129) [renderD128]\n"
         << "--get-volume (-g)  : Display volume settings for each channel of input\n"
         << "--input (-i)       : input idx, *required*. Starts at 1\n"
         << "--list-inputs (l)  : List capture card inputs\n"
         << "--mux (-m)         : capture audio and video and mux into TS [false]\n"
         << "--no-audio (-n)    : Only capture video. [false]\n"
         << "--read-edid (-r)   : Read EDID info for input to file\n"
         << "--set-volume (-s)  : Set volume for all channels of the input\n"
         << "--verbose (-v)     : message verbose level. 0=completely quiet [1]\n"
         << "--video-codec (-c) : Video codec name (e.g. hevc_qsv, h264_nvenc) [hevc_nvenc]\n"
         << "--lookahead (-a)   : How many frames to 'look ahead' [35]\n"
         << "--quality (-q)     : quality setting [25]\n"
         << "--preset (-p)      : encoder preset\n"
         << "--write-edid (-w)  : Write EDID info from file to input\n"
         << "--wait-for         : Wait for given number of inputs to be initialized. 10 second timeout\n";

    cerr << "\n"
         << "Examples:\n"
         << "\tCapture from input 2 and write Transport Stream to stdout:\n"
         << "\t" << app << " -i 2 -m\n"
         << "\n"
         << "\tWrite EDID to input 3 and capture audio and video:\n"
         << "\t" << app << " -i 3 -w ProCaptureHDMI-EAC3.bin -m\n"
         << "\n"
         << "\tSet Volume of input 1 to max and capture to TS:\n"
         << "\t" << app << " -i 1 -s 100 -m\n"
         << "\n"
         << "\tUse the iHD vaapi driver to encode h264 video and pipe it to mpv:\n"
         << "\t" << app << " ./magewellpro2ts -i 1 -m -n -c h264_qsv | mpv -\n";

    cerr << "\nNOTE: setting EDID does not survive a reboot.\n";
}

bool string_to_int(string_view st, int &value, string_view var)
{
    auto result = from_chars(st.data(), st.data() + st.size(),
                                  value);
    if (result.ec == errc::invalid_argument)
    {
        cerr << "Invalid " << var << ": " << st << endl;
        value = -1;
        return false;
    }

    return true;
}

int main(int argc, char* argv[])
{
//    parse_cmd(argc, argv);

    int    ret = 0;
    int    boardId  = -1;
    int    devIndex = -1;

    string_view app_name = argv[0];
    string      edid_file;
    string      video_codec = "hevc_nvenc";
    string      device      = "renderD128";

    bool        get_volume  = false;
    int         set_volume  = -1;

    bool        list_inputs = false;
    bool        do_capture  = false;
    bool        read_edid   = false;
    bool        write_edid  = false;

    string      preset;
    int         quality     = 25;
    int         look_ahead  = -1;
    bool        no_audio    = false;

    vector<string_view> args(argv + 1, argv + argc);

    {
        struct sigaction action;
        action.sa_handler = signal_handler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        sigaction(SIGINT, &action, NULL);
        sigaction(SIGTERM, &action, NULL);
        sigaction(SIGHUP, &action, NULL);
        sigaction(SIGUSR1, &action, NULL);
    }

    for (auto iter = args.begin(); iter != args.end(); ++iter)
    {
        if (*iter == "-h" || *iter == "--help")
        {
            show_help(app_name);
            return 0;
        }
        else if (*iter == "-l" || *iter == "--list-inputs")
        {
            list_inputs = true;
        }
        else if (*iter == "-p" || *iter == "--preset")
        {
            preset = *(++iter);
        }
        else if (*iter == "-q" || *iter == "--quality")
        {
            if (!string_to_int(*(++iter), quality, "quality"))
                exit(1);
        }
        else if (*iter == "-a" || *iter == "--lookahead")
        {
            if (!string_to_int(*(++iter), look_ahead, "lookahead"))
                exit(1);
        }
        else if (*iter == "-m" || *iter == "--mux")
        {
            do_capture = true;
        }
        else if (*iter == "-i" || *iter == "--input")
        {
            if (!string_to_int(*(++iter), devIndex, "device index"))
                exit(1);
        }
        else if (*iter == "-b" || *iter == "--board")
        {
            if (!string_to_int(*(++iter), boardId, "board id"))
                exit(1);
        }
        else if (*iter == "-c" || *iter == "--video-codec")
        {
            video_codec = *(++iter);
        }
        else if (*iter == "-r" || *iter == "--read-edid")
        {
            read_edid = true;
            edid_file = *(++iter);
        }
        else if (*iter == "-w" || *iter == "--write-edid")
        {
            write_edid = true;
            edid_file = *(++iter);
        }
        else if (*iter == "-g" || *iter == "--get-volume")
        {
            get_volume = true;
        }
        else if (*iter == "-s" || *iter == "--set-volume")
        {
            if (!string_to_int(*(++iter), set_volume, "volume"))
                exit(1);
        }
        else if (*iter == "-n" || *iter == "--no-audio")
        {
            no_audio = true;
        }
        else if (*iter == "-d" || *iter == "--device")
        {
            device = *(++iter);
        }
        else if (*iter == "--wait-for")
        {
            int input_count;
            if (!string_to_int(*(++iter), input_count, "input count"))
                exit(1);
            wait_for_inputs(input_count);
        }
        else if (*iter == "-v" || *iter == "--verbose")
        {
            if (iter + 1 == args.end())
                g_verbose = 1;
            else
            {
                if (!string_to_int(*(++iter), g_verbose, "verbose"))
                    exit(1);
            }
        }
        else
        {
            cerr << "Unrecognized option '" << *iter << "'\n";
            exit(1);
        }
    }

    if (!MWCaptureInitInstance())
    {
        cerr << "have InitilizeFailed\n";
        return -1;
    }

    if (list_inputs)
        ListInputs();

    if (devIndex < 1)
        return 0;

    HCHANNEL channel_handle = open_channel(devIndex - 1, boardId);
    if (channel_handle == nullptr)
        return -1;

    if (get_volume)
        display_volume(channel_handle);
    if (set_volume >= 0)
        save_volume(channel_handle, set_volume);

    if (!edid_file.empty())
    {
        if (read_edid)
            ReadEDID(channel_handle, edid_file);
        else if (write_edid)
            WriteEDID(channel_handle, edid_file);
    }

    if (do_capture)
    {
        g_out2ts = new OutputTS(g_verbose, video_codec, preset, quality,
                                look_ahead, no_audio,
                                device, image_buffer_available);

        ret = capture(channel_handle, no_audio);
        delete g_out2ts;
    }

    if (channel_handle != nullptr)
        MWCloseChannel(channel_handle);

    MWCaptureExitInstance();

    return ret;
}
