/*
 * magewell2ts
 * Copyright (c) 2022-2026 John Patrick Poet
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
#include <iomanip>
#include <cstring>
#include <algorithm>
#include <array>
#include <limits>
#include <unistd.h>
#include <cstddef>
#include <memory>
#include <sys/resource.h>
#include <sys/mman.h> // for mlock

#include <MWFOURCC.h>
#include <LibMWCapture/MWCapture.h>
#include "LibMWCapture/MWEcoCapture.h"

#include <sys/eventfd.h>

#include "Magewell.h"

//#define DUMP_RAW_AUDIO_ALLBITS
//#define DUMP_RAW_AUDIO

#if defined(DUMP_RAW_AUDIO) || defined(DUMP_RAW_AUDIO_ALLBITS)
#include <fstream>
#endif

using namespace std;

/**
 * @brief Get video signal status string
 * @param state Video signal state
 * @return String representation of the signal state
 */
static string GetVideoSignal(int state)
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

/**
 * @brief Get video input type string
 * @param type Video input type
 * @return String representation of the input type
 */
static string GetVideoInputType(DWORD type)
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

/**
 * @brief Get video color format string
 * @param color Video color format
 * @return String representation of the color format
 */
static string GetVideoColorName(MWCAP_VIDEO_COLOR_FORMAT color)
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

/**
 * @brief Get SDI type string
 * @param type SDI type
 * @return String representation of the SDI type
 */
static string GetVideoSDIType(SDI_TYPE type)
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

/**
 * @brief Get scanning format string
 * @param type Scanning format
 * @return String representation of the scanning format
 */
static string GetVideoScanFmt(SDI_SCANNING_FORMAT type)
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

/**
 * @brief Get sampling structure string
 * @param type Sampling structure
 * @return String representation of the sampling structure
 */
static string GetVideoSamplingStruct(SDI_SAMPLING_STRUCT type)
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

/**
 * @brief Get bit depth string
 * @param type Bit depth
 * @return String representation of the bit depth
 */
static string GetVideoBitDepth(SDI_BIT_DEPTH type)
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

/**
 * @brief Get VGA sync type string
 * @param type VGA sync type
 * @return String representation of the sync type
 */
static string GetVideoSyncType(BYTE type)
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

/**
 * @brief Get SD video standard string
 * @param type SD video standard
 * @return String representation of the standard
 */
static string GetVideoSDStandard(MWCAP_SD_VIDEO_STANDARD type)
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

/**
 * @brief Constructor for Magewell class
 *
 * Initializes the MWCapture library instance. If initialization fails,
 * sets fatal error flag.
 *
 * @note This function calls MWCaptureInitInstance() to initialize the SDK.
 */
Magewell::Magewell(void)
{
    m_log = spdlog::get("app_logger");
    if (!m_log)
    {
        // Handle error if logger not found (e.g., create a fallback or throw exception)
        std::cerr << "Magewell Error: Logger 'app_logger' not found!" << std::endl;
        m_fatal = true;
        return;
    }

    // Initialize the MWCapture SDK instance
    if (!MWCaptureInitInstance())
    {
        // If initialization fails, output error and set fatal flag
        m_log->error("Failed to initialize MWCapture.");
        m_fatal = true;
    }
    // Initialize last reset time
    m_last_reset = std::chrono::steady_clock::now();
}

/**
 * @brief Destructor for Magewell class
 *
 * Cleans up resources by closing the channel and exiting the MWCapture instance.
 * Ensures proper cleanup of all allocated resources.
 */
Magewell::~Magewell(void)
{
    // Close the channel if it's open
    if (m_channel)
        MWCloseChannel(m_channel);

    // Exit the MWCapture SDK instance
    MWCaptureExitInstance();
}

/**
 * @brief Describe input channel information
 *
 * Retrieves and displays detailed information about the video input signal,
 * including signal status, input type, color format, and audio information.
 *
 * @param hChannel Handle to the channel to describe
 * @return true if successful, false otherwise
 */
string Magewell::describe_input(HCHANNEL hChannel)
{
    MW_RESULT xr;
    MWCAP_VIDEO_SIGNAL_STATUS vStatus;
    MWCAP_INPUT_SPECIFIC_STATUS status;

    // Get input specific status
    xr = MWGetInputSpecificStatus(hChannel, &status);

    // Check if we got valid status
    if (xr != MW_SUCCEEDED ||
        MWGetVideoSignalStatus(hChannel, &vStatus) != MW_SUCCEEDED)
    {
        return "Failed to get video signal status.";
    }

    // Check if there's a valid signal
    if (!status.bValid)
    {
        return "No signal detected on input.";
    }

    // Output basic video signal information
    string msg = format("Video Signal {}: {}", GetVideoSignal(vStatus.state),
                        GetVideoInputType(status.dwVideoInputType));

    // Output HDMI-specific information
    if (status.dwVideoInputType == MWCAP_VIDEO_INPUT_TYPE_HDMI)
    {
        msg += format("  HDCP: {}"
                      ", Mode: {}"
                      ", Bit Depth: {}",
                      status.hdmiStatus.bHDCP ? "Yes" : "No",
                      static_cast<int>(status.hdmiStatus.bHDMIMode),
                      static_cast<int>(status.hdmiStatus.byBitDepth));
    }
    // Output SDI-specific information
    else if (status.dwVideoInputType == MWCAP_VIDEO_INPUT_TYPE_SDI)
    {
        msg += format("  Type: {}"
                      ", Scan Fmt: {}"
                      ", Bit depth: {}"
                      ", Sampling: {}",
                      GetVideoSDIType(status.sdiStatus.sdiType),
                      GetVideoScanFmt(status.sdiStatus.sdiScanningFormat),
                      GetVideoBitDepth(status.sdiStatus.sdiBitDepth),
                      GetVideoSamplingStruct(status.sdiStatus.sdiSamplingStruct));
    }
    // Output VGA-specific information
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

        msg += format("  ScanType: {}"
                      ", bHSPolarity: {}"
                      ", bVSPolarity: {}"
                      ", Interlaced: {}"
                      ", FrameDuration: {}",
                      GetVideoSyncType(status.vgaComponentStatus.syncInfo.bySyncType),
                      status.vgaComponentStatus.syncInfo.bHSPolarity,
                      status.vgaComponentStatus.syncInfo.bVSPolarity,
                      status.vgaComponentStatus.syncInfo.bInterlaced,
                      dFrameDuration);
    }
    // Output CVBS-specific information
    else if (status.dwVideoInputType == MWCAP_VIDEO_INPUT_TYPE_CVBS) {
        msg += format("  Standard: {}"
                      ", b50Hz: {}",
                      GetVideoSDStandard(status.cvbsYcStatus.standard),
                      status.cvbsYcStatus.b50Hz);
    }
    msg += format("  {}\n", GetVideoColorName(vStatus.colorFormat));

    // Calculate frame duration
    double dFrameDuration = (vStatus.bInterlaced == TRUE)
                            ? (double)20000000
                            / vStatus.dwFrameDuration
                            : (double)10000000
                            / vStatus.dwFrameDuration;
    dFrameDuration = static_cast<int>(dFrameDuration * 100)
                     / 100.0;

    // Output resolution and timing information
    msg += format("\t{}x{}{}{}"
                  "[x:{}, y:{}] "
                  "total ({}x{}) "
                  "aspect {}:{}\n",
                  vStatus.cx, vStatus.cy,
                  vStatus.bInterlaced ? "i" : "p",
                  dFrameDuration,
                  vStatus.x, vStatus.y,
                  vStatus.cxTotal, vStatus.cyTotal,
                  vStatus.nAspectX, vStatus.nAspectY);

    // Audio Signal Status
    MWCAP_AUDIO_SIGNAL_STATUS aStatus;
    xr = MWGetAudioSignalStatus(hChannel, &aStatus);
    if (xr == MW_SUCCEEDED)
    {
        msg += format("\tAudio Signal {}",
                      aStatus.bChannelStatusValid ? "Valid" : "Invalid");
        if (!aStatus.bChannelStatusValid)
            return msg;

        // Output audio channel information
        msg += ", Channels: ";
        for (int i = 0; i < 4; ++i)
        {
            if (aStatus.wChannelValid & (0x01 << i))
                msg += " " + std::to_string(i * 2 + 1) + "&" +
                            std::to_string(i * 2 + 2);
        }
        msg += format(", {}"
                      ", BPSample: {}"
                      ", Sample Rate: {}",
                      aStatus.bLPCM ? "lPCM" : "Bitstream",
                      static_cast<int>(aStatus.cBitsPerSample),
                      aStatus.dwSampleRate);
    }

    return msg;
}

/**
 * @brief List all available input channels
 *
 * Enumerates all available capture channels and displays their information.
 * This function is used for device discovery and status reporting.
 */
void Magewell::ListInputs(void)
{
    HCHANNEL hChannel = nullptr;
    MWCAP_CHANNEL_INFO prev_channelInfo = { 0 };

    // Refresh device list
    MWRefreshDevice();

    int num_channels = MWGetChannelCount();
    int idx;

    // Display number of channels found
    m_log->info("{} channels.", num_channels);

    // Iterate through all channels
    for (idx = 0; idx < num_channels; ++idx)
    {
        char path[128] = { 0 };

        // Get device path for this channel
        MWGetDevicePath(idx, path);
        hChannel = MWOpenChannelByPath(path);
        if (hChannel == nullptr)
        {
            m_log->error("failed to open input {}", idx);
            continue;
        }

        MWCAP_CHANNEL_INFO channelInfo = { 0 };

        // Get channel information
        if (MW_SUCCEEDED != MWGetChannelInfo(hChannel, &channelInfo))
        {
            m_log->error("failed to get channel info for input {}", idx);
            continue;
        }

        // Null-terminate strings to prevent buffer overflows
        channelInfo.szFamilyName[sizeof(channelInfo.szFamilyName)-1] = '\0';
        channelInfo.szProductName[sizeof(channelInfo.szProductName)-1] = '\0';
        channelInfo.szBoardSerialNo[sizeof(channelInfo.szBoardSerialNo)-1] = '\0';

        // Display board information if different from previous
        if (channelInfo.byBoardIndex != prev_channelInfo.byBoardIndex ||
            strcmp(channelInfo.szFamilyName,
                   prev_channelInfo.szFamilyName) != 0 ||
            strcmp(channelInfo.szProductName,
                   prev_channelInfo.szProductName) != 0 ||
            strcmp(channelInfo.szBoardSerialNo,
                   prev_channelInfo.szBoardSerialNo) != 0)
        {
            m_log->info("Board: {}"
                        ", Product: {}"
                         ", SerialNo: {}\n"
                         "\tFirmware: {}"
                         ", Driver: {}",
                        static_cast<int>(channelInfo.byBoardIndex),
                        channelInfo.szProductName,
                        channelInfo.szBoardSerialNo,
                        channelInfo.dwFirmwareVersion,
                        channelInfo.dwDriverVersion);
        }
        prev_channelInfo = channelInfo;

        // Display channel information
        m_log->info("[{}] {}", idx + 1,
                    describe_input(hChannel));

        // Close channel after use
        MWCloseChannel(hChannel);
    }
}

