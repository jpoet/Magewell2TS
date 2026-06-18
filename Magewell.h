#pragma once

#include <string>
#include <deque>
#include <set>

#include <chrono>
#include <vector>
#include <string>
#include <format>

#include <mutex>
#include <condition_variable>

#include <MWFOURCC.h>
#include <LibMWCapture/MWCapture.h>
#include "LibMWCapture/MWEcoCapture.h"

#include <spdlog/spdlog.h>

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


#include <iostream>
#include <chrono>
#include <vector>
#include <string>
#include <format>

#include <iostream>
#include <chrono>
#include <vector>
#include <string>
#include <format>

#if 0
class SteadyVideo
{
  public:
    struct Stats
    {
        bool isStable;
        std::chrono::milliseconds duration;
        int cnt; // Tracks samples measured in this state instance
    };

    struct TotalSummary
    {
        std::chrono::milliseconds stableDuration;
        std::chrono::milliseconds unstableDuration;
        int stableCnt;   // Added to track total stable samples
        int unstableCnt; // Added to track total unstable samples
    };

    SteadyVideo(std::chrono::milliseconds requiredDuration)
        : m_requiredDuration(requiredDuration),
          m_currentState(true),
          m_startTime(std::chrono::steady_clock::now()),
          m_stateStartTime(m_startTime),
          m_stableDuration(std::chrono::milliseconds(0)),
          m_cnt(0)
    {
    }

    bool AddSample(bool current)
    {
        std::chrono::time_point<std::chrono::steady_clock> now = std::chrono::steady_clock::now();
        std::chrono::milliseconds sampleDuration = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_stateStartTime);

        // Count this new sample for the active state
        m_cnt++;

        if (current == m_currentState)
        {
            if (m_currentState)
            {
                m_stableDuration = sampleDuration;
            }
        }
        else
        {
            // Save the finished state information to history
            Stats historicalStat;
            historicalStat.isStable = m_currentState;
            historicalStat.duration = sampleDuration;
            historicalStat.cnt      = m_cnt;
            m_history.push_back(historicalStat);

            // Reset trackers for the brand new state
            m_currentState = current;
            m_cnt = 0;
            m_stateStartTime = now;
            m_stableDuration = std::chrono::milliseconds(0);
        }

        return m_currentState && (m_stableDuration >= m_requiredDuration);
    }

    // Marked const since it does not modify the class state
    std::vector<Stats> Statistics() const
    {
        std::vector<Stats> currentStats = m_history;
        std::chrono::time_point<std::chrono::steady_clock> now = std::chrono::steady_clock::now();
        std::chrono::milliseconds currentDuration = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_stateStartTime);

        // Include the active ongoing state into the list
        Stats finalStat;
        finalStat.isStable = m_currentState;
        finalStat.duration = currentDuration;
        finalStat.cnt      = m_cnt;
        currentStats.push_back(finalStat);

        return currentStats;
    }

    // Marked const and updated to use modern range-based for loops
    TotalSummary StateDurations() const
    {
        std::vector<Stats> allStats = Statistics();
        TotalSummary summary{}; // Zero-initializes durations and counts

        for (const auto& stat : allStats)
        {
            if (stat.isStable)
            {
                summary.stableDuration += stat.duration;
                summary.stableCnt      += stat.cnt;
            }
            else
            {
                summary.unstableDuration += stat.duration;
                summary.unstableCnt      += stat.cnt;
            }
        }

        return summary;
    }

    // Marked const and updated to display counts in the tables
    std::string StatTable() const
    {
        std::vector<Stats> allStats = Statistics();
        std::string table = "--- Chronological Log ---\n";
        table += std::format("| {:<10} | {:<15} | {:<12} |\n", "State", "Duration (ms)", "Sample Count");
        table += "|------------|-----------------|--------------|\n";

        for (const auto& stat : allStats)
        {
            std::string stateString = stat.isStable ? "Stable" : "Unstable";
            table += std::format("| {:<10} | {:<15} | {:<12} |\n", stateString, stat.duration.count(), stat.cnt);
        }

        TotalSummary summary = StateDurations();
        table += "\n--- Cumulative Totals ---\n";
        table += std::format("| {:<10} | {:<15} | {:<12} |\n", "State", "Total Time (ms)", "Total Samples");
        table += "|------------|-----------------|--------------|\n";
        table += std::format("| {:<10} | {:<15} | {:<12} |\n", "Stable", summary.stableDuration.count(), summary.stableCnt);
        table += std::format("| {:<10} | {:<15} | {:<12} |\n", "Unstable", summary.unstableDuration.count(), summary.unstableCnt);

        return table;
    }

  private:
    std::chrono::milliseconds m_requiredDuration;
    bool m_currentState;
    std::chrono::time_point<std::chrono::steady_clock> m_startTime;
    std::chrono::time_point<std::chrono::steady_clock> m_stateStartTime;
    std::chrono::milliseconds m_stableDuration;
    int  m_cnt {0};
    std::vector<Stats> m_history;
};

class FrameRateDetector
{
  public:

    AVRational update(int64_t frame_duration_units)
    {
        AVRational detected =
            normalize_framerate(frame_duration_units);

        if (av_cmp_q(detected, m_candidate) == 0)
        {
            ++m_stable_count;

            //
            // Require stability before accepting
            //
            if (m_stable_count >= m_required_stability)
            {
                m_current = detected;
            }
        }
        else
        {
            m_candidate    = detected;
            m_stable_count = 1;
        }

        return m_current;
    }

    AVRational current() const
    {
        return m_current;
    }

  private:

    AVRational normalize_framerate(int64_t frame_duration_units) const
    {
        const double measured_fps =
            10000000.0 /
            static_cast<double>(frame_duration_units);

        double best_error =
            std::numeric_limits<double>::max();

        AVRational best {0,1};

        for (const auto& candidate : kCanonicalRates)
        {
            const double error =
                std::abs(measured_fps - candidate.fps);

            if (error < best_error)
            {
                best_error = error;
                best       = candidate.rate;
            }
        }

        return best;
    }

  private:

    struct CanonicalRate
    {
        AVRational rate;
        double     fps;
    };

    static constexpr std::array<CanonicalRate,10>
      kCanonicalRates
    {{
            {{24000,1001}, 23.976023976},
            {{24,1},       24.0},

            {{25,1},       25.0},

            {{30000,1001}, 29.970029970},
            {{30,1},       30.0},

            {{48000,1001}, 47.952047952},
            {{48,1},       48.0},

            {{60000,1001}, 59.940059940},
            {{60,1},       60.0},
        }};

    AVRational m_current  {0,1};
    AVRational m_candidate{0,1};

    int m_stable_count      {0};

    int m_required_stability{30};
};
#endif

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
                 int video_buffers);

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

    bool add_pro_image_buffers(void);
    bool add_eco_image_buffers(void);
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

    std::unique_ptr<uint64_t[]> m_image_buffer;
    size_t                      m_image_size_qwords {0};
    bool                        m_pinned            {false};

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
