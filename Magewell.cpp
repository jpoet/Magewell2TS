/*
 * magewell2ts
 * Copyright (c) 2022-2025 John Patrick Poet
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

#include <MWFOURCC.h>
#include <LibMWCapture/MWCapture.h>
#include "LibMWCapture/MWEcoCapture.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include "lock_ios.h"
#include "Magewell.h"

//#define DUMP_RAW_AUDIO_ALLBITS
//#define DUMP_RAW_AUDIO

#if defined(DUMP_RAW_AUDIO) || defined(DUMP_RAW_AUDIO_ALLBITS)
#include <fstream>
#endif

using namespace std;
using namespace s6_lock_ios;

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
    // Initialize the MWCapture SDK instance
    if (!MWCaptureInitInstance())
    {
        // If initialization fails, output error and set fatal flag
        cerr << lock_ios() << "ERROR: Failed to inialize MWCapture.\n";
        m_fatal = true;
    }
    // Initialize last reset time
    m_last_reset = std::chrono::high_resolution_clock::now();
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
bool Magewell::describe_input(HCHANNEL hChannel)
{
    MW_RESULT xr;
    MWCAP_VIDEO_SIGNAL_STATUS vStatus;
    MWCAP_INPUT_SPECIFIC_STATUS status;

    // Get input specific status
    xr = MWGetInputSpecificStatus(hChannel, &status);

    // Lock cerr for thread-safe output
    ios_lock lock;
    cerr << lock_ios(lock);

    // Check if we got valid status
    if (xr != MW_SUCCEEDED ||
        MWGetVideoSignalStatus(hChannel, &vStatus) != MW_SUCCEEDED)
    {
        cerr << "Failed to get video signal status.\n";
        return false;
    }

    // Check if there's a valid signal
    if (!status.bValid)
    {
        cerr << "No signal detected on input.\n";
        return false;
    }

    // Output basic video signal information
    cerr << "Video Signal " << GetVideoSignal(vStatus.state);
    cerr << ": " << GetVideoInputType(status.dwVideoInputType);

    // Output HDMI-specific information
    if (status.dwVideoInputType == MWCAP_VIDEO_INPUT_TYPE_HDMI)
    {
        cerr << ", HDCP: " << (status.hdmiStatus.bHDCP ? "Yes" : "No")
             << ", Mode: "
             << static_cast<int>(status.hdmiStatus.bHDMIMode)
             << ", Bit Depth: "
             << static_cast<int>(status.hdmiStatus.byBitDepth);
    }
    // Output SDI-specific information
    else if (status.dwVideoInputType == MWCAP_VIDEO_INPUT_TYPE_SDI)
    {
        cerr << ", Type: " << GetVideoSDIType(status.sdiStatus.sdiType)
             << ", Scan Fmt: "
             << GetVideoScanFmt(status.sdiStatus.sdiScanningFormat)
             << ", Bit depth: "
             << GetVideoBitDepth(status.sdiStatus.sdiBitDepth)
             << ", Sampling: "
             << GetVideoSamplingStruct(status.sdiStatus.sdiSamplingStruct);
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

        cerr << ", ScanType: "
             << GetVideoSyncType(status.vgaComponentStatus.syncInfo.bySyncType)
             << ", bHSPolarity: "
             << status.vgaComponentStatus.syncInfo.bHSPolarity
             << ", bVSPolarity: "
             << status.vgaComponentStatus.syncInfo.bVSPolarity
             << ", Interlaced: "
             << status.vgaComponentStatus.syncInfo.bInterlaced
             << ", FrameDuration: " << dFrameDuration;
    }
    // Output CVBS-specific information
    else if (status.dwVideoInputType == MWCAP_VIDEO_INPUT_TYPE_CVBS) {
        cerr << ", Standard: "
             << GetVideoSDStandard(status.cvbsYcStatus.standard)
             << ", b50Hz: " << status.cvbsYcStatus.b50Hz;
    }
    cerr << " " << GetVideoColorName(vStatus.colorFormat) << "\n";

    // Calculate frame duration
    double dFrameDuration = (vStatus.bInterlaced == TRUE)
                            ? (double)20000000
                            / vStatus.dwFrameDuration
                            : (double)10000000
                            / vStatus.dwFrameDuration;
    dFrameDuration = static_cast<int>(dFrameDuration * 100)
                     / 100.0;

    // Output resolution and timing information
    cerr << "    " << vStatus.cx << "x" << vStatus.cy
         << (vStatus.bInterlaced ? "i" : "p")
         << dFrameDuration;

    cerr << " [x:" << vStatus.x
         << ", y:" << vStatus.y << "] "
         << "total (" << vStatus.cxTotal << "x"
         << vStatus.cyTotal << ") "
         << "aspect " << vStatus.nAspectX << ":"
         << vStatus.nAspectY << ",\n";

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
            return false;
        }

        // Output audio channel information
        cerr << ", Channels:";
        for (int i = 0; i < 4; ++i)
        {
            if (aStatus.wChannelValid & (0x01 << i))
                cerr << " " << (i * 2 + 1) << "&" << (i * 2 + 2);
        }
        cerr << ", " << (aStatus.bLPCM ? "lPCM" : "Bitstream")
             << ", "
             << "BPSample: " << static_cast<int>(aStatus.cBitsPerSample)
             << ", Sample Rate: "
             << aStatus.dwSampleRate
             << "\n";
    }

    return true;
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
    cerr << lock_ios() << num_channels << " channels.\n";

    // Iterate through all channels
    for (idx = 0; idx < num_channels; ++idx)
    {
        char path[128] = { 0 };

        // Get device path for this channel
        MWGetDevicePath(idx, path);
        hChannel = MWOpenChannelByPath(path);
        if (hChannel == nullptr)
        {
            cerr << "ERROR: failed to open input " << idx << "\n";
            continue;
        }

        MWCAP_CHANNEL_INFO channelInfo = { 0 };

        // Get channel information
        if (MW_SUCCEEDED != MWGetChannelInfo(hChannel, &channelInfo))
        {
            cerr << "ERROR: failed to get channel info for input "
                 << idx << "\n";
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
            cerr << "Board: " << static_cast<int>(channelInfo.byBoardIndex)
                 << ", Product: " << channelInfo.szProductName
                 << ", SerialNo: " << channelInfo.szBoardSerialNo
                 << ", Firmware: " << channelInfo.dwFirmwareVersion
                 << ", Driver: " << channelInfo.dwDriverVersion
                 << "\n";
        }
        prev_channelInfo = channelInfo;

        // Display channel information
        cerr << "[" << idx + 1 << "] ";
        describe_input(hChannel);

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

    // Lock cerr for thread-safe output
    ios_lock lock;
    cerr << lock_ios(lock);

    // Check if any channels are available
    if (channel_cnt == 0)
    {
        cerr << "ERROR: Failed to detect any input channels.";
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
            cerr << "ERROR: Only " << channel_cnt
                 << " input channels detected. Cannot open input "
                 << devIndex << "\n";
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
        cerr << "ERROR: Failed to open input channel ";
        if (boardId >= 0)
            cerr << "board " << boardId << " ";
        cerr << devIndex + 1 << endl;
    }

    m_channel_idx = devIndex;

    // Get channel info
    MWCAP_CHANNEL_INFO channel_info = { 0 };
    if (MW_SUCCEEDED != MWGetChannelInfo(m_channel, &channel_info))
    {
        cerr << "ERROR: Unable to retrieve channel info for index "
             << m_channel_idx << "!\n";
        return false;
    }

    // Display channel info if verbose mode is enabled
    if (m_verbose > 0)
    {
        uint temperature = 0;
        MWGetTemperature(m_channel, &temperature);

        cerr << "Board: " << static_cast<int>(channel_info.byBoardIndex)
             << ", Product: " << channel_info.szProductName
             << ", SerialNo: " << channel_info.szBoardSerialNo
             << ", Firmware: " << channel_info.dwFirmwareVersion
             << ", Driver: " << channel_info.dwDriverVersion
             << ", Temperature: " << temperature
             << "\n";
    }

    // Set channel info and determine if using ECO mode
    channel_info.szFamilyName[sizeof(channel_info.szFamilyName)-1] = '\0';
    m_channel_info = channel_info;
    m_isEco = strcmp(m_channel_info.szFamilyName, "Eco Capture") == 0;

    // Get input status
    MWCAP_INPUT_SPECIFIC_STATUS status;
    if (MWGetInputSpecificStatus(m_channel, &status) != MW_SUCCEEDED)
        cerr << "Unable to get input status!\n";
    else if(!status.bValid)
        cerr << "No signal detected on input.\n";

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
    _MWCAP_AUDIO_NODE node = MWCAP_AUDIO_EMBEDDED_CAPTURE;
    MWGetAudioVolume(m_channel, node, &volume);

    // Lock cerr for thread-safe output
    ios_lock lock;
    cerr << lock_ios(lock);

    // Display volume range
    cerr << "VolumeMin: " << volume.sVolumeMin << "\n"
         << "VolumeMax: " << volume.sVolumeMax << "\n"
         << "VolumeStep: " << volume.sVolumeStep << "\n";

    // Display volume for each channel
    for (int idx = 0; idx < 8; ++idx)
    {
        cerr << "[" << idx << "] Mute: "
             << (volume.abMute[idx] ? "Yes" : "No")
             << ", Volume: " << volume.asVolume[idx] << "\n";
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

    // Calculate scaled volume
    float scale = (volume.sVolumeMax - volume.sVolumeMin) / 100;
    int scaled_volume = volume_level * scale;

    // Apply volume to all channels
    for(int i=0; i<MWCAP_MAX_NUM_AUDIO_CHANNEL; ++i)
    {
        volume.asVolume[i] = scaled_volume + volume.sVolumeMin;
    }

    // Set new volume
    MWSetAudioVolume(m_channel, node, &volume);

    // Display confirmation if verbose mode is enabled
    if (m_verbose > 0)
        cerr << lock_ios()
             << "Volume set to " << volume_level << " for all channels.\n";

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

    // Lock cerr for thread-safe output
    ios_lock lock;
    cerr << lock_ios(lock);

    // Get video and audio source information
    if (MW_SUCCEEDED != MWGetVideoInputSource(m_channel, &dwVideoSource))
    {
        cerr << "ERROR: Can't get video input source!\n";
        return false;
    }

    if (MW_SUCCEEDED != MWGetAudioInputSource(m_channel, &dwAudioSource))
    {
        cerr << "ERROR: Can't get audio input source!\n";
        return false;
    }

    // Verify both are HDMI
    if (INPUT_TYPE(dwVideoSource) != MWCAP_VIDEO_INPUT_TYPE_HDMI ||
        INPUT_TYPE(dwAudioSource) != MWCAP_AUDIO_INPUT_TYPE_HDMI)
    {
        cerr << "Type of input source is not HDMI !\n";
        return false;
    }

    MW_RESULT xr;
    FILE* pFile = nullptr;
    pFile=fopen(filepath.c_str(), "wb");
    if (pFile == nullptr)
    {
        cerr << "ERROR: Could not read EDID file '" << filepath << "'!\n";
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
            cerr << "Wrote EDID to '" << filepath << "'\n";
        }
        else
        {
            cerr << "ERROR: Failed to write to '" << filepath << "'"
                 << strerror(errno) << "\n";
        }
    }
    else
    {
        cerr << "ERROR: Get EDID Info!\n";
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

    // Lock cerr for thread-safe output
    ios_lock lock;
    cerr << lock_ios(lock);

    // Get video and audio source information
    if (MW_SUCCEEDED != MWGetVideoInputSource(m_channel, &dwVideoSource))
    {
        cerr << "ERROR: Can't get video input source!\n";
        return false;
    }

    if (MW_SUCCEEDED != MWGetAudioInputSource(m_channel, &dwAudioSource))
    {
        cerr << "ERROR: Can't get audio input source!\n";
        return false;
    }

    // Verify both are HDMI
    if (INPUT_TYPE(dwVideoSource) != MWCAP_VIDEO_INPUT_TYPE_HDMI ||
        INPUT_TYPE(dwAudioSource) != MWCAP_AUDIO_INPUT_TYPE_HDMI)
    {
        cerr << "Type of input source is not HDMI !\n";
        return false;
    }

    MW_RESULT xr;

    // Open file for reading
    FILE* pFile = nullptr;
    pFile=fopen(filepath.c_str(), "rb");
    if (pFile == nullptr)
    {
        cerr << "ERROR: could not read from EDID file '" << filepath << "'!\n";
        return false;
    }

    BYTE byData[1024];
    int nSize = (int)fread(byData, 1, 1024, pFile);

    // Write EDID data
    xr = MWSetEDID(m_channel, byData, nSize);
    if (xr == MW_SUCCEEDED)
        cerr << "EDID written successfully.\n";
    else
        cerr << "Failed to write EDID!\n";

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
    bool     params_changed  = false;
    bool     lr16bit         = false;

    ULONGLONG notify_status = 0;
    MWCAP_AUDIO_CAPTURE_FRAME macf;

    if (m_verbose > 2)
        cerr << "Starting audio capture loop\n";

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
            cerr << "ERROR: can't find audio input\n";
        goto audio_capture_stoped;
    }

    // Start audio capture
    if (MW_SUCCEEDED != MWStartAudioCapture(m_channel))
    {
        if (m_verbose > 0)
            cerr << "ERROR: start audio capture fail!\n";
        goto audio_capture_stoped;
    }

    // Set up notification based on capture mode
    if (m_isEco)
    {
        eco_event = eventfd(0, EFD_NONBLOCK);
        if (eco_event < 0)
        {
            cerr << lock_ios() << "ERROR: Failed to create eco event.\n";
            Shutdown();
            return;
        }
        notify_audio  = MWRegisterNotify(m_channel, eco_event,
                                         (DWORD)MWCAP_NOTIFY_AUDIO_FRAME_BUFFERED |
                                         (DWORD)MWCAP_NOTIFY_AUDIO_SIGNAL_CHANGE  |
                                         (DWORD)MWCAP_NOTIFY_AUDIO_INPUT_RESET
                                         );
    }
    else
    {
        notify_event = MWCreateEvent();
        if (notify_event == 0)
        {
            cerr << lock_ios() << "ERROR: create notify_event fail\n";
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
        cerr << lock_ios()
             << "Audio capture starting\n";

    // Main audio capture loop
    while (m_running.load() == true)
    {
        // Get audio signal status
        if (MW_SUCCEEDED != MWGetAudioSignalStatus(m_channel,
                                                   &audio_signal_status))
        {
            if (m_verbose > 0 && ++err_cnt % 50 == 0)
                cerr << lock_ios() << "WARNING (cnt: " << err_cnt
                     << ") can't get audio signal status\n";
            this_thread::sleep_for(chrono::milliseconds(m_frame_ms));
            continue;
        }

        // Check if audio signal is valid
        if (!audio_signal_status.bChannelStatusValid)
        {
            if (good_signal && m_verbose > 0 && ++err_cnt % 100 == 0)
                cerr << lock_ios() << "No audio signal.\n";
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
            // Lock cerr for thread-safe output
            ios_lock lock;
            cerr << lock_ios(lock);

            // Check for parameter changes
            if (m_reset_audio.load() == true)
            {
                if (m_verbose > 1)
                    cerr << "Audio re-initializing." << endl;
                params_changed = true;
            }
            if (lpcm != audio_signal_status.bLPCM)
            {
                if (m_verbose > 1)
                {
                    if (lpcm)
                        cerr << "lPCM -> Bitstream" << endl;
                    else
                        cerr << "Bitstream -> lPCM" << endl;
                }
                lpcm = audio_signal_status.bLPCM;
                params_changed = true;
            }
            if (sample_rate != audio_signal_status.dwSampleRate)
            {
                if (m_verbose > 1)
                    cerr << "Audio sample rate " << sample_rate
                         << " -> " << audio_signal_status.dwSampleRate << endl;
                sample_rate = audio_signal_status.dwSampleRate;
                params_changed = true;
            }
            if (bytes_per_sample != even_bytes_per_sample)
            {
                if (m_verbose > 1)
                    cerr << "Audio bytes per sample " << bytes_per_sample
                         << " -> " << even_bytes_per_sample << endl;
                bytes_per_sample = even_bytes_per_sample;
                params_changed = true;
            }
            if (valid_channels != audio_signal_status.wChannelValid)
            {
                if (m_verbose > 1)
                    cerr << "Audio channels " << valid_channels
                         << " -> " << audio_signal_status.wChannelValid << endl;
                valid_channels = audio_signal_status.wChannelValid;
                params_changed = true;
            }
        }

        // Handle parameter changes
        if (params_changed)
        {
            params_changed = false;

            if (m_verbose > 1 /* && frame_cnt > 0 */)
                cerr << lock_ios()
                     << "Audio signal CHANGED after "
                     << frame_cnt << " frames.\n";

            cur_channels = 0;
            for (int idx = 0; idx < (MWCAP_AUDIO_MAX_NUM_CHANNELS / 2); ++idx)
            {
                cur_channels +=
                    (valid_channels & (0x01 << idx)) ? 2 : 0;
            }

            if (0 == cur_channels)
            {
                if (m_verbose > 0 && err_cnt++ % 25 == 0)
                    cerr << lock_ios() << "WARNING [" << err_cnt
                         << "] Invalid audio channel count: "
                         << cur_channels << endl;

                this_thread::sleep_for(chrono::milliseconds(m_frame_ms));
                continue;
            }

            lr16bit = (cur_channels == 2 && bytes_per_sample == 2);

            frame_size = MWCAP_AUDIO_SAMPLES_PER_FRAME
                         * cur_channels * bytes_per_sample;

            // Set audio parameters in output handler
            m_out2ts->setAudioParams(cur_channels, lpcm,
                                     bytes_per_sample,
                                     sample_rate,
                                     MWCAP_AUDIO_SAMPLES_PER_FRAME,
                                     frame_size);

            // Handle shutdown if output handler is invalid
            if (!m_out2ts)
                Shutdown();

            m_reset_audio.store(false);
        }

        err_cnt = 0;
        frame_cnt = 0;
        for (;;)
        {
            if (m_reset_audio.load() == true)
                goto audio_capture_stoped;

            // Wait for notification
            if (m_isEco)
            {
                if (EcoEventWait(eco_event, -1) <= 0)
                {
                    if (m_verbose > 1)
                        cerr << lock_ios()
                             << "Audio wait notify error or timeout\n";
                    continue;
                }
            }
            else
            {
                if (MWWaitEvent(notify_event, -1) <= 0)
                {
                    if (m_verbose > 1)
                        cerr << lock_ios()
                             << "Audio wait notify error or timeout\n";
                    continue;
                }
            }

            // Get notification status
            if (MW_SUCCEEDED != MWGetNotifyStatus(m_channel,
                                                  notify_audio,
                                                  &notify_status))
                continue;

#if 0
            // Handle signal change
            if (!m_isEco)
            {
                // TODO: Sometime spurious, what to do?
                if (notify_status & MWCAP_NOTIFY_AUDIO_SIGNAL_CHANGE)
                {
                    if (m_verbose > 0)
                        cerr << lock_ios() << "AUDIO signal changed.\n";
                    this_thread::sleep_for(chrono::milliseconds(m_frame_ms));
                    break;
                }
            }
#endif

            // Handle input reset
            if (notify_status & MWCAP_NOTIFY_AUDIO_INPUT_RESET)
            {
                if (m_verbose > 0)
                    cerr << lock_ios()
                         << "WARNING: Audio input restarting.\n";
                this_thread::sleep_for(chrono::milliseconds(m_frame_ms));
                break;
            }

            // Check if frame is buffered
            if (!(notify_status & MWCAP_NOTIFY_AUDIO_FRAME_BUFFERED))
                continue;

            // Capture audio frame
            if (MW_ENODATA == MWCaptureAudioFrame(m_channel, &macf))
            {
//                this_thread::sleep_for(chrono::milliseconds(m_frame_ms));
                continue;
            }

            ++frame_cnt;

#ifdef DUMP_RAW_AUDIO_ALLBITS
            /*
              Audio sample data. Each sample is 32-bit width, and
              high bit effective. The priority of the path is:
              Left0, Left1, Left2, Left3, right0, right1, right2,
              right3.
            */
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
            if (lr16bit)
            {
                // Optimized for bitstream and 2-channel, 16bit PCM
                uint8_t* byteP = reinterpret_cast<uint8_t*>(macf.adwSamples);
                uint8_t* endP = byteP +
                                ((MWCAP_AUDIO_SAMPLES_PER_FRAME *
                                  MWCAP_AUDIO_MAX_NUM_CHANNELS) * 4);
                byteP += 2; // shift 16 bits
                while (byteP < endP)
                {
                    audio_frame->push_back(*byteP);
                    ++byteP;
                    audio_frame->push_back(*byteP);
                    byteP += 15;
                }
            }
            else
            {
                /*
                  L1L2L3L4 R1R2R3R4 L5L6L7L8 R5R6R7R8 (32bits per channel)
                  to 2channel 16bit
                  L1R1L5R5(2byte)
                */

                int left_pos, right_pos;
                uint32_t left, right;
                int half_channels = MWCAP_AUDIO_MAX_NUM_CHANNELS / 2;
                int shift = audio_signal_status.cBitsPerSample > 16 ? 0 : 16;

                int in_size = MWCAP_AUDIO_SAMPLES_PER_FRAME *
                              MWCAP_AUDIO_MAX_NUM_CHANNELS;

                for (int chan = 0; chan < (cur_channels/2); ++chan)
                {
                    for (int sample = 0 ; sample < in_size;
                         sample += MWCAP_AUDIO_MAX_NUM_CHANNELS)
                    {
                        left_pos = sample + chan;
                        right_pos = left_pos + half_channels;
                        left = macf.adwSamples[left_pos] >> shift;
                        right = macf.adwSamples[right_pos] >> shift;

                        copy(reinterpret_cast<uint8_t*>(&left),
                             reinterpret_cast<uint8_t*>(&left) + bytes_per_sample,
                             back_inserter(*audio_frame));
                        copy(reinterpret_cast<uint8_t*>(&right),
                             reinterpret_cast<uint8_t*>(&right) + bytes_per_sample,
                             back_inserter(*audio_frame));
                    }
                }
            }

#ifdef DUMP_RAW_AUDIO
            /*
              Bitstream Audio: Each sample is 16-bits for L1 and 16-bits for R1
              16-bit PCM: Each sample is 16-bits for each valid channel: L1R1L2R2, etc...
              24-bit PCM: Each sample is 32-bits for each valid channel: L1R1L2R2, etc...
            */
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

  audio_capture_stoped:
    cerr << lock_ios()
         << "\nAudio Capture finished.\n" << endl;

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
        cerr << lock_ios() << "Not a HDMI info frame\n";
        return false;
    }

    if (0 == uiValidFlag)
    {
        cerr << lock_ios() << "No HDMI InfoFrame!\n";
        return false;
    }

    if (0 == (uiValidFlag & MWCAP_HDMI_INFOFRAME_MASK_HDR))
        return false;

    // Get HDR info frame
    if (MW_SUCCEEDED != MWGetHDMIInfoFramePacket(m_channel,
                                                 MWCAP_HDMI_INFOFRAME_ID_HDR,
                                                 &m_infoPacket))
    {
        cerr << lock_ios() << "WARNING: HDMI HDR infoframe not available.\n";
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
        cerr << lock_ios() << "HDR info has not changed.\n";
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
            cerr << lock_ios() << "Color format: YUV601\n";
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
            cerr << lock_ios() << "Color format: YUV2020\n";
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
            cerr << lock_ios() << "Color format: YUV709\n";
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

    --m_image_buffers_inflight;

    // Check if we have too many buffers
    if (m_avail_image_buffers.size() > m_image_buffers_desired)
    {
        if (m_verbose > 3)
            cerr << lock_ios() << "Releasing excess video buffer.\n";
        // Unpin and delete buffer if we have too many
        MWUnpinVideoBuffer(m_channel, (LPBYTE)(pbImage));
        delete[] pbImage;
        if (--m_image_buffer_total <
            (m_image_buffers_desired + 2) && m_verbose > 2)
            cerr << lock_ios() << "INFO: Video encoder is "
                 << m_image_buffer_total << " frames behind.\n";

        m_image_buffers.erase(pbImage);
    }
    else
        m_avail_image_buffers.push_back(pbImage);

    // Notify waiting threads if needed
    if (m_image_buffers_desired == 0 && m_image_buffers_inflight == 0)
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

    --m_image_buffers_inflight;

    MWCAP_VIDEO_ECO_CAPTURE_FRAME* pEco =
        reinterpret_cast<MWCAP_VIDEO_ECO_CAPTURE_FRAME *>(buf);

    // Check if we have too many buffers
    if (m_image_buffer_avail > m_image_buffers_desired)
    {
        if (--m_image_buffer_total <
            (m_image_buffers_desired + 2) && m_verbose > 2)
            cerr << lock_ios() << "INFO: Video encoder is "
                 << m_image_buffer_total << " frames behind.\n";

        // Remove and free buffer
        m_eco_buffers.erase(pEco);
        delete[] reinterpret_cast<uint8_t *>(pEco->pvFrame);
        delete pEco;
    }
    else
    {
        // Re-queue buffer if we have enough
        if (MW_SUCCEEDED != MWCaptureSetVideoEcoFrame(m_channel, pEco))
        {
            cerr << lock_ios()
                 << "ERROR: buffer_avail: Failed to Q the Eco frame. "
                 << " desired " << m_image_buffers_desired
                 << " avail " << m_image_buffer_avail << endl;
            delete[] reinterpret_cast<uint8_t *>(pEco->pvFrame);
            pEco->pvFrame = 0;
            delete pEco;
            pEco = nullptr;
        }
        else
            ++m_image_buffer_avail;
    }

    // Notify waiting threads if needed
    if (m_image_buffers_desired == 0 && m_image_buffers_inflight == 0)
        m_image_returned.notify_one();
}

