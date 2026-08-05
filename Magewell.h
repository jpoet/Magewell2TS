#pragma once

#include <chrono>
#include <deque>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include <mutex>
#include <condition_variable>

#include <MWFOURCC.h>
#include <LibMWCapture/MWCapture.h>
#include "LibMWCapture/MWEcoCapture.h"

#include "OutputTS.h"

/**
 * @brief Magewell class for controlling video capture cards using
 * Magewell API
 *
 * This class provides functionality to open channels, capture video and audio,
 * Handle HDR information, and manage video buffers for Magewell
 * capture devices.
 *
 * @author John Patrick Poet
 * @date 2022-2026
 */

class Magewell
{
    // Type definitions
    using imageset_t = std::set<uint8_t*>;     ///< Set of image buffers
    using imageque_t = std::deque<uint8_t*>;   ///< Queue of available image buffers
    using ecoque_t  =
        std::vector<std::unique_ptr<MWCAP_VIDEO_ECO_CAPTURE_FRAME>>;

    enum : uint8_t
    {
        HDMI_EOTF_SDR          = 0,
        HDMI_EOTF_HDR_GAMMA    = 1,
        HDMI_EOTF_ST2084_PQ    = 2,
        HDMI_EOTF_HLG          = 3
    };

  public:
    Magewell(void);
    ~Magewell(void);

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

    bool Capture(VideoStream::Args&& video_args,
                 bool no_audio, std::chrono::milliseconds settle_time,
                 int video_buffers, bool realtime);

    /**
     * @brief Shutdown the capture process
     */
    void Shutdown(void);

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
    std::string describe_input(HCHANNEL channel);

    bool get_colorspace(MWCAP_VIDEO_SIGNAL_STATUS signal_status,
                        VideoStream::ColorSpace& meta);

    size_t   AllocateImageBuffers(void);
    uint8_t* GetFrameImage(size_t frame_idx);

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

    bool create_pro_image_buffers(void);
    bool create_eco_image_buffers(void);
    bool register_eco_image_buffers(void);
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

    void log_stats(size_t used);

    /**
     * @brief Capture video using ECO capture method
     * @param eco_params ECO capture parameters
     * @param eco_event ECO event handle
     * @param video_notify Video notification handle
     * @param ullStatusBits Status bits
     * @param interlaced Whether video is interlaced
     */
    bool capture_eco_video(MWCAP_VIDEO_ECO_CAPTURE_OPEN eco_params,
                           std::optional<VideoStream::Params>&& pParams,
                           int eco_event,
                           HNOTIFY video_notify,
                           ULONGLONG ullStatusBits);

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
                           std::optional<VideoStream::Params>&& pParams,
                           HNOTIFY video_notify,
                           MWCAP_PTR notify_event,
                           MWCAP_PTR capture_event,
                           int       frame_wrap_idx,
                           DWORD     event_mask,
                           ULONGLONG ullStatusBits);

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
    // spdlog
    std::shared_ptr<spdlog::logger> m_log;

    // Capture components
    OutputTS*            m_out2ts  {nullptr};    ///< Output TS handler
    HCHANNEL             m_channel {nullptr};    ///< Channel handle
    MWCAP_CHANNEL_INFO   m_channel_info  {0};    ///< Channel information
    int                  m_channel_idx   {0};    ///< Channel index
    std::chrono::milliseconds m_settle_time   {5000}; ///< signal change timeout

    std::unique_ptr<uint8_t[]> m_image_buffer;
    size_t                     m_aligned_image_size {0};
    bool                       m_pinned            {false};

    size_t       m_image_buffers           {0};
    size_t       m_image_buffers_total     {0}; ///< Total image buffers
    size_t       m_image_buffers_avail     {0}; ///< Available image buffers
    imageque_t   m_avail_image_buffers;         ///< Queue of available buffers
    ecoque_t     m_eco_image_buffers;           ///< Set of ECO buffers
    std::mutex   m_image_buffer_mutex;          ///< Mutex for buffer access
    std::condition_variable m_image_returned;   ///< Condition variable for buffer return

    // Video parameters
    VideoStream::Args        m_video_args;
    VideoStream::EncoderType m_encoderType;
    enum AVPixelFormat       m_pix_fmt;

    int m_image_size         {0};  ///< Image size in bytes
    int m_min_stride         {0};  ///< Minimum stride
    int m_frame_ms           {17}; ///< Video Frame time in milliseconds
    int m_frame_ms2          {34}; ///< Double video frame time
    int m_frame_half_ms      {8};  ///< Half of video frame time

    int     m_frame_cnt      {0};  ///< Number of frames processed
    int64_t m_expected_ts    {-1}; ///< Expected next timestamp

    // Audio thread
    std::thread       m_audio_thread;  ///< Audio capture thread

    // State flags
    std::atomic<bool> m_running     {true};  ///< Running flag

    // Function pointer
    std::function<bool (void)>  f_open_video;  ///< Video open function

    // Device flags
    bool m_isEco   {false};  ///< Whether using ECO capture

    bool m_fatal   {false};  ///< Fatal error flag
    int  m_verbose {1};      ///< Verbose level

#if 0
    FrameRateDetector m_rateDetector;
#endif

    std::chrono::steady_clock::time_point m_start_tm;
};