/**
 * @brief Wait for a specific number of input channels to be available
 *
 * Waits for a specified number of capture channels to become available.
 *
 * @param cnt Number of channels to wait for
 * @return true if channels are available, false otherwise
 */
bool Magewell::WaitForInputs(int cnt) const
{
    int idx = 10;

    do
    {
        // Refresh device list
        if (MWCaptureInitInstance())
        {
            // Check if we have enough channels
            if (MWGetChannelCount() >= cnt)
            {
                MWCaptureExitInstance(); // This was commented out, so not called
                return true;
            }
            MWCaptureExitInstance(); // This was commented out, so not called
        }
        sleep(1);
    }
    while (--idx);

    return false;
}

/**
 * @brief Open a video capture channel
 *
 * Opens a specific video capture channel by either board ID or channel index.
 *
 * @param devIndex Index of the device to open
 * @param boardId Board identifier (use -1 for default)
 * @return true if successful, false otherwise
 */
bool Magewell::OpenChannel(int devIndex, double boardId)
{
    int channel_cnt =  MWGetChannelCount();

    // Lock clog for thread-safe output
    // Check if any channels are available
    if (channel_cnt == 0)
    {
        m_log->error("Failed to detect any input channels.");
        m_fatal = true;
        return false;
    }

    // Get <board id > <channel id> or <channel index>
    // Open channel
    if (boardId >= 0)
        m_channel = MWOpenChannel(boardId, devIndex);
    else
    {
        // Check if requested index is valid
        if (channel_cnt < devIndex)
        {
            m_log->error("Only {} input channels detected. Cannot open input {}",
                         channel_cnt, devIndex);
            m_fatal = true;
            return false;
        }

        char path[128] = {0};
        MWGetDevicePath(devIndex, path);
        m_channel = MWOpenChannelByPath(path);
    }

    // Check if channel was opened successfully
    if (m_channel == nullptr)
    {
        m_log->error("Failed to open input channel {}:{}", boardId, devIndex + 1);
    }

    m_channel_idx = devIndex;

    // Get channel info
    MWCAP_CHANNEL_INFO channel_info = { 0 };
    if (MW_SUCCEEDED != MWGetChannelInfo(m_channel, &channel_info))
    {
        m_log->error("Unable to retrieve channel info for index {}!",
                     m_channel_idx);
        return false;
    }

    // Display channel info if verbose mode is enabled
    if (m_verbose > 0)
    {
        uint temperature = 0;
        MWGetTemperature(m_channel, &temperature);

        m_log->info("Board: {}, Product: {}, SerialNo: {}\n"
                    "\tFirmware: {}, Driver: {}, Temperature: {:.1f}ºC",
                    static_cast<int>(channel_info.byBoardIndex),
                    channel_info.szProductName,
                    channel_info.szBoardSerialNo,
                    channel_info.dwFirmwareVersion,
                    channel_info.dwDriverVersion,
                    static_cast<float>(temperature) / 10);
    }

    // Set channel info and determine if using ECO mode
    channel_info.szFamilyName[sizeof(channel_info.szFamilyName)-1] = '\0';
    m_channel_info = channel_info;
    m_isEco = strcmp(m_channel_info.szFamilyName, "Eco Capture") == 0;

    // Get input status
    MWCAP_INPUT_SPECIFIC_STATUS status;
    if (MWGetInputSpecificStatus(m_channel, &status) != MW_SUCCEEDED)
        m_log->error("Unable to get input status!");
    else if(!status.bValid)
        m_log->warn("No signal detected on input.");

    return true;
}

/**
 * @brief Close the currently open channel
 *
 * Closes the currently opened channel.
 *
 * @return true always
 */
bool Magewell::CloseChannel(void)
{
    MWCloseChannel(m_channel);
    return true;
}

/**
 * @brief Display current audio volume settings
 *
 * Retrieves and displays the current audio volume settings for the channel.
 */
void Magewell::DisplayVolume(void)
{
    MWCAP_AUDIO_VOLUME volume;
    /*
      uint8_t   byChannels
      uint8_t   byReserved
      int16_t   sVolumeMin
      int16_t   sVolumeMax
      int16_t   sVolumeStep
      bool_t    abMute [MWCAP_MAX_NUM_AUDIO_CHANNEL]
      int16_t   asVolume [MWCAP_MAX_NUM_AUDIO_CHANNEL]
    */
    MWCAP_AUDIO_NODE node = MWCAP_AUDIO_EMBEDDED_CAPTURE;
    MWGetAudioVolume(m_channel, node, &volume);

    // Display volume range
    m_log->info("Volume Channels: {}, Min:{}, Max:{}, Step{}",
                volume.byChannels, volume.sVolumeMin,
                volume.sVolumeMax, volume.sVolumeStep);

    // Display volume for each channel
    for(int idx=0; idx<MWCAP_MAX_NUM_AUDIO_CHANNEL; ++idx)
    {
        m_log->info("[{}] Mute: {}, Volume: {}", idx,
                    volume.abMute[idx] ? "Yes" : "No",
                    volume.asVolume[idx]);
    }
}

/**
 * @brief Set audio volume level
 *
 * Sets the audio volume level for all channels.
 *
 * @param volume_level Volume level (0-100)
 * @return true always
 */
bool Magewell::SetVolume(int volume_level)
{
    MWCAP_AUDIO_VOLUME volume;
    _MWCAP_AUDIO_NODE  node = MWCAP_AUDIO_EMBEDDED_CAPTURE;

    // Get current volume settings
    MWGetAudioVolume(m_channel, node, &volume);

#if 0
    // Calculate scaled volume
    float scale = (volume.sVolumeMax - volume.sVolumeMin) / 100;
    int scaled_volume = volume_level * scale;
#endif

    // Apply volume to all channels
    for(int idx=0; idx<MWCAP_MAX_NUM_AUDIO_CHANNEL; ++idx)
    {
        volume.abMute[idx] = false;
#if 0
        volume.asVolume[i] = scaled_volume + volume.sVolumeMin;
#else
        volume.asVolume[idx] = volume_level;
#endif
    }

    // Set new volume
    MWSetAudioVolume(m_channel, node, &volume);

    // Display confirmation if verbose mode is enabled
    if (m_verbose > 0)
        m_log->info("Volume set to {} for all channels.", volume_level);

    return true;
}

/**
 * @brief Read EDID information from the device
 *
 * Reads EDID data from the HDMI input and saves it to a file.
 *
 * @param filepath Path to save EDID data
 * @return true if successful, false otherwise
 */
bool Magewell::ReadEDID(const string & filepath)
{
    DWORD dwVideoSource = 0;
    DWORD dwAudioSource = 0;

    // Get video and audio source information
    if (MW_SUCCEEDED != MWGetVideoInputSource(m_channel, &dwVideoSource))
    {
        m_log->error("Can't get video input source!");
        return false;
    }

    if (MW_SUCCEEDED != MWGetAudioInputSource(m_channel, &dwAudioSource))
    {
        m_log->error("Can't get audio input source!");
        return false;
    }

    // Verify both are HDMI
    if (INPUT_TYPE(dwVideoSource) != MWCAP_VIDEO_INPUT_TYPE_HDMI ||
        INPUT_TYPE(dwAudioSource) != MWCAP_AUDIO_INPUT_TYPE_HDMI)
    {
        m_log->error("Type of input source is not HDMI!");
        return false;
    }

    MW_RESULT xr;
    FILE* pFile = nullptr;
    pFile=fopen(filepath.c_str(), "wb");
    if (pFile == nullptr)
    {
        m_log->error("Could not read EDID file '{}'", filepath);
        return false;
    }

    ULONG ulSize = 256;
    BYTE byData[256];

    // Read EDID data
    xr = MWGetEDID(m_channel, byData, &ulSize);
    if (xr == MW_SUCCEEDED)
    {
        ULONG nWriteSize = (int)fwrite(byData, 1, 256, pFile);

        if (nWriteSize == ulSize)
        {
            m_log->info("Wrote EDID to '{}'", filepath);
        }
        else
        {
            m_log->error("Failed to write to '{}' - {}", filepath, strerror(errno));
        }
    }
    else
    {
        m_log->error("Get EDID Info!");
    }

    // Clean up file handle
    fclose(pFile);
    pFile = NULL;

    return true;
}

/**
 * @brief Write EDID information to the device
 *
 * Writes EDID data from a file to the HDMI input.
 *
 * @param filepath Path to EDID data file
 * @return true if successful, false otherwise
 */
bool Magewell::WriteEDID(const string & filepath)
{
    DWORD dwVideoSource = 0;
    DWORD dwAudioSource = 0;

    // Get video and audio source information
    if (MW_SUCCEEDED != MWGetVideoInputSource(m_channel, &dwVideoSource))
    {
        m_log->error("Can't get video input source!");
        return false;
    }

    if (MW_SUCCEEDED != MWGetAudioInputSource(m_channel, &dwAudioSource))
    {
        m_log->error("Can't get audio input source!");
        return false;
    }

    // Verify both are HDMI
    if (INPUT_TYPE(dwVideoSource) != MWCAP_VIDEO_INPUT_TYPE_HDMI ||
        INPUT_TYPE(dwAudioSource) != MWCAP_AUDIO_INPUT_TYPE_HDMI)
    {
        m_log->error("Type of input source is not HDMI!");
        return false;
    }

    MW_RESULT xr;

    // Open file for reading
    FILE* pFile = nullptr;
    pFile=fopen(filepath.c_str(), "rb");
    if (pFile == nullptr)
    {
        m_log->error("could not read from EDID file '{}'!", filepath);
        return false;
    }

    BYTE byData[1024];
    int nSize = (int)fread(byData, 1, 1024, pFile);

    // Write EDID data
    xr = MWSetEDID(m_channel, byData, nSize);
    if (xr == MW_SUCCEEDED)
        m_log->info("EDID written successfully.");
    else
        m_log->error("Failed to write EDID!");

    // Clean up file handle
    fclose(pFile);
    pFile = NULL;

    return true;
}

/**
 * @brief Wait for event with timeout
 *
 * Waits for an event to occur with a specified timeout.
 *
 * @param event Event file descriptor
 * @param timeout Timeout in milliseconds
 * @return Event result
 */
using mw_event_t = int;
int EcoEventWait(mw_event_t event, int timeout/*ms*/)
{
    fd_set rfds;
    struct timeval tv;
    struct timeval *ptv = NULL;
    eventfd_t value = 0;
    int retval;

    FD_ZERO(&rfds);
    FD_SET(event, &rfds);

    // Set up timeout
    if (timeout < 0)
    {
        ptv = NULL;
    }
    else if (timeout == 0)
    {
        tv.tv_sec = timeout / 1000;
        tv.tv_usec = (timeout % 1000) * 1000;
        ptv = &tv;
    }
    else
    {
        tv.tv_sec = timeout / 1000;
        tv.tv_usec = (timeout % 1000) * 1000;
        ptv = &tv;
    }

    // Wait for event
    retval = select(event + 1, &rfds, NULL, NULL, ptv);
    if (retval == -1)
        return retval;
    else if (retval > 0)
    {
        retval = eventfd_read(event, &value);
        if (value > 0)
        {
            return value;
        }
        else
        {
            return retval < 0 ? retval : -1;
        }
    }

    // timeout
    return 0;
}

