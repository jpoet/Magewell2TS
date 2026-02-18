#ifndef Magewell_h_
#define Magewell_h_

#include <string>
#include <deque>
#include <set>

#include <mutex>
#include <condition_variable>

#include <MWFOURCC.h>
#include <LibMWCapture/MWCapture.h>
#include "LibMWCapture/MWEcoCapture.h"

#include "OutputTS.h"

/**
 * @brief Magewell class for controlling video capture cards using Magewell API
 *
 * This class provides functionality to open channels, capture video and audio,
 * handle HDR information, and manage video buffers for Magewell capture devices.
 *
 * @author John Patrick Poet
 * @date 2022-2025
 */
class Magewell
{
    // Constants
    static const int k_min_video_buffers = 6;  ///< Minimum number of video buffers to maintain

    // Type definitions
    using imageset_t = std::set<uint8_t*>;     ///< Set of image buffers
    using imageque_t = std::deque<uint8_t*>;   ///< Queue of available image buffers
    using ecoque_t   = std::set<MWCAP_VIDEO_ECO_CAPTURE_FRAME*>; ///< Set of eco capture frames

public:
    /**
     * @brief Constructor for Magewell class
     *
     * Initializes the MWCapture library instance. If initialization fails,
     * sets fatal error flag.
     */
    Magewell(void);

    /**
     * @brief Destructor for Magewell class
     *
     * Cleans up resources by closing the channel and exiting the MWCapture instance.
     */
    ~Magewell(void);

    /**
     * @brief Set verbose level for logging
     * @param v Verbose level (0-3)
     */
    void Verbose(int v) { m_verbose = v; }

    /**
     * @brief Open a video capture channel
     * @param idx Channel index to open
     * @param boardId Board identifier (use -1 for default)
     * @return true if successful, false otherwise
     */
    bool OpenChannel(int idx, double boardId);

    /**
     * @brief Close the currently open channel
     * @return true always
     */
    bool CloseChannel(void);

    /**
     * @brief List all available input channels
     */
    void ListInputs(void);

    /**
     * @brief Wait for a specific number of input channels to be available
     * @param cnt Number of channels to wait for
     * @return true if channels are available, false otherwise
     */
    bool WaitForInputs(int cnt) const;

    /**
     * @brief Display current audio volume settings
     */
    void DisplayVolume(void);

    /**
     * @brief Set audio volume level
     * @param volume_level Volume level (0-100)
     * @return true always
     */
    bool SetVolume(int volume_level);

    /**
     * @brief Read EDID information from the device
     * @param filepath Path to save EDID data
     * @return true if successful, false otherwise
     */
    bool ReadEDID(const std::string & filepath);

    /**
     * @brief Write EDID information to the device
     * @param filepath Path to EDID data file
     * @return true if successful, false otherwise
     */
    bool WriteEDID(const std::string & filepath);

    /**
     * @brief Start video capture with specified parameters
     * @param video_codec Video codec to use
     * @param preset Encoding preset
     * @param quality Quality setting
     * @param look_ahead Look ahead setting
     * @param no_audio Whether to disable audio capture
     * @param p010 Whether to use P010 format
     * @param gpu_device GPU device to use
     * @return true if successful, false otherwise
     */
    bool Capture(const std::string & video_codec, const std::string & preset,
                 int quality, int look_ahead, bool no_audio, bool p010,
                 const std::string & gpu_device);

    /**
     * @brief Shutdown the capture process
     */
    void Shutdown(void);

    /**
     * @brief Reset the capture process
     */
    void Reset(void);

    /**
     * @brief Check if fatal error occurred
     * @return true if fatal error, false otherwise
     */
    bool operator! (void) { return m_fatal; }

private:
    /**
     * @brief Describe input channel information
     * @param channel Handle to the channel
     * @return true if successful, false otherwise
     */
    bool describe_input(HCHANNEL channel);

    /**
     * @brief Update HDR color space information
     * @param signal_status Video signal status
     * @return true if color space changed, false otherwise
     */
    bool update_HDRcolorspace(MWCAP_VIDEO_SIGNAL_STATUS signal_status);

    /**
     * @brief Update HDR frame information
     * @return true if successful, false otherwise
     */
    bool update_HDRframe(void);

    /**
     * @brief Update HDR information from info frames
     * @return true if successful, false otherwise
     */
    bool update_HDRinfo(void);

    /**
     * @brief Handle available image buffer for PRO capture
     * @param pbImage Pointer to image buffer
     * @param buf Context buffer
     */
    void pro_image_buffer_available(uint8_t* pbImage, void* buf);

    /**
     * @brief Handle available image buffer for ECO capture
     * @param pbImage Pointer to image buffer
     * @param buf Context buffer
     */
    void eco_image_buffer_available(uint8_t* pbImage, void* buf);

    /**
     * @brief Add a new PRO image buffer
     * @return true if successful, false otherwise
     */
    bool add_pro_image_buffer(void);

    /**
     * @brief Add a new ECO image buffer
     * @return true if successful, false otherwise
     */
    bool add_eco_image_buffer(void);