/**
 * @brief Free all image buffers
 *
 * Releases all allocated image buffers and cleans up resources.
 */
void Magewell::free_image_buffers(void)
{
    {
        unique_lock<mutex> lock(m_image_buffer_mutex);
        m_image_buffers_desired = 0;
    }

    unique_lock<mutex> lock(m_image_buffer_mutex);

    // Wait for all buffers to be returned from output thread
    m_image_returned.wait_for(lock, chrono::milliseconds(m_frame_ms),
                              [this]{return m_image_buffers_inflight == 0;});

    // Free ECO buffers if using ECO mode
    if (m_isEco)
    {
        ecoque_t::iterator Ieco;
        for (Ieco = m_eco_buffers.begin(); Ieco != m_eco_buffers.end(); ++Ieco)
        {
            delete[] reinterpret_cast<uint8_t *>((*Ieco)->pvFrame);
            (*Ieco)->pvFrame = 0;
        }

        m_eco_buffers.clear();
    }
    else
    {
        // Free PRO buffers
        imageset_t::iterator Iimage;
        for (Iimage = m_image_buffers.begin();
             Iimage != m_image_buffers.end(); ++Iimage)
        {
            MWUnpinVideoBuffer(m_channel, (LPBYTE)(*Iimage));
            delete[] (*Iimage);
        }
        m_image_buffers.clear();
        m_avail_image_buffers.clear();
    }

    // Reset buffer counters
    m_image_buffer_avail = 0;
    m_image_buffer_total = 0;
    m_image_buffers_desired = k_min_video_buffers;
}