/**
 * @brief Capture audio data from the device
 *
 * Main audio capture loop that handles audio frame acquisition and processing.
 */
void Magewell::capture_audio_loop(void)
{
    bool      good_signal = true;
    bool      lpcm = false;
    int       bytes_per_sample = 0;
    int       even_bytes_per_sample = 0;
    unsigned int sample_rate  = 0;
    WORD      valid_channels = 0;
    MWCAP_PTR notify_event = 0;
    int       eco_event = 0;
    HNOTIFY   notify_audio = 0;
    DWORD     input_count = 0;
    int       cur_channels;

    MWCAP_AUDIO_SIGNAL_STATUS audio_signal_status;
    int err_cnt = 0;
    int frame_cnt = 0;

    int      frame_size      = 0;
    int      channel_pairs   = 0;
    int      shift           = 0;

    bool     params_changed  = false;

    ULONGLONG notify_status = 0;
    MWCAP_AUDIO_CAPTURE_FRAME macf;

    const int half_channels = MWCAP_AUDIO_MAX_NUM_CHANNELS / 2;
    const int sample_stride = MWCAP_AUDIO_MAX_NUM_CHANNELS;

    if (m_verbose > 2)
    {
        m_log->info("Starting audio capture loop");
    }

#ifdef DUMP_RAW_AUDIO_ALLBITS
    ofstream fraw_all;
    fraw_all.open("raw-audio-allbits.bin", ofstream::binary);
#endif
#ifdef DUMP_RAW_AUDIO
    ofstream fraw;
    fraw.open("raw-audio.bin", ofstream::binary);
#endif

    // Get audio input source array
    MWGetAudioInputSourceArray(m_channel, nullptr, &input_count);
    if (input_count == 0)
    {
        if (m_verbose > 0)
        {
            m_log->error("can't find audio input.");
        }
        goto audio_capture_stoped;
    }

    // Start audio capture
    if (MW_SUCCEEDED != MWStartAudioCapture(m_channel))
    {
        if (m_verbose > 0)
        {
            m_log->error("start audio capture fail!");
        }
        goto audio_capture_stoped;
    }

    // Set up notification based on capture mode
    if (m_isEco)
    {
        eco_event = eventfd(0, EFD_NONBLOCK);
        if (eco_event < 0)
        {
            m_log->critical("Failed to create eco event.");
            Shutdown();
            return;
        }
        notify_audio  = MWRegisterNotify(m_channel, eco_event,
                                         (DWORD)MWCAP_NOTIFY_AUDIO_FRAME_BUFFERED
//                                     | (DWORD)MWCAP_NOTIFY_AUDIO_SIGNAL_CHANGE
                                       | (DWORD)MWCAP_NOTIFY_AUDIO_INPUT_RESET
                                         );
    }
    else
    {
        notify_event = MWCreateEvent();
        if (notify_event == 0)
        {
            m_log->critical("create notify_event fail.");
            Shutdown();
            return;
        }
        notify_audio  = MWRegisterNotify(m_channel, notify_event,
                                         (DWORD)MWCAP_NOTIFY_AUDIO_FRAME_BUFFERED |
                                         (DWORD)MWCAP_NOTIFY_AUDIO_SIGNAL_CHANGE  |
                                         (DWORD)MWCAP_NOTIFY_AUDIO_INPUT_RESET);
    }

    // Display starting message if verbose
    if (m_verbose > 1)
    {
        m_log->info("Audio capture starting");
    }

    // Main audio capture loop
    while (m_running.load() == true)
    {
        // Get audio signal status
        if (MW_SUCCEEDED != MWGetAudioSignalStatus(m_channel,
                                                   &audio_signal_status))
        {
            if (m_verbose > 0 && ++err_cnt % 50 == 0)
            {
                m_log->warn("cnt {}: can't get audio signal status.", err_cnt);
            }
            this_thread::sleep_for(chrono::milliseconds(m_frame_ms));
            continue;
        }

        // Check if audio signal is valid
        if (!audio_signal_status.bChannelStatusValid)
        {
            if (good_signal && m_verbose > 0 && ++err_cnt % 100 == 0)
            {
                m_log->info("No audio signal.");
            }
            good_signal = false;
            this_thread::sleep_for(chrono::milliseconds(m_frame_ms * 2));
            continue;
        }
        good_signal = true;

        // Calculate bytes per sample
        even_bytes_per_sample = audio_signal_status.cBitsPerSample / 8;
        if (even_bytes_per_sample > 2)
            even_bytes_per_sample = 4;

        {
            // Check for parameter changes
            if (m_reset_audio.load() == true)
            {
                if (m_verbose > 1)
                    m_log->info("Audio re-initializing.");
                params_changed = true;
            }
            if (lpcm != audio_signal_status.bLPCM)
            {
                if (m_verbose > 1)
                {
                    if (lpcm)
                        m_log->info("lPCM -> Bitstream");
                    else
                        m_log->info("Bitstream -> lPCM");
                }
                lpcm = audio_signal_status.bLPCM;
                params_changed = true;
            }
            if (sample_rate != audio_signal_status.dwSampleRate)
            {
                if (m_verbose > 1)
                    m_log->info("Audio sample rate {} -> {}", sample_rate,
                                audio_signal_status.dwSampleRate);
                sample_rate = audio_signal_status.dwSampleRate;
                params_changed = true;
            }
            if (bytes_per_sample != even_bytes_per_sample)
            {
                if (m_verbose > 1)
                    m_log->info("Audio bytes per sample {} -> {}",
                                bytes_per_sample,
                                even_bytes_per_sample);
                bytes_per_sample = even_bytes_per_sample;
                params_changed = true;
            }
            if (valid_channels != audio_signal_status.wChannelValid)
            {
                if (m_verbose > 1)
                    m_log->info("Audio channels {} -> {}", valid_channels,
                                audio_signal_status.wChannelValid);
                valid_channels = audio_signal_status.wChannelValid;
                params_changed = true;
            }
        }

        // Handle parameter changes
        if (params_changed)
        {
            params_changed = false;

            if (m_verbose > 1 /* && frame_cnt > 0 */)
                m_log->info("Audio signal CHANGED after {} frames.", frame_cnt);

            cur_channels = 0;
            for (int idx = 0; idx < (MWCAP_AUDIO_MAX_NUM_CHANNELS / 2); ++idx)
            {
                cur_channels +=
                    (valid_channels & (0x01 << idx)) ? 2 : 0;
            }

            if (0 == cur_channels)
            {
                if (m_verbose > 0 && err_cnt++ % 25 == 0)
                {
                    m_log->warn("Invalid audio channel count: {}",
                                cur_channels);
                }

                this_thread::sleep_for(chrono::milliseconds(m_frame_ms));
                continue;
            }

            frame_size = MWCAP_AUDIO_SAMPLES_PER_FRAME
                         * cur_channels * even_bytes_per_sample;

            channel_pairs = cur_channels / 2;
            shift = audio_signal_status.cBitsPerSample > 16 ? 0 : 16;

            // Set audio parameters in output handler
            m_out2ts->setAudioParams(cur_channels, lpcm,
                                     bytes_per_sample,
                                     sample_rate,
                                     MWCAP_AUDIO_SAMPLES_PER_FRAME,
                                     frame_size);

            m_reset_audio.store(false);
        }

        err_cnt = 0;
        frame_cnt = 0;
        while (m_reset_audio.load() == false)
        {
            // Wait for notification
            if (m_isEco)
            {
                if (EcoEventWait(eco_event, m_frame_ms) <= 0)
                {
                    if (m_verbose > 1)
                        m_log->info("Waiting for audio data.");
                    continue;
                }
            }
            else
            {
                if (MWWaitEvent(notify_event, m_frame_ms) <= 0)
                {
                    if (m_verbose > 1)
                        m_log->info("Waiting for audio data.");
                    continue;
                }
            }

            // Get notification status
            if (MW_SUCCEEDED != MWGetNotifyStatus(m_channel,
                                                  notify_audio,
                                                  &notify_status))
                continue;

            // Can be spurious from "bad" devices (And eco capture cards).
            if (notify_status & MWCAP_NOTIFY_AUDIO_SIGNAL_CHANGE)
            {
                if (m_verbose > 0)
                    m_log->info("AUDIO signal changed.");
                this_thread::sleep_for(chrono::milliseconds(m_frame_ms));
                break;
            }

            // Handle input reset
            if (notify_status & MWCAP_NOTIFY_AUDIO_INPUT_RESET)
            {
                this_thread::sleep_for(chrono::milliseconds(m_frame_ms));
                break;
            }

            // Check if frame is buffered
            if (!(notify_status & MWCAP_NOTIFY_AUDIO_FRAME_BUFFERED))
                continue;

            for (;;)
            {
                // Capture audio frame
                if (MW_ENODATA == MWCaptureAudioFrame(m_channel, &macf))
                    break;

                ++frame_cnt;

#ifdef DUMP_RAW_AUDIO_ALLBITS
                /*
                  Audio sample data. Each sample is 32-bit width, and
                  high bit effective. The priority of the path is:
                  Left0, Left1, Left2, Left3, right0, right1, right2,
                  right3.
                */
                uint8_t mark = 0xcc;
                for (int idx = 0; idx < 16; ++idx)
                    fraw_all.write(reinterpret_cast<const char*>(&mark), sizeof(uint8_t));

                for (int idx = 0;
                     idx < MWCAP_AUDIO_SAMPLES_PER_FRAME * MWCAP_AUDIO_MAX_NUM_CHANNELS;
                     ++idx)
                {
                    fraw_all.write(reinterpret_cast<char*>(&macf.adwSamples[idx]),
                                   sizeof(DWORD));
                }
#endif

                // Create audio frame buffer
                AudioBuffer::AudioFrame* audio_frame = new AudioBuffer::AudioFrame;
                audio_frame->resize(frame_size);
                uint8_t* output_ptr = audio_frame->data();

                for (int pair = 0; pair < channel_pairs; ++pair)
                {
                    const uint32_t* left_samples = &macf.adwSamples[pair];
                    const uint32_t* right_samples = &macf.adwSamples[pair +
                                                                     half_channels];

                    for (int sample = 0;
                         sample < MWCAP_AUDIO_SAMPLES_PER_FRAME; ++sample)
                    {
                        uint32_t left_raw = left_samples[sample * sample_stride];
                        uint32_t right_raw = right_samples[sample * sample_stride];

                        uint32_t left_val = left_raw >> shift;
                        uint32_t right_val = right_raw >> shift;

                        /* For 32-bit samples, we copy 4 bytes,
                         * for 16-bit samples, we copy 2 bytes
                         */
                        std::memcpy(output_ptr, &left_val, even_bytes_per_sample);
                        output_ptr += even_bytes_per_sample;
                        std::memcpy(output_ptr, &right_val, even_bytes_per_sample);
                        output_ptr += even_bytes_per_sample;
                    }
                }

#ifdef DUMP_RAW_AUDIO
                /*
                  Bitstream Audio: Each sample is 16-bits for L1 and 16-bits for R1
                  16-bit PCM: Each sample is 16-bits for each valid channel: L1R1L2R2, etc...
                  24-bit PCM: Each sample is 32-bits for each valid channel: L1R1L2R2, etc...
                */
                uint8_t mark = 0xcc;
                for (int idx = 0; idx < 16; ++idx)
                    fraw.write(reinterpret_cast<const char*>(&mark), sizeof(uint8_t));

                AudioBuffer::AudioFrame::const_iterator Itr;
                for (Itr = audio_frame->begin(); Itr != audio_frame->end(); ++Itr)
                {
                    fraw.write(reinterpret_cast<const char*>(&(*Itr)), sizeof(uint8_t));
                }
#endif

                // Add frame to output handler
                m_out2ts->addAudio(audio_frame, macf.llTimestamp);
            }
        }
    }

  audio_capture_stoped:
    m_log->info("Audio Capture finished.");

    // Clean up notification resources
    if (notify_audio)
    {
        MWUnregisterNotify(m_channel, notify_audio);
        notify_audio = 0;
    }

    if (eco_event)
    {
        eventfd_write(eco_event, 1);
        close(eco_event);
    }

    // Stop audio capture
    MWStopAudioCapture(m_channel);

    if (notify_event!= 0)
    {
        MWCloseEvent(notify_event);
        notify_event = 0;
    }
}