    /**
     * @brief Free all image buffers
     */
    void free_image_buffers(void);

    /**
     * @brief Set up notification for channel events
     * @param notify Reference to notification handle
     * @param hChannel Channel handle
     * @param hNotifyEvent Event handle
     * @param flags Notification flags
     */
    void set_notify(HNOTIFY&  notify,
                    HCHANNEL  hChannel,
                    MWCAP_PTR hNotifyEvent,
                    DWORD     flags);

    /**
     * @brief Open ECO video capture
     * @param eco_params ECO capture parameters
     * @return true if successful, false otherwise
     */
    bool open_eco_video(MWCAP_VIDEO_ECO_CAPTURE_OPEN & eco_params);

    /**
     * @brief Close ECO video capture
     */
    void close_eco_video(void);

    /**
     * @brief Capture video using ECO capture method
     * @param eco_params ECO capture parameters
     * @param eco_event ECO event handle
     * @param video_notify Video notification handle
     * @param ullStatusBits Status bits
     * @param interlaced Whether video is interlaced
     */
    bool capture_eco_video(MWCAP_VIDEO_ECO_CAPTURE_OPEN eco_params,
                           int eco_event,
                           HNOTIFY video_notify,
                           ULONGLONG ullStatusBits,
                           bool interlaced);

    /**
     * @brief Capture video using PRO capture method
     * @param eco_params ECO capture parameters
     * @param video_notify Video notification handle
     * @param notify_event Notification event handle
     * @param capture_event Capture event handle
     * @param frame_wrap_idx Frame wrap index
     * @param event_mask Event mask
     * @param ullStatusBits Status bits
     * @param interlaced Whether video is interlaced
     */
    bool capture_pro_video(MWCAP_VIDEO_ECO_CAPTURE_OPEN eco_params,
                           HNOTIFY video_notify,
                           MWCAP_PTR notify_event,
                           MWCAP_PTR capture_event,
                           int       frame_wrap_idx,
                           DWORD     event_mask,
                           ULONGLONG ullStatusBits,
                           bool interlaced);

    /**
     * @brief Main video capture loop
     * @return true always
     */
    bool capture_video(void);

    /**
     * @brief Capture audio data
     */
    void capture_audio_loop(void);
    void capture_audio(void);

private:
    // Audio buffer parameters
    int m_audio_buf_frames {12288};  ///< Audio buffer frames

    // Capture components
    OutputTS*            m_out2ts  {nullptr};  ///< Output TS handler
    HCHANNEL             m_channel {nullptr};   ///< Channel handle
    MWCAP_CHANNEL_INFO   m_channel_info  {0};  ///< Channel information
    int                  m_channel_idx   {0};  ///< Channel index

    // HDR information
    HDMI_INFOFRAME_PACKET m_infoPacket      {0};  ///< Info packet
    HDMI_HDR_INFOFRAME_PAYLOAD& m_HDRinfo {m_infoPacket.hdrInfoFramePayload};  ///< HDR info
    HDMI_INFOFRAME_PACKET m_infoPacket_prev {0};  ///< Previous info packet
    HDMI_HDR_INFOFRAME_PAYLOAD& m_HDRinfo_prev {m_infoPacket_prev.hdrInfoFramePayload};  ///< Previous HDR info

    // Buffer management
    size_t       m_image_buffer_total     {0};  ///< Total image buffers
    size_t       m_image_buffer_avail     {0};  ///< Available image buffers
    size_t       m_image_buffers_desired  {6};  ///< Desired image buffers
    size_t       m_image_buffers_inflight {0};  ///< Buffers in flight
    imageset_t   m_image_buffers;               ///< Set of image buffers
    imageque_t   m_avail_image_buffers;         ///< Queue of available buffers
    ecoque_t     m_eco_buffers;                 ///< Set of ECO buffers
    std::mutex   m_image_buffer_mutex;          ///< Mutex for buffer access
    std::condition_variable m_image_returned;   ///< Condition variable for buffer return

    // Video parameters
    int m_num_pixels         {0};  ///< Number of pixels
    int m_image_size         {0};  ///< Image size in bytes
    int m_min_stride         {0};  ///< Minimum stride
    int m_frame_ms           {17};  ///< Frame time in milliseconds
    int m_frame_ms2          {34};  ///< Double frame time

    // Audio thread
    std::thread       m_audio_thread;  ///< Audio capture thread

    // State flags
    std::atomic<bool> m_running     {true};  ///< Running flag
    std::atomic<bool> m_reset_audio {true};  ///< Audio reset flag
    std::atomic<bool> m_reset_video {true};  ///< Video reset flag
    std::chrono::high_resolution_clock::time_point m_last_reset;  ///< Last reset time

    // Function pointer
    std::function<bool (void)>  f_open_video;  ///< Video open function

    // Device flags
    bool m_isEco   {false};  ///< Whether using ECO capture
    bool m_isHDR   {false};  ///< Whether HDR is active
    bool m_p010    {false};  ///< Whether P010 format is used
    bool m_fatal   {false};  ///< Fatal error flag
    int  m_verbose {1};      ///< Verbose level
};

#endif
