#pragma once

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

#include <string>
#include <utility>
#include <optional>
#include <functional>
#include <deque>

#include <variant>
#include <utility>

#include <spdlog/spdlog.h>
#ifdef SPDLOG_FMT_EXTERNAL
#include <fmt/format.h>
#else
#include <spdlog/fmt/bundled/format.h>
#endif

extern "C" {
// FFmpeg structure for HDR
#include <libavutil/pixdesc.h>
#include <libavutil/mastering_display_metadata.h>
}

#include "MediaQueue.h"
#include "ffmpeg_types.h"

class OutputTS;

class VideoStream
{
  public:
    using MagCallback = std::function<void (uint8_t*, void*)>;

    enum EncoderType { UNKNOWN, NV, VAAPI, QSV };

    struct ColorSpace
    {
        AVRational display_primaries[3][2] {};
        AVRational white_point[2] {};
        AVRational max_luminance{};
        AVRational min_luminance{};

        AVColorRange range { AVCOL_RANGE_UNSPECIFIED };
        AVColorSpace space { AVCOL_SPC_UNSPECIFIED };
        AVColorTransferCharacteristic trc { AVCOL_TRC_UNSPECIFIED };
        AVColorPrimaries primaries { AVCOL_PRI_UNSPECIFIED };
        unsigned MaxCLL {0};
        unsigned MaxFALL {0};

        uint8_t EOTF {0};
        bool is_valid {false};
        bool is_HDR {false};
        bool has_primaries {false};
        bool has_luminance {false};

        std::string description;

        bool operator==(const ColorSpace&) const = default;
    };

    struct Args
    {
        std::string device { "renderD128" };
        std::string codecName { "hevc_qsv" };
        std::string preset { };
        int quality       { 25 };
        int lookahead     { 35 };
        int buffers       { 4 };
        int extraHWframes { 32 };
        float gopSecs     { 1.5 };
        int idrInterval   {  0  };
        bool p010         { false };
    };

    struct Params
    {
        ColorSpace color;

        EncoderType encoder_type {EncoderType::UNKNOWN};
        AVPixelFormat pix_fmt {AV_PIX_FMT_NONE};
        AVRational time_base {0, 1};
        AVRational frame_duration {1, 0};
        int width {0};
        int height {0};
        int num_pixels {0};

        bool operator==(const Params&) const = default;
    };

    struct Image
    {
        uint8_t* pImage {nullptr};
        int imageSize {0};
        int64_t timestamp {-1};
        void* pEco {nullptr};
        std::optional<Params> oParams;
    };
    using imageque_t = std::deque<Image>;

    using hw_frame_t = std::deque<FramePtr>;

    VideoStream(OutputTS& parent, int verbose_level, Args& args,
                Params&& params, MagCallback image_buffer_avail,
                int64_t timestamp);
    ~VideoStream(void);

    VideoStream(const VideoStream&) = delete;
    VideoStream& operator=(const VideoStream&) = delete;

    VideoStream(VideoStream&&) = default;
    VideoStream& operator=(VideoStream&&) = default;

    void Shutdown(void);

    int AddImage(Image&& image);

    std::string ColorSpaceDesc(void) const
        { return m_params.color.description; }

  private:
    bool open_video(void);
    void close_video(void);
    bool open_nvidia(const AVCodec* codec, AVDictionary** opt_arg);
    bool open_vaapi(const AVCodec* codec, AVDictionary** opt_arg);
    bool open_qsv(const AVCodec* codec, AVDictionary** opt_arg);

    void start_encoder(void);
    void stop_encoder(void);
    bool encode_frames(void);
    void prepare_frames(void);
    int add_image_error_cleanup(Image&& image, FramePtr&& hw);

    void set_light(const ColorSpace& color);

    OutputTS& m_parent;
    int m_verbose;
    int m_version {-1};

    // spdlog
    std::shared_ptr<spdlog::logger> m_log;

    EncoderType m_encoderType { UNKNOWN };

    int m_frame_cnt {0};
    SharedCodecContextPtr m_encoder;

    BufferRefPtr m_hw_device_ctx;
    BufferRefPtr m_hw_frames_ctx;

    Args m_args;
    Params m_params;

    enum AVPixelFormat m_sw_pix_fmt {AV_PIX_FMT_NV12};

    hw_frame_t m_empty_shells;   // Wiped containers waiting for GPU mapping
    hw_frame_t m_preped_frames;
    hw_frame_t m_active_frames;

    // HDR
    MasteringDisplayMetadataPtr m_display_primaries;
    ContentLightMetadataPtr m_content_light;

    MagCallback f_image_avail;

    std::mutex m_empty_shell_mutex;
    mutable std::mutex m_active_frame_mutex;
    std::mutex m_preped_frame_mutex;

    std::condition_variable m_shell_avail;
    std::condition_variable m_preped_avail;
    std::condition_variable m_active_avail;

    mutable std::mutex      m_queue_mutex;  // Protects m_active_frames

    // Thread management
    std::mutex m_hwframe_mutex;    // Controls access to m_preped_frames

    std::thread       m_prepare_thread;
    std::thread       m_encode_thread;
    std::atomic<bool> m_running      {false};

#ifdef LOG_ELAPSED
    uint64_t m_total_transfer_time_us = 0;
    uint64_t m_total_buf_wait_time_us  = 0;
    uint32_t m_frame_counter           = 0;
#endif
};

// Custom format specification for spdlog / libfmt
template <>
  struct fmt::formatter<VideoStream::Params>
{
    constexpr auto parse(format_parse_context& ctx) -> decltype(ctx.begin())
    {
        return ctx.begin();
    }

    template <typename FormatContext>
      auto format(const VideoStream::Params& params,
                  FormatContext& ctx) const -> decltype(ctx.out())
    {
        // Prevent division by zero if frame_duration isn't set yet
        double fps = static_cast<double>(params.frame_duration.den) /
                     static_cast<double>(params.frame_duration.num);

        std::string color = "Unknown";
        switch (params.color.space)
        {
            case AVCOL_SPC_BT470BG:
              color = "YUV601";
              break;
            case AVCOL_SPC_BT709:
              color = "YUV709";
              break;
            case AVCOL_SPC_BT2020_NCL:
              color = "YUV2020";
              break;
            default:
              break;
        }

        return fmt::format_to(ctx.out(),
                              "Video[{}x{}p{:.4f} {} {} FR:{}/{} {}]",
                              params.width,
                              params.height,
                              fps,
                              color,
                              av_get_pix_fmt_name(static_cast<AVPixelFormat>(params.pix_fmt)),
                              params.frame_duration.den,
                              params.frame_duration.num,
                              params.color.description);
    }
};