void Magewell::capture_audio(void)
{
    while (m_running.load() == true)
    {
        capture_audio_loop();
    }

    // Make sure other threads stop.
    Shutdown();
}

/**
 * @brief Update HDR information from info frames
 *
 * Retrieves and processes HDR info frame data to update HDR metadata.
 *
 * @return true if successful, false otherwise
 */
bool Magewell::update_HDRinfo(void)
{
    unsigned int uiValidFlag = 0;
    if (MW_SUCCEEDED != MWGetHDMIInfoFrameValidFlag(m_channel, &uiValidFlag))
    {
        m_log->info("Not a HDMI info frame");
        return false;
    }

    if (0 == uiValidFlag)
    {
        m_log->info("No HDMI InfoFrame!");
        return false;
    }

    if (0 == (uiValidFlag & MWCAP_HDMI_INFOFRAME_MASK_HDR))
        return false;

    // Get HDR info frame
    if (MW_SUCCEEDED != MWGetHDMIInfoFramePacket(m_channel,
                                                 MWCAP_HDMI_INFOFRAME_ID_HDR,
                                                 &m_infoPacket))
    {
        m_log->warn("HDMI HDR infoframe not available.");
        return false;
    }

    // Check EOTF (Electro-Optical Transfer Function)
    if (static_cast<int>(m_HDRinfo.byEOTF) != 2 &&
        static_cast<int>(m_HDRinfo.byEOTF) != 3)
        return false;

    // Check if HDR info has changed
    if (memcmp(&m_HDRinfo, &m_HDRinfo_prev,
               sizeof(HDMI_HDR_INFOFRAME_PAYLOAD)) == 0)
    {
        m_log->info("HDR info has not changed.");
        return true;
    }

    // Store previous HDR info
    memcpy(&m_HDRinfo_prev, &m_HDRinfo,
           sizeof(HDMI_HDR_INFOFRAME_PAYLOAD));

    // Allocate and populate mastering display metadata
    AVMasteringDisplayMetadata* meta = av_mastering_display_metadata_alloc();

    // Primaries
    meta->has_primaries = 1;

    // CIE 1931 xy chromaticity coords of color primaries (r, g, b order)
    // RED x
    meta->display_primaries[0][0].num =
        static_cast<int32_t>
        (static_cast<uint16_t>(m_HDRinfo.display_primaries_lsb_x0) |
         (static_cast<uint16_t>(m_HDRinfo.display_primaries_msb_x0) << 8));
    meta->display_primaries[0][0].den = 1;

    // RED y
    meta->display_primaries[0][1].num =
        static_cast<int32_t>
        (static_cast<uint16_t>(m_HDRinfo.display_primaries_lsb_y0) |
         (static_cast<uint16_t>(m_HDRinfo.display_primaries_msb_y0) << 8));
    meta->display_primaries[0][1].den = 1;

    // GREEN x
    meta->display_primaries[1][0].num =
        static_cast<int32_t>
        (static_cast<uint16_t>(m_HDRinfo.display_primaries_lsb_x1) |
         (static_cast<uint16_t>(m_HDRinfo.display_primaries_msb_x1) << 8));
    meta->display_primaries[1][0].den = 1;

    // GREEN y
    meta->display_primaries[1][1].num =
        static_cast<int32_t>
        (static_cast<uint16_t>(m_HDRinfo.display_primaries_lsb_y1) |
         (static_cast<uint16_t>(m_HDRinfo.display_primaries_msb_y1) << 8));
    meta->display_primaries[1][1].den = 1;

    // BLUE x
    meta->display_primaries[2][0].num =
        static_cast<int32_t>
        (static_cast<uint16_t>(m_HDRinfo.display_primaries_lsb_x2) |
         (static_cast<uint16_t>(m_HDRinfo.display_primaries_msb_x2) << 8));
    meta->display_primaries[2][0].den = 1;

    // BLUE y
    meta->display_primaries[2][1].num =
        static_cast<int32_t>
        (static_cast<uint16_t>(m_HDRinfo.display_primaries_lsb_y2) |
         (static_cast<uint16_t>(m_HDRinfo.display_primaries_msb_y2) << 8));
    meta->display_primaries[2][1].den = 1;

    // CIE 1931 xy chromaticity coords of white point.
    meta->white_point[0].num  =
        static_cast<int32_t>
        (static_cast<uint16_t>(m_HDRinfo.white_point_lsb_x) |
         (static_cast<uint16_t>(m_HDRinfo.white_point_msb_x) << 8));
    meta->white_point[0].den  = 1;

    meta->white_point[1].num  =
        static_cast<int32_t>
        (static_cast<uint16_t>(m_HDRinfo.white_point_lsb_y) |
         (static_cast<uint16_t>(m_HDRinfo.white_point_msb_y) << 8));
    meta->white_point[1].den  = 1;

    // Luminance
    meta->has_luminance = 1;

    // Max luminance of mastering display (cd/m^2).
    meta->max_luminance.num  =
        static_cast<int32_t>
        (static_cast<uint16_t>(m_HDRinfo.max_display_mastering_lsb_luminance) |
         (static_cast<uint16_t>(m_HDRinfo.max_display_mastering_msb_luminance) << 8));
    meta->max_luminance.num *= 10000;
    meta->max_luminance.den  = 1;

    // Min luminance of mastering display (cd/m^2).
    meta->min_luminance.num  =
        static_cast<int32_t>
        (static_cast<uint16_t>(m_HDRinfo.min_display_mastering_lsb_luminance) |
         (static_cast<uint16_t>(m_HDRinfo.min_display_mastering_msb_luminance) << 8));
    meta->min_luminance.den  = 1;

    /* Light level */
    AVContentLightMetadata* light = av_content_light_metadata_alloc(NULL);

    // Max content light level (cd/m^2).
    light->MaxCLL  =
        static_cast<int32_t>
        (static_cast<uint16_t>(m_HDRinfo.maximum_content_light_level_lsb) |
         (static_cast<uint16_t>(m_HDRinfo.maximum_content_light_level_msb) << 8));

    // Max average light level per frame (cd/m^2).
    light->MaxFALL  =
        static_cast<int32_t>
        (static_cast<uint16_t>(m_HDRinfo.maximum_frame_average_light_level_lsb) |
         (static_cast<uint16_t>(m_HDRinfo.maximum_frame_average_light_level_msb) << 8));

    // Pass metadata to output handler
    m_out2ts->setLight(meta, light);

    return true;
}

/**
 * @brief Update HDR color space information
 *
 * Updates color space parameters based on video signal status and HDR information.
 *
 * @param signal_status Video signal status
 * @return true if color space changed, false otherwise
 */
bool Magewell::update_HDRcolorspace(MWCAP_VIDEO_SIGNAL_STATUS signal_status)
{
    bool result = false;

    // Handle YUV601 color space
    if (signal_status.colorFormat == MWCAP_VIDEO_COLOR_FORMAT_YUV601)
    {
        if (m_verbose > 1)
            m_log->info("Color format: YUV601");
        if (m_out2ts->getColorSpace() != AVCOL_SPC_BT470BG ||
            m_out2ts->getColorPrimaries() != AVCOL_PRI_BT470BG ||
            m_out2ts->getColorTRC() != AVCOL_TRC_SMPTE170M)
        {
            m_out2ts->setColorSpace(AVCOL_SPC_BT470BG);
            m_out2ts->setColorPrimaries(AVCOL_PRI_BT470BG);
            m_out2ts->setColorTRC(AVCOL_TRC_SMPTE170M);
            result = true;
        }
    }
    // Handle YUV2020 color space
    else if (signal_status.colorFormat == MWCAP_VIDEO_COLOR_FORMAT_YUV2020)
    {
        if (m_verbose > 1)
            m_log->info("Color format: YUV2020");
        if (m_out2ts->getColorSpace() != AVCOL_SPC_BT2020_NCL ||
            m_out2ts->getColorPrimaries() != AVCOL_PRI_BT2020)
        {
            m_out2ts->setColorSpace(AVCOL_SPC_BT2020_NCL);
            m_out2ts->setColorPrimaries(AVCOL_PRI_BT2020);
            result = true;
        }
        switch (static_cast<int>(m_HDRinfo.byEOTF))
        {
            case 2: // HDR10
              if (m_out2ts->getColorTRC() != AVCOL_TRC_SMPTE2084)
              {
                  m_out2ts->setColorTRC(AVCOL_TRC_SMPTE2084);
                  result = true;
              }
              break;
            case 3: // HLG
              if (m_out2ts->getColorTRC() != AVCOL_TRC_ARIB_STD_B67)
              {
                  m_out2ts->setColorTRC(AVCOL_TRC_ARIB_STD_B67);
                  result = true;
              }
              break;
            default:
              if (m_out2ts->getColorTRC() != AVCOL_TRC_BT2020_10)
              {
                  m_out2ts->setColorTRC(AVCOL_TRC_BT2020_10);
                  result = true;
              }
              break;
        }
    }
    // Handle YUV709 color space
    else /* (signal_status.colorFormat == MWCAP_VIDEO_COLOR_FORMAT_YUV709) */
    {
        if (m_verbose > 1)
            m_log->info("Color format: YUV709");
        if (m_out2ts->getColorSpace() != AVCOL_SPC_BT709 ||
            m_out2ts->getColorPrimaries() != AVCOL_PRI_BT709 ||
            m_out2ts->getColorTRC() != AVCOL_TRC_BT709)
        {
            m_out2ts->setColorSpace(AVCOL_SPC_BT709);
            m_out2ts->setColorPrimaries(AVCOL_PRI_BT709);
            m_out2ts->setColorTRC(AVCOL_TRC_BT709);
            result = true;
        }
    }

    return result;
}

