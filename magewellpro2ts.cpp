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

#include <iostream>
#include <cstring>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <thread>
#include <charconv>
#include <string_view>

#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <cerrno>

#include <MWFOURCC.h>
#include <LibMWCapture/MWCapture.h>
#include <libavutil/rational.h>

#include "OutputTS.h"
#include "version.h"

using namespace std;

bool g_running = true;

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

HCHANNEL open_channel(int verbose, int devIndex, double boardId)
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

    if (verbose > 0)
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
        if (verbose > 0)
        {
            cerr << "Error: Failed to open ";
            if (boardId >= 0)
                cerr << "board " << hex << boardId << dec << " ";
            cerr << "index " << devIndex << endl;
        }

        exit(2);
    }

    if (verbose > 0)
    {
        cerr << "Opened ";
        if (boardId >= 0)
            cerr << "board " << boardId << " ";
        cerr << "index " << devIndex << " (starting at 0)." << endl;
    }

    return hChannel;
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

    FILE * pFile = nullptr;
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

    FILE * pFile = nullptr;
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


void *audio_capture(void * param1, int param2, void * param3)
{
    int       bytes_per_sample;
    MWCAP_PTR notify_event = 0;
    HNOTIFY   notify_audio = 0;
    DWORD     input_count = 0;
    int       cur_channels;
    int       channel_offset;
    unsigned char *capture_buf = nullptr;
    MWCAP_AUDIO_SIGNAL_STATUS audio_signal_status;
    int err_cnt = 0;
    int cnt     = 0;

    long long int tm_current  = 0LL;
    long long int tm_last     = 0LL;
    uint64_t audio_frame_rate = 0LL;

    HCHANNEL * channel_handle = reinterpret_cast<HCHANNEL *>(param1);
    int        verbose        = param2;
    OutputTS * out2ts         = reinterpret_cast<OutputTS *>(param3);

    notify_event = MWCreateEvent();
    if (notify_event == 0)
    {
        if (verbose > 0)
            cerr << "create notify_event fail\n";
        goto audio_capture_stoped;
    }

    notify_audio  = MWRegisterNotify(channel_handle, notify_event,
                                     MWCAP_NOTIFY_AUDIO_FRAME_BUFFERED |
                                     MWCAP_NOTIFY_AUDIO_SIGNAL_CHANGE |
                                     MWCAP_NOTIFY_AUDIO_INPUT_RESET |
                                     MWCAP_NOTIFY_HDMI_INFOFRAME_AUDIO
                                     );

    if (notify_audio == 0)
    {
        if (verbose > 0)
            cerr << "Register Notify audio fail\n";
        goto audio_capture_stoped;
    }

    MWGetAudioInputSourceArray(channel_handle, nullptr, &input_count);
    if (input_count == 0)
    {
        if (verbose > 0)
            cerr << "can't find audio input\n";
        goto audio_capture_stoped;
    }

    if (MW_SUCCEEDED != MWStartAudioCapture(channel_handle))
    {
        if (verbose > 0)
            cerr << "start audio capture fail!\n";
        goto audio_capture_stoped;
    }

    if (verbose > 1)
    cerr << "Audio capture starting\n";

    while (g_running)
    {
        if (MW_SUCCEEDED != MWGetAudioSignalStatus(channel_handle,
                                                   &audio_signal_status))
        {
            if (++err_cnt > 50)
                break;
            if (verbose > 0)
                cerr << "[" << err_cnt << "] can't get audio signal status\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // FALSE ==
        if (!audio_signal_status.wChannelValid)
        {
            if (++err_cnt > 50)
                break;
            if (verbose > 0)
                cerr << "[" << err_cnt << "] audio signal is invalid\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        bytes_per_sample = audio_signal_status.cBitsPerSample / 8;
        cur_channels = 0;
        for (int idx = 0; idx < (MWCAP_AUDIO_MAX_NUM_CHANNELS / 2); ++idx)
        {
            cur_channels +=
                (audio_signal_status.wChannelValid & (0x01 << idx)) ? 2 : 0;
        }

        if (0 == cur_channels)
        {
            if (++err_cnt > 50)
                break;
            if (verbose > 0)
                cerr << "[" << err_cnt << "] audio channel "
                     << cur_channels << " error\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        channel_offset = cur_channels / 2;

        if (capture_buf != nullptr)
        {
            delete[] capture_buf;
            capture_buf = nullptr;
        }

        size_t frame_size = MWCAP_AUDIO_SAMPLES_PER_FRAME * cur_channels *
                            bytes_per_sample;

        capture_buf = new uint8_t[frame_size];
        if (nullptr == capture_buf)
        {
            if (verbose > 0)
                cerr << "capture_buf alloc failed\n";
            break;
        }

        MWGetDeviceTime(channel_handle, &tm_last);

// Channels: 2 SampleRate: 48000 FrameRate: 9216000 BytesPerSample: 2
// perFrame 192
        out2ts->setAudioParams(cur_channels, bytes_per_sample,
                               MWCAP_AUDIO_SAMPLES_PER_FRAME);

        cnt = 0;
        err_cnt = 0;

        while (g_running)
        {
            ULONGLONG notify_status = 0;
            unsigned char *audio_frame;
            MWCAP_AUDIO_CAPTURE_FRAME macf;

            if (MWWaitEvent(notify_event, 1000) <= 0)
            {
                if (verbose > 1)
                    cerr << "wait notify error or timeout\n";
            }

            if (MW_SUCCEEDED != MWGetNotifyStatus(channel_handle,
                                                  notify_audio,
                                                  &notify_status))
            {
                continue;
            }

            if (notify_status & MWCAP_NOTIFY_AUDIO_SIGNAL_CHANGE)
            {
                if (verbose > 1)
                    cerr << "Audio signal CHANGED!\n";
                break;
            }

            if (notify_status & MWCAP_NOTIFY_AUDIO_INPUT_RESET)
            {
                if (verbose > 1)
                    cerr << "Audio input RESET!\n";
                break;
            }

            if (notify_status & MWCAP_NOTIFY_HDMI_INFOFRAME_AUDIO)
            {
                if (verbose > 1)
                    cerr << "Audio HDMI INFOFRAME AUDIO!\n";
            }

            if (!(notify_status & MWCAP_NOTIFY_AUDIO_FRAME_BUFFERED))
            {
                continue;
            }

            if (MW_ENODATA == MWCaptureAudioFrame(channel_handle, &macf))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            uint64_t timestamp = macf.llTimestamp;

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
                    memcpy(&capture_buf[write_pos], &left, bytes_per_sample);
                    memcpy(&capture_buf[write_pos + bytes_per_sample],
                           &right, bytes_per_sample);
                }
            }

            out2ts->addPacket(capture_buf, frame_size, timestamp);
        }
    }

  audio_capture_stoped:
    g_running = false;

    if(capture_buf)
        delete[] capture_buf;

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

bool video_capture_loop(HCHANNEL  hChannel,
                        HNOTIFY   hNotify,
                        MWCAP_PTR hNotifyEvent,
                        MWCAP_PTR hCaptureEvent,
                        uint8_t * pbImage,
                        int       verbose,
                        OutputTS & out2ts)
{
    MWCAP_VIDEO_DEINTERLACE_MODE mode;
    DWORD notifyBufferMode;
    LONGLONG llTotalTime = 0LL;

    while (g_running)
    {
        if (!out2ts.AudioReady())
            continue;

        MWCAP_VIDEO_BUFFER_INFO videoBufferInfo;
        MWGetVideoBufferInfo(hChannel, &videoBufferInfo);

        MWCAP_VIDEO_FRAME_INFO videoFrameInfo;
        MWGetVideoFrameInfo(hChannel,
                            videoBufferInfo.iNewestBufferedFullFrame,
                            &videoFrameInfo);

        MWCAP_VIDEO_SIGNAL_STATUS videoSignalStatus;
        MWGetVideoSignalStatus(hChannel, &videoSignalStatus);

//        DWORD dwFourcc = MWFOURCC_BGR24;
        DWORD dwFourcc = MWFOURCC_I420;
        DWORD dwMinStride = FOURCC_CalcMinStride(dwFourcc,
                                                 videoSignalStatus.cx, 4);
        DWORD dwImageSize = FOURCC_CalcImageSize(dwFourcc,
                                                 videoSignalStatus.cx,
                                                 videoSignalStatus.cy,
                                                 dwMinStride);

        switch (videoSignalStatus.state)
        {
            case MWCAP_VIDEO_SIGNAL_NONE:
              if (verbose > 1)
                  cerr << "ERRPR: Input signal status: NONE\n";
              break;
            case MWCAP_VIDEO_SIGNAL_UNSUPPORTED:
              if (verbose > 1)
                  cerr << "ERRPR: Input signal status: Unsupported\n";
              break;
            case MWCAP_VIDEO_SIGNAL_LOCKING:
              if (verbose > 1)
                  cerr << "ERRPR: Input signal status: Locking\n";
              break;
            case MWCAP_VIDEO_SIGNAL_LOCKED:
              if (verbose > 1)
                  cerr << "Input signal status: Locked\n";
              break;
        }

        if (videoSignalStatus.state != MWCAP_VIDEO_SIGNAL_LOCKED)
        {
            MWStopVideoCapture(hChannel);
            break;
        }

        double frame_duration = videoSignalStatus.dwFrameDuration;
        AVRational frame_rate, time_base;
        if (videoSignalStatus.bInterlaced == TRUE)
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
        if (verbose > 2)
        {
            cerr << "========\n";
            double fps = (videoSignalStatus.bInterlaced == TRUE) ?
                         (double)20000000LL / frame_duration :
                         (double)10000000LL / frame_duration;
            cerr << "Input signal resolution: " << videoSignalStatus.cx
                 << "x" << videoSignalStatus.cy << "\n";
            cerr << "Input signal fps: " << fps
                 << "  " << frame_rate.num << "/" << frame_rate.den << "\n";
            cerr << "Frame duration: " << frame_duration << "\n";
            cerr << "Time base: " << time_base.num << "/" << time_base.den << "\n";
            cerr << "Input signal interlaced: "
                 << static_cast<bool>(videoSignalStatus.bInterlaced) << "\n";
            cerr << "Input signal frame segmented: "
                 << static_cast<bool>(videoSignalStatus.bSegmentedFrame) << "\n";
            cerr << "========\n";
        }

        if (pbImage)
        {
            delete[] pbImage;
            pbImage == nullptr;
        }
        pbImage = new uint8_t[dwImageSize];
        if (pbImage == nullptr)
        {
            if (verbose > 0)
                cerr << "ERROR: image buffer alloc fail!\n";
            return false;
        }

        out2ts.setVideoParams(videoSignalStatus.cx,
                              videoSignalStatus.cy,
                              videoSignalStatus.bInterlaced,
                              time_base, frame_rate);

        notifyBufferMode = videoSignalStatus.bInterlaced ?
                           MWCAP_NOTIFY_VIDEO_FIELD_BUFFERED :
                           MWCAP_NOTIFY_VIDEO_FRAME_BUFFERED;

        hNotify = MWRegisterNotify(hChannel, hNotifyEvent, notifyBufferMode);
        if (hNotify == 0)
        {
            if (verbose > 0)
                cerr << "ERROR: Register Notify error.\n";
            return false;
        }
        MWPinVideoBuffer(hChannel, (MWCAP_PTR)pbImage, dwImageSize);

        while (g_running)
        {
            if (MWWaitEvent(hNotifyEvent, 1000) <= 0)
            {
                if (verbose > 0)
                    cerr << "Error:wait notify error or timeout\n";
                break;
            }

            ULONGLONG ullStatusBits = 0;
            if (MW_SUCCEEDED != MWGetNotifyStatus(hChannel, hNotify,
                                                  &ullStatusBits))
            {
                if (verbose > 0)
                    cerr << "Failed to get Notify status.\n";
                continue;
            }

            if (MW_SUCCEEDED != MWGetVideoBufferInfo(hChannel,
                                                     &videoBufferInfo))
            {
                if (verbose > 0)
                    cerr << "Failed to get video buffer info.\n";
                continue;
            }

            if (MWGetVideoFrameInfo(hChannel,
                                    videoBufferInfo.iNewestBufferedFullFrame,
                                    &videoFrameInfo) != MW_SUCCEEDED)
            {
                if (verbose > 0)
                    cerr << "Failed to get video frame info.\n";
                continue;
            }

            if (0 == (ullStatusBits & notifyBufferMode))
            {
                continue;
            }

            if(videoSignalStatus.bInterlaced)
            {
                if (0 == videoBufferInfo.iBufferedFieldIndex)
                {
                    mode = MWCAP_VIDEO_DEINTERLACE_TOP_FIELD;
                }
                else
                {
                    mode = MWCAP_VIDEO_DEINTERLACE_BOTTOM_FIELD;
                }
            }
            else
            {
                mode = MWCAP_VIDEO_DEINTERLACE_BLEND;
            }

            MW_RESULT ret = MWCaptureVideoFrameToVirtualAddressEx
                            (hChannel,
                             videoBufferInfo.iNewestBufferedFullFrame,
                             (unsigned char *)pbImage,
                             dwImageSize,
                             dwMinStride,
                             0,
                             0,
                             dwFourcc,
                             videoSignalStatus.cx,
                             videoSignalStatus.cy,
                             0,
                             0,
                             0,
                             0,
                             0,
                             100,
                             0,
                             100,
                             0,
                             mode,
                             MWCAP_VIDEO_ASPECT_RATIO_CROPPING,
                             0,
                             0,
                             0,
                             0,
                             MWCAP_VIDEO_COLOR_FORMAT_UNKNOWN,
                             MWCAP_VIDEO_QUANTIZATION_UNKNOWN,
                             MWCAP_VIDEO_SATURATION_UNKNOWN);

            if (ret != MW_SUCCEEDED)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (MWWaitEvent(hCaptureEvent,1000) <= 0)
            {
                if (verbose > 0)
                    cerr << "Error:wait capture event error or timeout\n";
                break;
            }

            MWCAP_VIDEO_CAPTURE_STATUS captureStatus;
            MWGetVideoCaptureStatus(hChannel, &captureStatus);

            LONGLONG llCurrent = 0LL;
            MWGetDeviceTime(hChannel, &llCurrent);

            if (verbose > 4)
            {
                llTotalTime += (llCurrent -
                                (videoSignalStatus.bInterlaced
                                 ? videoFrameInfo.allFieldBufferedTimes[1]
                                 : videoFrameInfo.allFieldBufferedTimes[0]));
                cerr << "Time: " << llTotalTime << endl;
            }

            out2ts.Write(pbImage, dwImageSize, llCurrent);
        }

        MWUnpinVideoBuffer(hChannel, (LPBYTE)pbImage);
    }

    return true;
}

bool video_capture(HCHANNEL hChannel, int verbose, OutputTS & out2ts)
{
    HNOTIFY   hNotify       = 0;
    MWCAP_PTR hNotifyEvent  = 0;
    MWCAP_PTR hCaptureEvent = 0;
    uint8_t * pbImage       = nullptr;

    hCaptureEvent = MWCreateEvent();
    if (hCaptureEvent == 0)
    {
        if (verbose > 0)
            cerr << "Create timer event error\n";
        return false;
    }

    hNotifyEvent = MWCreateEvent();
    if (hNotifyEvent == 0)
    {
        if (verbose > 0)
            cerr << "Create notify event error\n";
        return false;
    }

    if (MW_SUCCEEDED != MWStartVideoCapture(hChannel, hCaptureEvent))
    {
        if (verbose > 0)
            cerr << "ERROR: Start Video Capture error!\n";
        return false;
    }

    while (g_running)
    {
        if (!video_capture_loop(hChannel, hNotify,
                                hNotifyEvent, hCaptureEvent,
                                pbImage, verbose, out2ts))
            break;
    }

    g_running = false;

    if(pbImage)
    {
        delete[] pbImage;
        pbImage = nullptr;
    }

    MWUnregisterNotify(hChannel, hNotify);
    hNotify=0;
    MWStopVideoCapture(hChannel);
    if (verbose > 2)
        cerr << "\nStop capture\n";

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

bool capture(HCHANNEL channel_handle, int verbose,
             OutputTS & out2ts, bool no_audio)
{

    MWCAP_CHANNEL_INFO channel_info;
    MWRefreshDevice();
    if (MW_SUCCEEDED != MWGetChannelInfo(channel_handle, &channel_info)) {
        if (verbose > 0)
            cerr << "ERROR: Can't get channel info!\n";
        return false;
    }

    if (verbose > 1)
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

    std::thread audio_thr;

    if (!no_audio)
    {
        audio_thr = std::thread(audio_capture, channel_handle,
                                verbose, &out2ts);
    }

    video_capture(channel_handle, verbose, out2ts);

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

void save_volume(HCHANNEL channel_handle, int verbose, int volume_level)
{
    MWCAP_AUDIO_VOLUME volume;
    _MWCAP_AUDIO_NODE  node = MWCAP_AUDIO_EMBEDDED_CAPTURE;

    MWGetAudioVolume(channel_handle, node, &volume);

    for(int i=0; i<MWCAP_MAX_NUM_AUDIO_CHANNEL; ++i)
    {
        volume.asVolume[i] = volume_level;
    }

    MWSetAudioVolume(channel_handle, node, &volume);

    if (verbose > 0)
        cerr << "Volume set to " << volume_level << " for all channels.\n";
}

void show_help(string_view app)
{
    cerr << app << " <--board id> --input id <--edid 'filename'> "
         << "<--verbose level> "
         << "<--read-edid 'filename'> "
         << "<--write-edid 'filename'> <--get-volume> <--set-volume val> "
         << "<--mux> <--lookahead frames> <--no-audio>"
         << endl;

    cerr << "\n";

    cerr << "--board (-b)      : board id, if you have more than one.\n"
         << "--input (-i)      : input idx, *required*. Starts at 1.\n"
         << "--verbose (-v)    : message verbose level. 0=completely quiet.\n"
         << "--mux (-m)        : capture audio and video and mux into TS.\n"
         << "--lookahead (-l)  : How many frames to 'look ahead' (default 32)\n"
         << "--read-edid (-r)  : Read EDID info for input to file.\n"
         << "--write-edid (-w) : Write EDID info from file to input.\n"
         << "--get-volume (-g) : Display volume settings for each channel of input.\n"
         << "--set-volume (-s) : Set volume for all channels of the input.\n"
         << "--no-audio (-n)   : Only capture video.\n";

    cerr << "\n"
         << "Video encoding is done at 'constant quality' and will adjust \n"
         << "the bitrate automatically. 'lookahead' allows for more efficient\n"
         << "use of bits while maintaining the quality at the expense of RAM.\n"
         << "Set lookahead to 0 to disable, or -1 to use the encoder default.\n";

    cerr << "\n"
         << "Examples:\n"
         << "\tCapture from input 2 and write Transport Stream to stdout:\n"
         << "\t" << app << " -i 2 -m\n"
         << "\n"
         << "\tWrite EDID to input 3 and capture audio and video:\n"
         << "\t" << app << " -i 3 -w ProCaptureHDMI-EAC3.bin -m\n"
         << "\n"
         << "\tSet Volume of input 1 to max and capture to TS:\n"
         << "\t" << app << " -i 1 -s 100 -m\n";

    cerr << "\nNOTE: setting EDID does not survive a reboot.\n";
}

bool string_to_int(string_view st, int &value, string_view var)
{
    auto result = std::from_chars(st.data(), st.data() + st.size(),
                                  value);
    if (result.ec == std::errc::invalid_argument)
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

    int    boardId  = -1;
    int    devIndex = -1;

    string_view app_name = argv[0];
    string      edid_file;
    string      video_codec = "hevc_nvenc";
    int         verbose = 1;

    bool        get_volume = false;
    int         set_volume = -1;

    bool        do_capture = false;
    bool        read_edid  = false;
    bool        write_edid = false;

    int         look_ahead = 32;
    bool        no_audio = false;

    vector<string_view> args(argv + 1, argv + argc);

    for (auto iter = args.begin(); iter != args.end(); ++iter)
    {
        if (*iter == "-h" || *iter == "--help")
        {
            show_help(app_name);
            return 0;
        }
        else if (*iter == "-l" || *iter == "--lookahead")
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
        else if (*iter == "-v" || *iter == "--verbose")
        {
            if (iter + 1 == args.end())
                verbose = 1;
            else
            {
                if (!string_to_int(*(++iter), verbose, "verbose"))
                    exit(1);
            }
        }
        else
        {
            cerr << "Unrecognized option '" << *iter << "'\n";
            exit(1);
        }
    }

    if(!MWCaptureInitInstance())
    {
        cerr << "have InitilizeFailed\n";
        return -1;
    }

    HCHANNEL channel_handle = open_channel(verbose, devIndex - 1,
                                           boardId);
    if (channel_handle == nullptr)
        return -1;

    if (get_volume)
        display_volume(channel_handle);
    if (set_volume >= 0)
        save_volume(channel_handle, verbose, set_volume);

    if (!edid_file.empty())
    {
        if (read_edid)
            ReadEDID(channel_handle, edid_file);
        else if (write_edid)
            WriteEDID(channel_handle, edid_file);
    }

    if (do_capture)
    {
        OutputTS out2ts(verbose, video_codec, look_ahead, no_audio);

        if (!capture(channel_handle, verbose, out2ts, no_audio))
            return -1;
    }

    if (channel_handle != nullptr)
        MWCloseChannel(channel_handle);

    MWCaptureExitInstance();

    return 0;
}