/**
 * @brief Add a new ECO image buffer
 *
 * Allocates and initializes a new ECO capture buffer.
 *
 * @return true if successful, false otherwise
 */
bool Magewell::add_eco_image_buffer(void)
{
    MW_RESULT xr;
    MWCAP_VIDEO_ECO_CAPTURE_FRAME * pBuf = new MWCAP_VIDEO_ECO_CAPTURE_FRAME;

    unique_lock<mutex> lock(m_image_buffer_mutex);

    // Initialize buffer parameters
    pBuf->deinterlaceMode = MWCAP_VIDEO_DEINTERLACE_BLEND;
    pBuf->cbFrame  = m_image_size;
    pBuf->pvFrame  = reinterpret_cast<MWCAP_PTR>(new uint8_t[m_image_size]);
    pBuf->cbStride = m_min_stride;
    pBuf->bBottomUp = false;

    // Check if allocation succeeded
    if (reinterpret_cast<uint8_t *>(pBuf->pvFrame) == nullptr)
    {
        cerr << lock_ios() << "Eco video frame alloc failed.\n";
        return false;
    }
    pBuf->pvContext = reinterpret_cast<MWCAP_PTR>(pBuf);
    memset(reinterpret_cast<uint8_t *>(pBuf->pvFrame), 0, m_image_size);

    // Register buffer with capture system
    if ((xr = MWCaptureSetVideoEcoFrame(m_channel, pBuf)) != MW_SUCCEEDED)
    {
        cerr << lock_ios() << "MWCaptureSetVideoEcoFrame failed!\n";
        return false;
    }

    // Add to buffer set and increment counters
    m_eco_buffers.insert(pBuf);
    ++m_image_buffer_total;
    ++m_image_buffer_avail;

    if (m_verbose > 2)
        cerr << lock_ios()
             << "Added Eco frame (" << m_image_buffer_avail << "/"
             << m_image_buffer_total << ") flight " << m_image_buffers_inflight
             << endl;

    return true;
}