size_t Magewell::AllocateImageBuffer(void)
{
    m_image_size_qwords = (m_image_size + 7) / 8;
    size_t total_qwords = m_image_size_qwords * m_requested_buffers;

#if 0
    m_log->debug("Allocating Magewell frames: {} /8= {} total8 {} for {}KB",
                 m_image_size, m_image_size_qwords,
                 total_qwords, total_qwords * 8 / 1024);
#endif

    m_image_buffer = std::make_unique<uint64_t[]>(total_qwords);
    if (m_image_buffer == nullptr)
    {
        m_log->critical("Failed to allocate {} for Magewell image buffer",
                        total_qwords * sizeof(uint64_t));
        return 0;
    }

    return total_qwords;
}

uint8_t* Magewell::GetFrameImage(size_t frame_index)
{
    if (frame_index >= m_requested_buffers)
        return nullptr;
    return reinterpret_cast<uint8_t*>
        (m_image_buffer.get() + (frame_index * m_image_size_qwords));
}

/**
 * @brief Handle available image buffer for PRO capture
 *
 * Processes returned image buffer from PRO capture mode.
 *
 * @param pbImage Pointer to image buffer
 * @param buf Context buffer
 */
void Magewell::pro_image_buffer_available(uint8_t* pbImage, void* buf)
{
    unique_lock<mutex> lock(m_image_buffer_mutex);

    m_avail_image_buffers.push_back(pbImage);
    ++m_image_buffers_avail;
    m_image_returned.notify_one();
}

/**
 * @brief Handle available image buffer for ECO capture
 *
 * Processes returned image buffer from ECO capture mode.
 *
 * @param pbImage Pointer to image buffer
 * @param buf Context buffer
 */
void Magewell::eco_image_buffer_available(uint8_t* pbImage, void* buf)
{
    unique_lock<mutex> lock(m_image_buffer_mutex);

    MWCAP_VIDEO_ECO_CAPTURE_FRAME* pEco =
        reinterpret_cast<MWCAP_VIDEO_ECO_CAPTURE_FRAME *>(buf);

    // Re-queue
    if (MW_SUCCEEDED != MWCaptureSetVideoEcoFrame(m_channel, pEco))
    {
        m_log->error("buffer_avail: Failed to Q the Eco frame. avail {}",
                     m_image_buffers_avail);
        delete pEco;
        pEco = nullptr;
    }

    ++m_image_buffers_avail;
    m_image_returned.notify_one();
}

/**
 * @brief Free all image buffers
 *
 * Releases all allocated image buffers and cleans up resources.
 */
void Magewell::free_image_buffers(void)
{
    unique_lock<mutex> lock(m_image_buffer_mutex);

#if 0
    // Wait for all buffers to be returned from output thread
    int idx;
    for (idx = 0;
         idx < 3 && m_image_buffers_total > m_image_buffers_avail;
         ++idx)
    {
        m_log->info("Waiting for Magewell buffers to be returned. "
                    "Total: {} avail: {}", m_image_buffers_total,
                    m_image_buffers_avail);
        if (m_image_returned.wait_for(lock, chrono::seconds(2))
            == cv_status::timeout)
        {
            if (m_running == false)
                break;
            m_log->info("Still waiting for Magewell buffers to be returned. "
                        "Total: {} avail: {}", m_image_buffers_total,
                        m_image_buffers_avail);
        }
    }
    if (idx == 3)
        m_log->warn("Gave up waiting for Magewell buffers to be returned.\n");
#endif

    if (m_isEco)
    {
        if (m_pinned)
        {
            size_t total_qwords = m_image_size_qwords * m_requested_buffers;
            munlock(m_image_buffer.get(), total_qwords * sizeof(uint64_t));
            m_pinned = false;
        }
    }
    else
    {
        if (m_pinned)
        {
            MW_RESULT result;
            uint8_t*  pbImage;

            for (size_t idx = 0; idx < m_requested_buffers; ++idx)
            {
                pbImage = GetFrameImage(idx);
                result = MWUnpinVideoBuffer(m_channel, pbImage);
                switch (result)
                {
                    case MW_SUCCEEDED:
                      break;
                    case MW_FAILED:
                      m_log->warn("Failed to Unpin Magewell frame buffer.");
                      break;
                    case MW_INVALID_PARAMS:
                      m_log->warn("Failed to Unpin Magewell frame buffer. "
                                  "Invalid arguments.");
                      break;
                    case MW_ENODATA:
                      break;
                }
            }
        }
        m_avail_image_buffers.clear();
        m_pinned = false;
    }

    // Reset buffer counters
    m_image_buffers_total = m_image_buffers_avail = 0;
}

/**
 * @brief Add a new ECO image buffer
 *
 * Allocates and initializes a new ECO capture buffer.
 *
 * @return true if successful, false otherwise
 */
bool Magewell::add_eco_image_buffers(void)
{
    MW_RESULT xr;
    uint      idx;

    if (m_eco_image_buffers.empty())
    {
        for (idx = 0; idx < m_requested_buffers; ++idx)
        {
            auto& buf = m_eco_image_buffers.emplace_back
                        (std::make_unique<MWCAP_VIDEO_ECO_CAPTURE_FRAME>());
            buf->deinterlaceMode = MWCAP_VIDEO_DEINTERLACE_BLEND;
            buf->cbFrame  = m_image_size;
            buf->cbStride = m_min_stride;
            buf->bBottomUp = false;
        }
    }

    size_t total_qwords = AllocateImageBuffer();
    if (total_qwords == 0)
        return false;

    if (mlock(m_image_buffer.get(), total_qwords * sizeof(uint64_t)) != 0)
    {
        m_log->warn("Failed to PIN Magewell image buffer memory.");
        m_log->warn("Perhaps update systemd service file with: "
                    "LimitMEMLOCK=infinity");
        m_log->warn("And/or see /etc/security/limits.conf and set "
                    "to at least {}KB for this user.",
                    total_qwords * sizeof(uint64_t) / 1024);

        m_log->warn("Performance may suffer.");
    }
    else
        m_pinned = true;

    if (m_verbose > 3)
    {
        struct rlimit rl;
        if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0)
        {
            // RLIMIT_MEMLOCK corresponds to ulimit -l
            m_log->info("Current soft memory limit (-l): {}",
                        (rl.rlim_cur == RLIM_INFINITY) ?
                        "unlimited" : std::to_string(rl.rlim_cur));
            m_log->info("Current hard memory limit (-l): {}",
                        (rl.rlim_max == RLIM_INFINITY) ?
                        "unlimited" : std::to_string(rl.rlim_max));
        }
        else
        {
            m_log->warn("Failed to check current "
                        "'maximum locked-in-memory size': {}",
                        strerror(errno));
        }
    }

    ecoque_t::iterator Ibuf;
    for (Ibuf = m_eco_image_buffers.begin(), idx = 0;
         Ibuf != m_eco_image_buffers.end(); ++Ibuf, ++idx)
    {
        (*Ibuf)->pvFrame = reinterpret_cast<MWCAP_PTR>(GetFrameImage(idx));
        (*Ibuf)->pvContext = reinterpret_cast<MWCAP_PTR>
                             (std::to_address(*Ibuf));

        // Register buffer with capture system
        if ((xr = MWCaptureSetVideoEcoFrame(m_channel,
                                    std::to_address(*Ibuf))) != MW_SUCCEEDED)
        {
            m_log->critical("MWCaptureSetVideoEcoFrame failed!");
            return false;
        }
    }

    m_image_buffers_avail = m_image_buffers_total = m_requested_buffers;

    return true;
}

/**
 * @brief Add a new PRO image buffer
 *
 * Allocates and initializes a new PRO capture buffer.
 *
 * @return true if successful, false otherwise
 */
bool Magewell::add_pro_image_buffers(void)
{
    if (AllocateImageBuffer() == 0)
        return false;

    MW_RESULT result;
    uint8_t*  pbImage;

    m_pinned = false;
    for (size_t idx = 0; idx < m_requested_buffers; ++idx)
    {
        pbImage = GetFrameImage(idx);
        m_avail_image_buffers.push_back(pbImage);
        result = MWPinVideoBuffer(m_channel, (MWCAP_PTR)pbImage, m_image_size);
        switch (result)
        {
            case MW_SUCCEEDED:
              m_pinned = true;
              break;
            case MW_FAILED:
              m_log->warn("Failed to Pin Magewell frame buffer.");
              break;
            case MW_INVALID_PARAMS:
              m_log->warn("Failed to Pin Magewell frame buffer. "
                             "Invalid arguments.");
              break;
            case MW_ENODATA:
              break;
        }
    }

    m_image_buffers_avail = m_image_buffers_total = m_requested_buffers;

    return true;
}

/**
 * @brief Open ECO video capture
 *
 * Starts ECO video capture with specified parameters.
 *
 * @param eco_params ECO capture parameters
 * @return true if successful, false otherwise
 */
bool Magewell::open_eco_video(MWCAP_VIDEO_ECO_CAPTURE_OPEN & eco_params)
{
    int idx = 0;
    int ret;

    // Retry up to 5 times if needed
    for (idx = 0; idx < 5; ++idx)
    {
        if ((ret = MWStartVideoEcoCapture(m_channel, &eco_params)) ==
            MW_SUCCEEDED)
            break;

        if (m_verbose > 0)
        {
            if (ret == MW_INVALID_PARAMS)
                m_log->error("Start Eco Video Capture error: invalid params");
            else if (ret == MW_FAILED)
                m_log->error("Start Eco Video Capture error: general failure");
            else
                m_log->error("Start Eco Video Capture error: {}", ret);
        }

        this_thread::sleep_for(chrono::milliseconds(100));
    }
    if (idx == 5)
    {
        m_log->critical("Gave up trying to open Eco Capture card.");
        return false;
    }

    if (m_verbose > 1)
    {
        m_log->info("Eco Video capture started.");
    }

    return true;
}

/**
 * @brief Close ECO video capture
 *
 * Stops ECO video capture and frees buffers.
 */
void Magewell::close_eco_video(void)
{
    // Free buffers
    free_image_buffers();
    // Stop capture
    MWStopVideoEcoCapture(m_channel);
}

/**
 * @brief Set up notification for channel events
 *
 * Registers or re-registers a notification with the channel.
 *
 * @param notify Reference to notification handle
 * @param hChannel Channel handle
 * @param hNotifyEvent Event handle
 * @param flags Notification flags
 */
void Magewell::set_notify(HNOTIFY&  notify,
                          HCHANNEL  hChannel,
                          MWCAP_PTR hNotifyEvent,
                          DWORD     flags)
{
    // Unregister existing notification if present
    if (notify)
        MWUnregisterNotify(hChannel, notify);
    // Register new notification
    notify = MWRegisterNotify(hChannel, hNotifyEvent, flags);
}

/**
 * @brief Capture video using ECO capture method
 *
 * Main video capture loop for ECO capture mode.
 *
 * @param eco_params ECO capture parameters
 * @param eco_event ECO event handle
 * @param video_notify Video notification handle
 * @param ullStatusBits Status bits
 * @param interlaced Whether video is interlaced
 *
 * @return true if reset needed
 */
bool Magewell::capture_eco_video(MWCAP_VIDEO_ECO_CAPTURE_OPEN eco_params,
                                 int eco_event,
                                 HNOTIFY video_notify,
                                 ULONGLONG ullStatusBits,
                                 bool interlaced)
{
    uint8_t* pbImage = nullptr;
    int64_t  timestamp;

    float    skipped_frame_cnt = 0;
    float    skipped = 0;
    int      quarter_dur = eco_params.llFrameDuration / 4;

    MWCAP_VIDEO_ECO_CAPTURE_STATUS eco_status;
    MW_RESULT ret;

    int            vidpool_used_1m  {0};
    array<int, 5>  vidpool_used_5m  {0};
    array<int, 10> vidpool_used_10m {0};
    int            vidpool_5m_idx   {0};
    int            vidpool_10m_idx  {0};
    array<int, 5>::iterator  vidpool_5m_max;
    array<int, 10>::iterator vidpool_10m_max;

    int64_t timestamp_adj {0};
    int     short_frame   {-1};

    int used        {0};

    chrono::steady_clock::time_point current_tm;
    chrono::steady_clock::time_point vidpool_tm = chrono::steady_clock::now();
    int duration;

    // Main capture loop
    while (m_running.load() == true)
    {
        // Check if we have enough buffers
        {
            unique_lock<mutex> lock(m_image_buffer_mutex);
            while (m_image_buffers_avail < 2)
            {
                m_image_returned.wait_for(lock,
                                          chrono::milliseconds(1));

                if (m_running.load() == false)
                    return true;
            }

            used = m_image_buffers_total - m_image_buffers_avail;
        }

        // Wait for notification
        if (EcoEventWait(eco_event, m_frame_ms2) <= 0)
        {
            if (m_verbose > 1)
                m_log->info("Waiting for video data (frame {})",
                            m_frame_cnt);
            continue;
        }

        // Get notification status
        if (MW_SUCCEEDED != MWGetNotifyStatus(m_channel, video_notify,
                                              &ullStatusBits))
        {
            if (m_verbose > 0)
            {
                m_log->warn("Failed to get Notify status (frame {})",
                            m_frame_cnt);
            }
            continue;
        }

        // Handle reset
        if (m_reset_video.load() == true)
        {
            if (m_verbose > 1)
                m_log->info("Video reset.");
            return true;
        }

        // Handle signal change
        if (ullStatusBits & MWCAP_NOTIFY_VIDEO_SIGNAL_CHANGE)
        {
            m_log->warn("DAMAGED: Magewell lost video sync.");
            return false;
        }

        // Get capture status
        ret = MWGetVideoEcoCaptureStatus(m_channel, &eco_status);
        if (MW_SUCCEEDED != ret
            || eco_status.pvFrame == reinterpret_cast<MWCAP_PTR>(nullptr))
        {
            this_thread::sleep_for(chrono::milliseconds(1));
            continue;
        }

        // Process frame
        pbImage = reinterpret_cast<uint8_t *>(eco_status.pvFrame);
        timestamp = eco_status.llTimestamp;
        ++m_frame_cnt;
        --m_image_buffers_avail;

        if (m_expected_ts == -1 && timestamp < 0)
        {
            continue;
        }
        else if (m_expected_ts > 0 &&
                 (timestamp < m_expected_ts - quarter_dur ||
                  m_expected_ts + quarter_dur < timestamp))
        {
            if (timestamp < 0)
            {
                if (m_verbose > 0)
                    m_log->warn("Invalid ECO timestamp: {}", timestamp);
                timestamp = m_expected_ts;
            }
            else if (timestamp < m_expected_ts)
            {
                float frames = static_cast<float>(m_expected_ts - timestamp)
                               / static_cast<float>(eco_params.llFrameDuration);
                if (m_verbose > 3)
                    m_log->info("Timestamp is {:.0f} frame less than "
                                "expected.    ({}) [Adjusting]",
                                frames, m_frame_cnt);
                short_frame = m_frame_cnt;
                timestamp_adj = eco_params.llFrameDuration;
            }
            else // timestamp > m_expected_ts
            {
                /* There can be some WACKY TS from the frame info!
                 */
                if (timestamp <
                    (m_expected_ts + (eco_params.llFrameDuration * 32)))
                {
                    skipped = static_cast<float>(timestamp - m_expected_ts)
                              / static_cast<float>(eco_params.llFrameDuration);
                    if (skipped > 0.5)
                    {
                        if (short_frame > 0 && skipped < 2)
                        {
                            if (m_verbose > 3)
                                m_log->info("Timestamp is {:.0f} frame greater "
                                            "than expected. ({}) "
                                            "[Adjustment cleared]",
                                            skipped, m_frame_cnt);
                            short_frame = -1;
                            timestamp_adj = 0;
                        }
                        else
                        {
                            skipped_frame_cnt += skipped;
                            if (skipped_frame_cnt > 1 && m_frame_cnt > 60)
                                m_log->warn("DAMAGED: Magewell lost {:.0f} "
                                            "frames, have skipped {:.0f} : {}",
                                            skipped, skipped_frame_cnt,
                                            m_frame_cnt);
                            else
                                m_log->warn("Magewell lost {:.0f} "
                                            "frames, have skipped {:.0f} : {}",
                                            skipped, skipped_frame_cnt,
                                            m_frame_cnt);

#if 0
                            if (skipped > 10)
                                Reset();
#endif
                        }
                    }
                    else
                        m_log->debug("Timestamp {} > {} Expected, but by only "
                                     "{} frames.", timestamp, m_expected_ts,
                                     skipped);
                }
                else
                    m_log->warn("timestamp {} >>>> {} expected.",
                                timestamp, m_expected_ts);
            }
        }
        m_expected_ts = timestamp + eco_params.llFrameDuration;

        // Add frame to output handler
        m_out2ts->AddVideoFrame(pbImage,
                        reinterpret_cast<MWCAP_VIDEO_ECO_CAPTURE_FRAME *>
                                (eco_status.pvContext),
                                m_num_pixels, timestamp + timestamp_adj);

        if (m_verbose > 1)
        {
            if (vidpool_used_1m < used)
                vidpool_used_1m = used;
            if (vidpool_used_5m[vidpool_5m_idx] < used)
                vidpool_used_5m[vidpool_5m_idx] = used;
            if (vidpool_used_10m[vidpool_10m_idx] < used)
                vidpool_used_10m[vidpool_10m_idx] = used;
            current_tm = chrono::steady_clock::now();
            duration = chrono::duration_cast<chrono::seconds>
                       (current_tm - vidpool_tm).count();

            if (duration >= 60)
            {
                vidpool_5m_max  = ranges::max_element(vidpool_used_5m);
                vidpool_10m_max = ranges::max_element(vidpool_used_10m);

                uint temperature;
                MWGetTemperature(m_channel, &temperature);
                m_log->info("Magewell frame pool used 1m:{:<3d} "
                            "5m:{:<3d} 10m:{:<3d} of {:<3d} "
                            "(Temp {:.1f}ºC)",
                            vidpool_used_1m, *vidpool_5m_max,
                            *vidpool_10m_max, m_image_buffers_total,
                            static_cast<float>(temperature) / 10);

                vidpool_used_1m = 0;

                ++vidpool_5m_idx;
                vidpool_5m_idx %= 5;
                vidpool_used_5m[vidpool_5m_idx] = 0;

                ++vidpool_10m_idx;
                vidpool_10m_idx %= 10;
                vidpool_used_10m[vidpool_10m_idx] = 0;

                vidpool_tm = current_tm;
            }
        }
    }

    return true;
}

/**
 * @brief Capture video using PRO capture method
 *
 * Main video capture loop for PRO capture mode.
 *
 * @param eco_params ECO capture parameters
 * @param video_notify Video notification handle
 * @param notify_event Notification event handle
 * @param capture_event Capture event handle
 * @param frame_wrap_idx Frame wrap index
 * @param event_mask Event mask
 * @param ullStatusBits Status bits
 * @param interlaced Whether video is interlaced
 *
 * @return true if reset needed
 */