/**
 * @brief Add a new PRO image buffer
 *
 * Allocates and initializes a new PRO capture buffer.
 *
 * @return true if successful, false otherwise
 */
bool Magewell::add_pro_image_buffer(void)
{
    unique_lock<mutex> lock(m_image_buffer_mutex);

    // Allocate memory for image buffer
    uint8_t* pbImage =  new uint8_t[m_image_size];
    if (pbImage == nullptr)
    {
        cerr << lock_ios() << "ERROR: image buffer alloc fail!\n";
        return false;
    }

    // Pin buffer to prevent memory movement
    MWPinVideoBuffer(m_channel, (MWCAP_PTR)pbImage, m_image_size);

    // Add to buffer sets
    m_image_buffers.insert(pbImage);
    m_avail_image_buffers.push_back(pbImage);

    ++m_image_buffer_total;
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
                cerr << lock_ios()
                     << "ERROR: Start Eco Video Capture error: invalid params\n";
            else if (ret == MW_FAILED)
                cerr << lock_ios()
                     << "ERROR: Start Eco Video Capture error: general failure\n";
            else
                cerr << lock_ios()
                     << "ERROR: Start Eco Video Capture error: " << ret << "\n";
        }

        this_thread::sleep_for(chrono::milliseconds(100));
    }
    if (idx == 5)
        return false;

    if (m_verbose > 1)
        cerr << lock_ios()
             << "Eco Video capture started.\n";

    return true;
}