bool Magewell::capture_pro_video(MWCAP_VIDEO_ECO_CAPTURE_OPEN eco_params,
                                 HNOTIFY video_notify,
                                 MWCAP_PTR notify_event,
                                 MWCAP_PTR capture_event,
                                 int frame_wrap_idx,
                                 DWORD event_mask,
                                 ULONGLONG ullStatusBits,
                                 bool interlaced)
{
    int frame_idx  = -1;

    uint8_t* pbImage = nullptr;
    int64_t  timestamp = -1;
    int64_t  desired_ts  = -1;
    int64_t  min_ts = -1;
    int      min_idx = -1;
    int      skipped_frame_cnt = 0;
    int      skipped = 0;

    int      eighth_dur = eco_params.llFrameDuration / 8;

    MWCAP_VIDEO_BUFFER_INFO   videoBufferInfo;
    MWCAP_VIDEO_FRAME_INFO    videoFrameInfo;
    MWCAP_VIDEO_SIGNAL_STATUS videoSignalStatus;
    MW_RESULT result;

    int            vidpool_used_1m  {0};
    array<int, 5>  vidpool_used_5m  {0};
    array<int, 10> vidpool_used_10m {0};
    int            vidpool_5m_idx   {0};
    int            vidpool_10m_idx  {0};
    array<int, 5>::iterator  vidpool_5m_max;
    array<int, 10>::iterator vidpool_10m_max;

    int used = 0;

    chrono::steady_clock::time_point current_tm;
    chrono::steady_clock::time_point vidpool_tm = chrono::steady_clock::now();
    int duration;

    // Main capture loop
    while (m_running.load() == true)
    {
        // Wait for notification
        if (MWWaitEvent(notify_event, m_frame_ms2) <= 0)
        {
            if (m_verbose > 1)
                m_log->info("Waiting for video data (frame {})",
                            m_frame_cnt);
            continue;
        }

        // Get notification status
        if (MW_SUCCEEDED != MWGetNotifyStatus(m_channel, video_notify,
                                              &ullStatusBits))
        {
            if (m_verbose > 0)
            {
                m_log->warn("Failed to get Notify status (frame {})",
                            m_frame_cnt);
            }
            continue;
        }

        // Handle reset
        if (m_reset_video.load() == true)
        {
            if (m_verbose > 1)
                m_log->info("Video reset.");
            return true;
        }

        // Handle signal change
        if (ullStatusBits & MWCAP_NOTIFY_VIDEO_SIGNAL_CHANGE)
        {
            m_log->warn("DAMAGED: Magewell lost video sync.");
            return false;
        }

        // Check signal lock status
        MWGetVideoSignalStatus(m_channel, &videoSignalStatus);
        if (videoSignalStatus.state != MWCAP_VIDEO_SIGNAL_LOCKED)
        {
            if (m_verbose > 0)
            {
                m_log->warn("DAMAGED: Video signal lost lock. (frame {})",
                            m_frame_cnt);
            }
            this_thread::sleep_for(chrono::milliseconds(5));
            return false;
        }

        // Check event mask
        if ((ullStatusBits & event_mask) == 0)
            continue;

        // Get buffer info
        if (MW_SUCCEEDED != MWGetVideoBufferInfo(m_channel,
                                                 &videoBufferInfo))
        {
            if (m_verbose > 0)
            {
                m_log->warn("Failed to get video buffer info (frame {})",
                            m_frame_cnt);
            }
            continue;
        }

        // Manage frame index
        if (frame_idx == -1)
        {
            frame_idx = videoBufferInfo.iNewestBufferedFullFrame;
        }
        else
        {
            if (++frame_idx == frame_wrap_idx)
                frame_idx = 0;
        }

        // Get frame info
        if (MWGetVideoFrameInfo(m_channel, frame_idx,
                                &videoFrameInfo) != MW_SUCCEEDED)
        {
            if (m_verbose > 0)
            {
                m_log->warn("Failed to get video frame info (frame {})",
                            m_frame_cnt);
            }
            continue;
        }

        timestamp = videoFrameInfo.allFieldBufferedTimes[0];
        if (m_expected_ts >= 0 &&
            (timestamp == -1 ||
             (timestamp < m_expected_ts - eighth_dur ||
              m_expected_ts + eighth_dur < timestamp)))
        {
            bool found = false;

            if (timestamp == -1)
            {
                min_idx = -1;
                min_ts  = numeric_limits<std::int64_t>::max();
            }
            else
            {
                min_idx = frame_idx;
                min_ts  = timestamp;
            }
            desired_ts = m_expected_ts - eighth_dur;

            // Find the earliest, valid TS
            for (int i = 0; i < frame_wrap_idx; ++i)
            {
                if (i == frame_idx)
                    continue;

                // Get frame info
                if (MWGetVideoFrameInfo(m_channel, i,
                                        &videoFrameInfo) != MW_SUCCEEDED)
                {
                    if (m_verbose > 0)
                    {
                        m_log->warn("Failed to get video frame info (frame {})",
                                    i);
                    }
                    continue;
                }
                if (videoFrameInfo.allFieldBufferedTimes[0] > desired_ts &&
                    videoFrameInfo.allFieldBufferedTimes[0] < min_ts)
                {
                    min_ts = videoFrameInfo.allFieldBufferedTimes[0];
                    min_idx = i;
                    found = true;
                }
            }
            if (!found)
            {
                m_log->warn("None of the MW card buffers are valid.");
                break;
            }

            skipped = (min_ts - m_expected_ts) / eco_params.llFrameDuration;
            if (skipped > 0)
            {
                skipped_frame_cnt += skipped;
                m_log->warn("DAMAGED: Magewell lost {} frames. "
                            "Have skipped {} : {}",
                            skipped, skipped_frame_cnt, m_frame_cnt);
            }

            frame_idx = min_idx;
            timestamp = min_ts;
        }
        m_expected_ts = timestamp + eco_params.llFrameDuration;

        // Get available buffer
        {
            unique_lock<mutex> lock(m_image_buffer_mutex);
            while (m_avail_image_buffers.empty())
            {
                m_image_returned.wait_for(lock,
                                          chrono::milliseconds(4));
                if (m_running.load() == false)
                    return true;
            }
            pbImage = m_avail_image_buffers.front();
            m_avail_image_buffers.pop_front();

            used = m_image_buffers_total - m_avail_image_buffers.size();
        }

        // Capture frame to virtual address
        result = MWCaptureVideoFrameToVirtualAddress
                 (m_channel,
                  frame_idx,
                  reinterpret_cast<MWCAP_PTR>(pbImage),
                  m_image_size,
                  m_min_stride,
                  0,
                  0,
                  eco_params.dwFOURCC,
                  eco_params.cx,
                  eco_params.cy);

        ++m_frame_cnt;
        --m_image_buffers_avail;

        if (result != MW_SUCCEEDED)
        {
            if (m_verbose > 0)
            {
                m_log->warn("Damaged: Failed to retrieve next frame "
                            "[{}] (processed {})", frame_idx, m_frame_cnt);
            }
            pro_image_buffer_available(pbImage, nullptr);
            continue;
        }

        // Wait for capture completion
        if (MWWaitEvent(capture_event, m_frame_ms2) <= 0)
        {
            if (m_verbose > 0)
            {
                m_log->warn("wait capture event error or timeout "
                            "(frame {})", m_frame_cnt);
            }
            pro_image_buffer_available(pbImage, nullptr);
            continue;
        }

        // Get capture status
        MWCAP_VIDEO_CAPTURE_STATUS captureStatus;
        MWGetVideoCaptureStatus(m_channel, &captureStatus);

        // Add frame to output handler
        m_out2ts->AddVideoFrame(pbImage, nullptr,
                                m_num_pixels, timestamp);

        if (m_verbose > 1)
        {
            if (vidpool_used_1m < used)
                vidpool_used_1m = used;
            if (vidpool_used_5m[vidpool_5m_idx] < used)
                vidpool_used_5m[vidpool_5m_idx] = used;
            if (vidpool_used_10m[vidpool_10m_idx] < used)
                vidpool_used_10m[vidpool_10m_idx] = used;

            current_tm = chrono::steady_clock::now();
            duration = chrono::duration_cast<chrono::seconds>
                       (current_tm - vidpool_tm).count();

            if (duration >= 60)
            {
                vidpool_5m_max  = ranges::max_element(vidpool_used_5m);
                vidpool_10m_max = ranges::max_element(vidpool_used_10m);

                uint temperature;
                MWGetTemperature(m_channel, &temperature);
                m_log->info("Magewell frame pool used 1m:{:<3d} "
                            "5m:{:<3d} 10m:{:<3d} of {:<3d} "
                            "(Temp {:.1f}ºC)",
                            vidpool_used_1m, *vidpool_5m_max,
                            *vidpool_10m_max, m_image_buffers_total,
                            static_cast<float>(temperature) / 10);

                vidpool_used_1m = 0;

                ++vidpool_5m_idx;
                vidpool_5m_idx %= 5;
                vidpool_used_5m[vidpool_5m_idx] = 0;

                ++vidpool_10m_idx;
                vidpool_10m_idx %= 10;
                vidpool_used_10m[vidpool_10m_idx] = 0;

                vidpool_tm = current_tm;
            }
        }
    }

    return true;
}

/**
 * @brief Main video capture loop
 *
 * Main loop that handles video capture with automatic parameter detection and buffer management.
 *
 * @return true always
 */
bool Magewell::capture_video(int quality)
{
    // Eco
    int       eco_event     = -1;
    HNOTIFY   video_notify  {0};
    DWORD     event_mask    {0};

    // Pro
    MWCAP_PTR notify_event  = 0/*nullptr*/;
    MWCAP_PTR capture_event = 0/*nullptr*/;

    MWCAP_VIDEO_ECO_CAPTURE_OPEN eco_params {0};
    MWCAP_VIDEO_BUFFER_INFO      videoBufferInfo;

    bool     interlaced = false;
    bool     params_changed = false;
    bool     color_changed = false;
    bool     locked = false;
    DWORD    state = 0;

    int      frame_wrap_idx = 4;

    int       bpp = 0;
    ULONGLONG ullStatusBits = 0;
    bool      rejected = false;

#if 0
    DWORD event_mask = MWCAP_NOTIFY_VIDEO_SAMPLING_PHASE_CHANGE |
                       MWCAP_NOTIFY_VIDEO_SMPTE_TIME_CODE |
                       MWCAP_NOTIFY_VIDEO_SIGNAL_CHANGE |
                       MWCAP_NOTIFY_HDMI_INFOFRAME_HDR;
#endif

    if (m_verbose > 0)
        m_log->info("Video capture starting.");

    if (m_isEco)
    {
        eco_event = eventfd(0, EFD_NONBLOCK);
        if (eco_event < 0)
        {
            m_log->critical("Unable to create event fd for eco capture.");
            Shutdown();
        }
    }
    else
    {
        capture_event = MWCreateEvent();
        if (capture_event == 0)
        {
            m_log->critical("Create timer event error");
            Shutdown();

        }

        notify_event = MWCreateEvent();
        if (notify_event == 0)
        {
            m_log->critical("Create notify event error");
            Shutdown();
        }

        if (MW_SUCCEEDED != MWStartVideoCapture(m_channel, capture_event))
        {
            m_log->critical("Start Pro Video Capture error!");
            Shutdown();
        }
    }

    while (m_running.load() == true)
    {
        MWCAP_VIDEO_SIGNAL_STATUS videoSignalStatus;

        MWGetVideoSignalStatus(m_channel, &videoSignalStatus);

        if (videoSignalStatus.state == MWCAP_VIDEO_SIGNAL_UNSUPPORTED)
        {
            if (state != videoSignalStatus.state && m_verbose > 0)
                m_log->warn("Input video signal status: Unsupported");
            locked = false;
            state = videoSignalStatus.state;
            this_thread::sleep_for(chrono::milliseconds(m_frame_ms * 3));
            continue;
        }

        switch (videoSignalStatus.state)
        {
            case MWCAP_VIDEO_SIGNAL_LOCKED:
              if (!locked && m_verbose > 1)
                  m_log->info("Input video signal status: Locked");
              locked = true;
              break;
            case MWCAP_VIDEO_SIGNAL_NONE:
              if (state != videoSignalStatus.state && m_verbose > 0)
                  m_log->warn("Input video signal status: NONE");
              locked = false;
              state = videoSignalStatus.state;
              this_thread::sleep_for(chrono::milliseconds(m_frame_ms * 5));
              continue;
            case MWCAP_VIDEO_SIGNAL_LOCKING:
              if (state != videoSignalStatus.state && m_verbose > 0)
                  m_log->warn("Input video signal status: Locking");
              locked = false;
              state = videoSignalStatus.state;
              this_thread::sleep_for(chrono::milliseconds(m_frame_ms * 5));
              continue;
            default:
              if (m_verbose > 0)
                  m_log->warn("Video signal status: lost locked.");
              locked = false;
              this_thread::sleep_for(chrono::milliseconds(m_frame_ms * 5));
              continue;
        }

        if (videoSignalStatus.bInterlaced)
        {
            if (!rejected && m_verbose > 0)
                m_log->info("REJECTING interlaced video.");
            rejected = true;
            continue;
        }
        rejected = false;

        if (update_HDRinfo())
        {
            color_changed = update_HDRcolorspace(videoSignalStatus);
            if (static_cast<int>(m_HDRinfo.byEOTF) == 3 ||
                static_cast<int>(m_HDRinfo.byEOTF) == 2)
            {
                eco_params.dwFOURCC = MWFOURCC_P010;
            }
            m_isHDR = true;
        }
        else /* if (m_out2ts->isHDR()) */
        {
            color_changed = update_HDRcolorspace(videoSignalStatus);

            if (m_p010)
                eco_params.dwFOURCC = MWFOURCC_P010;
            else if (m_out2ts->encoderType() == OutputTS::QSV ||
                     m_out2ts->encoderType() == OutputTS::VAAPI)
                eco_params.dwFOURCC = MWFOURCC_NV12;
            else if (m_out2ts->encoderType() == OutputTS::NV)
                eco_params.dwFOURCC = MWFOURCC_I420;
            else
            {
                m_log->critical("Failed to determine best magewell pixel format.");
                Shutdown();
            }

            m_isHDR = false;
        }

        if (eco_params.cx != videoSignalStatus.cx)
        {
            if (m_verbose > 1)
                m_log->info("Width: {} -> {}", eco_params.cx, videoSignalStatus.cx);
            eco_params.cx = videoSignalStatus.cx;
            params_changed = true;
        }
        if (eco_params.cy != videoSignalStatus.cy)
        {
            if (m_verbose > 1)
                m_log->info("Height: {} -> {}", eco_params.cy, videoSignalStatus.cy);
            eco_params.cy = videoSignalStatus.cy;
            params_changed = true;
        }
        m_min_stride = FOURCC_CalcMinStride(eco_params.dwFOURCC,
                                            eco_params.cx, 4);
        m_image_size = FOURCC_CalcImageSize(eco_params.dwFOURCC,
                                            eco_params.cx,
                                            eco_params.cy,
                                            m_min_stride); /* * 3 / 2; */
        if (m_num_pixels != m_min_stride * eco_params.cy)
        {
            if (m_verbose > 1)
                m_log->info("Num pixels: {} -> {}", m_num_pixels,
                            m_min_stride * eco_params.cy);
            m_num_pixels = m_min_stride * eco_params.cy;
            params_changed = true;
        }
        if (eco_params.llFrameDuration != videoSignalStatus.dwFrameDuration)
        {
            if (m_verbose > 1)
                m_log->info("Duration: {} -> {}", eco_params.llFrameDuration,
                            videoSignalStatus.dwFrameDuration);
            eco_params.llFrameDuration = videoSignalStatus.dwFrameDuration;
            params_changed = true;
        }
        if (interlaced != static_cast<bool>(videoSignalStatus.bInterlaced))
        {
            if (m_verbose > 1)
                m_log->info("Interlaced: {} -> {}", interlaced ? "Y" : "N",
                            videoSignalStatus.bInterlaced ? "Y" : "N");
            interlaced = static_cast<bool>(videoSignalStatus.bInterlaced);
            params_changed = true;
        }
        if (bpp != FOURCC_GetBpp(eco_params.dwFOURCC))
        {
            if (m_verbose > 1)
                m_log->info("Video Bpp: {} -> {}", bpp,
                            FOURCC_GetBpp(eco_params.dwFOURCC));
            bpp = FOURCC_GetBpp(eco_params.dwFOURCC);
            params_changed = true;
        }

        if (m_verbose > 0)
            m_log->info("Using {} RAM frame buffers.", m_requested_buffers);

        if (params_changed || color_changed)
        {
            color_changed = false;
            params_changed = false;

            if (m_verbose > 1 /* && m_frame_cnt > 0 */)
                m_log->info("Video signal CHANGED.");

            m_frame_ms = eco_params.llFrameDuration / 10000;
            m_frame_ms2 = m_frame_ms * 2;

            AVRational frame_rate, time_base;
            if (interlaced)
            {
                frame_rate = (AVRational){20000000LL,
                    (int)eco_params.llFrameDuration};
                time_base = (AVRational){1, 20000000LL};
            }
            else
            {
                frame_rate = (AVRational){10000000LL,
                    (int)eco_params.llFrameDuration};
                time_base = (AVRational){1, 10000000LL};
            }

            // 100ns / frame duration
            if (m_verbose > 1)
            {
                double fps = (interlaced) ?
                             (double)20000000LL / eco_params.llFrameDuration :
                             (double)10000000LL / eco_params.llFrameDuration;

                m_log->debug("========");
                m_log->info("Input signal: {}x{}{} {:.2f} "
                            "{}/{}, Time base: {}/{}, Frame segmented: {}",
                            eco_params.cx, eco_params.cy,
                            interlaced ? 'i' : 'p', fps,
                            frame_rate.num, frame_rate.den,
                            time_base.num, time_base.den,
                            videoSignalStatus.bSegmentedFrame ? "Yes" : "No");
                m_log->debug("========");
            }

            if (MWGetVideoBufferInfo(m_channel,
                                     &videoBufferInfo) != MW_SUCCEEDED)
                continue;

            frame_wrap_idx = videoBufferInfo.cMaxFrames;

            event_mask = MWCAP_NOTIFY_VIDEO_SIGNAL_CHANGE;
            if(interlaced)
            {
                event_mask |= MWCAP_NOTIFY_VIDEO_FIELD_BUFFERED;
#if 0
                if (0 == videoBufferInfo.iBufferedFieldIndex)
                    mode = MWCAP_VIDEO_DEINTERLACE_TOP_FIELD;
                else
                    mode = MWCAP_VIDEO_DEINTERLACE_BOTTOM_FIELD;
#endif
            }
            else
            {
                event_mask |= MWCAP_NOTIFY_VIDEO_FRAME_BUFFERED;
#if 0
                mode = MWCAP_VIDEO_DEINTERLACE_BLEND;
#endif
            }

            m_out2ts->setVideoParams(eco_params.cx, eco_params.cy, interlaced,
                                     time_base, eco_params.llFrameDuration,
                                     frame_rate, m_isHDR);

            if (m_isEco)
            {
                close_eco_video();

                eco_params.hEvent = eco_event;
                if (!open_eco_video(eco_params))
                    Shutdown();
                else
                {
                    if (!add_eco_image_buffers())
                    {
                        Shutdown();
                        break;
                    }
                }
            }
            else
            {
                free_image_buffers();
                if (!add_pro_image_buffers())
                {
                    Shutdown();
                    break;
                }
            }

#if 0
            int audio_buf_sz = eco_params.llFrameDuration >> 8;
            if (eco_params.cx > 1920)
                audio_buf_sz = audio_buf_sz * 2;
            if (m_audio_buf_frames < audio_buf_sz)
            {
                m_audio_buf_frames = audio_buf_sz;
                m_reset_audio.store(true);
            }
#endif
        }
#if 0
        else
        {
            m_log->info(" No changed to input");
        }
#endif

        if (video_notify)
            MWUnregisterNotify(m_channel, video_notify);
        if (m_isEco)
            video_notify = MWRegisterNotify(m_channel, eco_event, event_mask);
        else
            video_notify = MWRegisterNotify(m_channel, notify_event, event_mask);
        if (!video_notify)
        {
            m_log->critical("Video: Failed to register notify event.");
            Shutdown();
        }

#if 0
        if (m_reset_audio.load())
        {
            this_thread::sleep_for(chrono::milliseconds(m_frame_ms));
            continue;
        }
#endif

        m_reset_video.store(false);
        if (m_isEco)
            params_changed = capture_eco_video(eco_params, eco_event, video_notify,
                                               ullStatusBits, interlaced);
        else
            params_changed = capture_pro_video(eco_params, video_notify,
                                               notify_event, capture_event,
                                               frame_wrap_idx, event_mask,
                                               ullStatusBits, interlaced);
    }

    if (m_isEco)
    {
        close_eco_video();
        if (eco_event)
        {
            eventfd_write(eco_event, 1);
            close(eco_event);
        }
    }
    else
    {
        free_image_buffers();
        MWStopVideoCapture(m_channel);
        if (video_notify)
            MWUnregisterNotify(m_channel, video_notify);

        if (reinterpret_cast<void*>(capture_event) != nullptr)
        {
            MWCloseEvent(capture_event);
            capture_event = 0 /*nullptr*/;
        }

        if (reinterpret_cast<void*>(notify_event) != nullptr)
        {
            MWCloseEvent(notify_event);
            notify_event = 0 /*nullptr*/;
        }
    }

    if (m_verbose > 2)
        m_log->info("Video Capture finished.");
    Shutdown();

    return true;
}

/**
 * @brief Start capture process
 *
 * Starts the video and audio capture process with specified parameters.
 *
 * @param video_codec Video codec to use
 * @param preset Encoding preset
 * @param quality Quality setting
 * @param look_ahead Look ahead setting
 * @param no_audio Whether to disable audio capture
 * @param p010 Whether to use P010 format
 * @param gpu_device GPU device to use
 * @return true if successful, false otherwise
 */
bool Magewell::Capture(const string & video_codec,
                       const string & preset, int quality,
                       int look_ahead, bool no_audio,
                       bool p010, const string & gpu_device, float gop_secs,
                       int extra_hw_frames, int gpu_buffers, int video_buffers)
{
    m_p010 = p010;
    m_requested_buffers = video_buffers;

    // Display input information if verbose
    if (m_verbose > 1)
        describe_input(m_channel);

    // Create output handler based on capture mode
    if (m_isEco)
    {
        m_out2ts = new OutputTS(m_verbose, video_codec, preset, quality,
                                look_ahead, p010, true, gpu_device,
                                extra_hw_frames, gpu_buffers, gop_secs,
                                [=,this](void) { this->Shutdown(); },
                                [=,this](void) { this->Reset(); },
                                [=,this](uint8_t* ib, void* eb)
                                { this->eco_image_buffer_available(ib, eb); });
    }
    else
    {
        m_out2ts = new OutputTS(m_verbose, video_codec, preset, quality,
                                look_ahead, p010, false, gpu_device,
                                extra_hw_frames, gpu_buffers, gop_secs,
                                [=,this](void) { this->Shutdown(); },
                                [=,this](void) { this->Reset(); },
                                [=,this](uint8_t* ib, void* eb)
                                { this->pro_image_buffer_available(ib, eb); });
    }

    // Check if output handler was created successfully
    if (!m_out2ts)
    {
        m_log->critical("Failed to create OutputTS muxing.");
        Shutdown();
        delete m_out2ts;
        return false;
    }

    // Start audio thread if audio is enabled
    if (!no_audio)
    {
        m_audio_thread = thread(&Magewell::capture_audio, this);
        pthread_setname_np(m_audio_thread.native_handle(),
                           "capture_audio");
        this_thread::sleep_for(chrono::milliseconds(1));
    }

    // Start video capture
    capture_video(quality);

    // Join audio thread if it was started
    if (!no_audio)
        m_audio_thread.join();

    // Clean up output handler
    delete m_out2ts;
    m_out2ts = nullptr;

    return true;
}

/**
 * @brief Shutdown capture process
 *
 * Stops all capture processes and cleans up resources.
 */
void Magewell::Shutdown(void)
{
    // Only shutdown if running
    if (m_running.exchange(false))
    {
        if (m_verbose > 2)
            m_log->info("Magewell::Shutdown");
        m_out2ts->Shutdown();
        m_reset_audio.store(true);
    }
}

/**
 * @brief Reset capture process
 *
 * Resets capture process with rate limiting to prevent excessive resets.
 */
void Magewell::Reset(void)
{
    chrono::steady_clock::time_point end = chrono::steady_clock::now();

    // Rate limit resets to prevent excessive resets
    if (chrono::duration_cast<chrono::seconds>(end - m_last_reset).count() > 4)
    {
        if (m_verbose > 0)
            m_log->info("Magewell:Reset");
        m_reset_audio.store(true);
        m_reset_video.store(true);
        m_last_reset = std::chrono::steady_clock::now();
    }
#if 0
    m_expected_ts = -1;
#endif
}