/**
 * @brief Close ECO video capture
 *
 * Stops ECO video capture and frees buffers.
 */
void Magewell::close_eco_video(void)
{
    // Stop capture
    MWStopVideoEcoCapture(m_channel);
    // Free buffers
    free_image_buffers();
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
    int frame_cnt  = 0;

    uint8_t* pbImage = nullptr;
    int64_t  timestamp;

    MWCAP_VIDEO_ECO_CAPTURE_STATUS eco_status;
    MW_RESULT ret;

    // Main capture loop
    while (m_running.load() == true)
    {
        // Wait for notification
        if (EcoEventWait(eco_event, -1) <= 0)
        {
            if (m_verbose > 1)
                cerr << lock_ios()
                     << "Video wait notify error or timeout (frame "
                     << frame_cnt << ")\n";
            continue;
        }

        // Get notification status
        if (MW_SUCCEEDED != MWGetNotifyStatus(m_channel, video_notify,
                                              &ullStatusBits))
        {
            if (m_verbose > 0)
                cerr << lock_ios()
                     << "WARNING: Failed to get Notify status (frame "
                     << frame_cnt << ")\n";
            continue;
        }

        // Handle reset
        if (m_reset_video.load() == true)
        {
            if (m_verbose > 1)
                cerr << "Video reset." << endl;
            return true;
        }

        // Handle signal change
        if (ullStatusBits & MWCAP_NOTIFY_VIDEO_SIGNAL_CHANGE)
        {
            if (m_verbose > 1)
                cerr << lock_ios()
                     << "MWCAP_NOTIFY_VIDEO_SIGNAL_CHANGE\n";
            this_thread::sleep_for(chrono::milliseconds(5));
            return false;
        }

        // Check if we have enough buffers
        if (m_image_buffer_avail < 2)
        {
            if (m_image_buffers_inflight > 25)
            {
                if (m_verbose > 0)
                    cerr << "Dropping Eco frame.\n";
                continue;
            }
            else
                add_eco_image_buffer();
            if (m_verbose > 2)
                cerr << lock_ios()
                     << "WARNING: video encoder is "
                     << m_image_buffer_total << " frames behind (frame "
                     << frame_cnt << ")\n";
        }

        // Get capture status
        memset(&eco_status, 0, sizeof(eco_status));
        ret = MWGetVideoEcoCaptureStatus(m_channel, &eco_status);
        if (0 != ret ||
            eco_status.pvFrame == reinterpret_cast<MWCAP_PTR>(nullptr))
        {
            if (m_verbose > 4)
                cerr << lock_ios()
                     << "WARNING: Failed to get Eco video frame.\n";
//                    add_eco_image_buffer();
            this_thread::sleep_for(chrono::milliseconds(1));
            continue;
        }

        // Process frame
        ++m_image_buffers_inflight;
        --m_image_buffer_avail;
        pbImage = reinterpret_cast<uint8_t *>(eco_status.pvFrame);
        timestamp = eco_status.llTimestamp;

        ++frame_cnt;

        if (ret != MW_SUCCEEDED)
        {
            cerr << lock_ios() << "Failed\n";
            eco_image_buffer_available(pbImage,
                               reinterpret_cast<MWCAP_VIDEO_ECO_CAPTURE_FRAME *>
                               (eco_status.pvContext));
            continue;
        }

        // Add frame to output handler
        if (!m_out2ts->AddVideoFrame(pbImage,
                             reinterpret_cast<MWCAP_VIDEO_ECO_CAPTURE_FRAME *>
                             (eco_status.pvContext),
                             m_num_pixels, timestamp))
            Shutdown();
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
    int frame_cnt  = 0;
    int frame_idx  = -1;

    uint8_t* pbImage = nullptr;
    int64_t  timestamp = -1;
    int64_t  previous_ts = -1;
    int64_t  expected_ts = -1;
    int64_t  frame_ts    = -1;
#if 0
    long long card_ts    = -1;
#endif
    int      previous_idx = -1;

    int      quarter_dur = eco_params.llFrameDuration / 4;

    MWCAP_VIDEO_BUFFER_INFO   videoBufferInfo;
    MWCAP_VIDEO_FRAME_INFO    videoFrameInfo;
    MWCAP_VIDEO_SIGNAL_STATUS videoSignalStatus;
    MW_RESULT ret;

    // Main capture loop
    while (m_running.load() == true)
    {
        // Wait for notification
        if (MWWaitEvent(notify_event, m_frame_ms2) <= 0)
        {
            if (m_verbose > 1)
                cerr << lock_ios()
                     << "Video wait notify error or timeout (frame "
                     << frame_cnt << ")\n";
            continue;
        }

        // Get notification status
        if (MW_SUCCEEDED != MWGetNotifyStatus(m_channel, video_notify,
                                              &ullStatusBits))
        {
            if (m_verbose > 0)
                cerr << lock_ios()
                     << "WARNING: Failed to get Notify status (frame "
                     << frame_cnt << ")\n";
            continue;
        }

        // Handle reset
        if (m_reset_video.load() == true)
        {
            if (m_verbose > 1)
                cerr << "Video reset." << endl;
            return true;
        }

        // Handle signal change
        if (ullStatusBits & MWCAP_NOTIFY_VIDEO_SIGNAL_CHANGE)
        {
            if (m_verbose > 1)
                cerr << lock_ios()
                     << "MWCAP_NOTIFY_VIDEO_SIGNAL_CHANGE\n";
            this_thread::sleep_for(chrono::milliseconds(5));
            return false;
        }

        // Check signal lock status
        MWGetVideoSignalStatus(m_channel, &videoSignalStatus);
        if (videoSignalStatus.state != MWCAP_VIDEO_SIGNAL_LOCKED)
        {
            if (m_verbose > 0)
                cerr << lock_ios()
                     << "WARNING: Video signal lost lock. (frame "
                     << frame_cnt << ")\n";
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
                cerr << lock_ios()
                     << "WARNING: Failed to get video buffer info (frame "
                     << frame_cnt << ")\n";
            continue;
        }

        // Manage frame index
        previous_idx = frame_idx;
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
                cerr << lock_ios()
                     << "WARNING: Failed to get video frame info (frame "
                     << frame_cnt << ")\n";
            continue;
        }

        frame_ts = videoFrameInfo.allFieldBufferedTimes[0];
#if 0
        MWGetDeviceTime(m_channel, &card_ts);
#endif
        if (frame_ts == -1)
            timestamp = expected_ts;
        else
        {
            timestamp = frame_ts;

            if (timestamp < expected_ts - quarter_dur ||
                expected_ts + quarter_dur < timestamp)
            {
                if (expected_ts >= 0)
                {
                    if (m_verbose > 2)
                    {
                        cerr << lock_ios()
                             << "WARNING: Unexpected TimeStamp " << "[" << previous_idx << " -> "
                             << frame_idx << "]"
                             << " diff:" << expected_ts - timestamp
                             << " prev:" << previous_ts
                             << " expected:" << expected_ts
                             << " actual:" << timestamp
#if 0
                             << " cardTS:" << card_ts
                             << " diff: " << card_ts - timestamp
#endif
                             << "\n";
                    }
                    if (timestamp > expected_ts)
                    {
                        if (m_verbose > 0)
                            cerr << "WARNING: Magewell driver lost a frame. Can't keep up!\n";
                    }
                    else
                        timestamp = expected_ts;
                }
            }
        }
        expected_ts = timestamp + eco_params.llFrameDuration;
        previous_ts = timestamp;

        // Get available buffer
        m_image_buffer_mutex.lock();
        if (m_avail_image_buffers.empty())
        {
            m_image_buffer_mutex.unlock();
            add_pro_image_buffer();
            m_image_buffer_mutex.lock();
            if (m_verbose > 2)
                cerr << lock_ios()
                     << "WARNING: video encoder is "
                     << m_image_buffer_total << " frames behind (frame "
                     << frame_cnt << ")\n";
        }

        pbImage = m_avail_image_buffers.front();
        m_avail_image_buffers.pop_front();
        m_image_buffer_mutex.unlock();

        // Capture frame to virtual address
        ret = MWCaptureVideoFrameToVirtualAddress
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

        // Wait for capture completion
        if (MWWaitEvent(capture_event, -1) <= 0)
        {
            if (m_verbose > 0)
                cerr << lock_ios()
                     << "WARNING: wait capture event error or timeout "
                     << "(frame " << frame_cnt << ")\n";
            pro_image_buffer_available(pbImage, nullptr);
            continue;
        }

        // Get capture status
        MWCAP_VIDEO_CAPTURE_STATUS captureStatus;
        MWGetVideoCaptureStatus(m_channel, &captureStatus);

        ++frame_cnt;

        if (ret != MW_SUCCEEDED)
        {
            cerr << lock_ios() << "Failed\n";
            pro_image_buffer_available(pbImage, nullptr);
            continue;
        }

        // Add frame to output handler
        if (!m_out2ts->AddVideoFrame(pbImage, nullptr,
                                     m_num_pixels, timestamp))
            Shutdown();
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
bool Magewell::capture_video(void)
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
    size_t    idx;
    bool      rejected = false;

#if 0
    DWORD event_mask = MWCAP_NOTIFY_VIDEO_SAMPLING_PHASE_CHANGE |
                       MWCAP_NOTIFY_VIDEO_SMPTE_TIME_CODE |
                       MWCAP_NOTIFY_VIDEO_SIGNAL_CHANGE |
                       MWCAP_NOTIFY_HDMI_INFOFRAME_HDR;
#endif

    if (m_verbose > 0)
        cerr << lock_ios() << "Video capture starting.\n";

    if (m_isEco)
    {
        eco_event = eventfd(0, EFD_NONBLOCK);
        if (eco_event < 0)
        {
            cerr << lock_ios()
                 << "Unable to create event fd for eco capture.\n";
            Shutdown();
        }
    }
    else
    {
        capture_event = MWCreateEvent();
        if (capture_event == 0)
        {
            if (m_verbose > 0)
                cerr << lock_ios() << "ERROR: Create timer event error\n";
            Shutdown();

        }

        notify_event = MWCreateEvent();
        if (notify_event == 0)
        {
            if (m_verbose > 0)
                cerr << lock_ios() << "ERROR: Create notify event error\n";
            Shutdown();
        }

        if (MW_SUCCEEDED != MWStartVideoCapture(m_channel, capture_event))
        {
            if (m_verbose > 0)
                cerr << lock_ios() << "ERROR: Start Pro Video Capture error!\n";
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
                cerr << lock_ios()
                     << "WARNING: Input video signal status: Unsupported\n";
            locked = false;
            state = videoSignalStatus.state;
            this_thread::sleep_for(chrono::milliseconds(m_frame_ms * 10));
            continue;
        }

        switch (videoSignalStatus.state)
        {
            case MWCAP_VIDEO_SIGNAL_LOCKED:
              if (!locked && m_verbose > 1)
                  cerr << lock_ios()
                       << "INFO: Input video signal status: Locked\n";
              locked = true;
              break;
            case MWCAP_VIDEO_SIGNAL_NONE:
              if (state != videoSignalStatus.state && m_verbose > 0)
                  cerr << lock_ios()
                       << "WARNING: Input video signal status: NONE\n";
              locked = false;
              state = videoSignalStatus.state;
              this_thread::sleep_for(chrono::milliseconds(m_frame_ms * 5));
              continue;
            case MWCAP_VIDEO_SIGNAL_LOCKING:
              if (state != videoSignalStatus.state && m_verbose > 0)
                  cerr << lock_ios()
                       << "WARNING: Input video signal status: Locking\n";
              locked = false;
              state = videoSignalStatus.state;
              this_thread::sleep_for(chrono::milliseconds(m_frame_ms * 5));
              continue;
            default:
              if (m_verbose > 0)
                  cerr << lock_ios()
                       << "WARNING: Video signal status: lost locked.\n";
              locked = false;
              this_thread::sleep_for(chrono::milliseconds(m_frame_ms * 5));
              continue;
        }

        if (videoSignalStatus.bInterlaced)
        {
            if (!rejected && m_verbose > 0)
                cerr << lock_ios() << "REJECTING interlaced video.\n";
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
                cerr << lock_ios()
                     << "ERROR: Failed to determine best magewell pixel format.\n";
                Shutdown();
            }

            m_isHDR = false;
        }

        if (eco_params.cx != videoSignalStatus.cx)
        {
            if (m_verbose > 1)
                cerr << lock_ios() << "Width: " << eco_params.cx
                     << " -> " << videoSignalStatus.cx << "\n";
            eco_params.cx = videoSignalStatus.cx;
            params_changed = true;
        }
        if (eco_params.cy != videoSignalStatus.cy)
        {
            if (m_verbose > 1)
                cerr << lock_ios() << "Height: " << eco_params.cy
                     << " -> " << videoSignalStatus.cy << "\n";
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
                cerr << lock_ios() << "Num pixels: " << m_num_pixels
                     << " -> " << m_min_stride * eco_params.cy << "\n";
            m_num_pixels = m_min_stride * eco_params.cy;
            params_changed = true;
        }
        if (eco_params.llFrameDuration != videoSignalStatus.dwFrameDuration)
        {
            if (m_verbose > 1)
                cerr << lock_ios() << "Duration: " << eco_params.llFrameDuration
                     << " -> " << videoSignalStatus.dwFrameDuration << "\n";
            eco_params.llFrameDuration = videoSignalStatus.dwFrameDuration;
            params_changed = true;
        }
        if (interlaced != static_cast<bool>(videoSignalStatus.bInterlaced))
        {
            if (m_verbose > 1)
                cerr << lock_ios() << "Interlaced: " << (interlaced ? "Y" : "N")
                     << " -> " << (videoSignalStatus.bInterlaced ? "Y" : "N")
                     << "\n";
            interlaced = static_cast<bool>(videoSignalStatus.bInterlaced);
            params_changed = true;
        }
        if (bpp != FOURCC_GetBpp(eco_params.dwFOURCC))
        {
            if (m_verbose > 1)
                cerr << lock_ios() << "Video Bpp: " << bpp << " -> "
                     << FOURCC_GetBpp(eco_params.dwFOURCC) << "\n";
            bpp = FOURCC_GetBpp(eco_params.dwFOURCC);
            params_changed = true;
        }

        if (params_changed || color_changed)
        {
            color_changed = false;
            params_changed = false;

            if (m_verbose > 1 /* && frame_cnt > 0 */)
                cerr << lock_ios() << "Video signal CHANGED.\n";

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
                // Mutex lock cerr
                ios_lock lock;
                cerr << lock_ios(lock);

                cerr << "========\n";
                double fps = (interlaced) ?
                             (double)20000000LL / eco_params.llFrameDuration :
                             (double)10000000LL / eco_params.llFrameDuration;
                cerr << "Input signal resolution: " << eco_params.cx
                     << "x" << eco_params.cy
                     << (interlaced ? 'i' : 'p')
                     << fps << " "
                     << frame_rate.num << "/" << frame_rate.den << "\n";
                cerr << "Time base: " << time_base.num << "/"
                     << time_base.den << "\n";
                if (videoSignalStatus.bSegmentedFrame)
                    cerr << "Input signal frame segmented\n";
                cerr << "========\n";
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
                    m_image_buffers_desired = k_min_video_buffers;
                    for (idx = 0; idx < m_image_buffers_desired; ++idx)
                    {
                        if (!add_eco_image_buffer())
                        {
                            Shutdown();
                            break;
                        }
                    }
                }
            }
            else
            {
                free_image_buffers();
                m_image_buffers_desired = k_min_video_buffers;
                for (idx = 0; idx < m_image_buffers_desired; ++idx)
                {
                    if (!add_pro_image_buffer())
                    {
                        Shutdown();
                        break;
                    }
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
            cerr << lock_ios() << " No changed to input\n";
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
            cerr << lock_ios()
                 << "ERROR: Video: Failed to register notify event." << endl;
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
        cerr << lock_ios() << "Video Capture finished.\n";
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
bool Magewell::Capture(const string & video_codec, const string & preset,
                       int quality, int look_ahead, bool no_audio, bool p010,
                       const string & gpu_device)
{
    m_p010 = p010;

    // Display input information if verbose
    if (m_verbose > 1)
        describe_input(m_channel);

    // Create output handler based on capture mode
    if (m_isEco)
    {
        m_out2ts = new OutputTS(m_verbose, video_codec, preset, quality,
                                look_ahead, p010, gpu_device,
                                [=](void) { this->Shutdown(); },
                                [=](void) { this->Reset(); },
                                [=](uint8_t* ib, void* eb)
                                { this->eco_image_buffer_available(ib, eb); });
    }
    else
    {
        m_out2ts = new OutputTS(m_verbose, video_codec, preset, quality,
                                look_ahead, p010, gpu_device,
                                [=](void) { this->Shutdown(); },
                                [=](void) { this->Reset(); },
                                [=](uint8_t* ib, void* eb)
                                { this->pro_image_buffer_available(ib, eb); });
    }

    // Check if output handler was created successfully
    if (!m_out2ts)
    {
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
    capture_video();

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
            cerr << lock_ios() << "Magewell::Shutdown\n";
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
    chrono::high_resolution_clock::time_point end =
        chrono::high_resolution_clock::now();

    // Rate limit resets to prevent excessive resets
    if (chrono::duration_cast<chrono::microseconds>(end - m_last_reset).count() > 4000)
    {
        if (m_verbose > 0)
            cerr << "Magewell:Reset\n";
        m_reset_audio.store(true);
        m_reset_video.store(true);
        m_last_reset = std::chrono::high_resolution_clock::now();
    }
}
