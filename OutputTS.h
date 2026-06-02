#pragma once

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <utility>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>

#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>

#include <memory>
class BitstreamAudioParser; // Lightweight forward declaration


#include "MediaQueue.h"

#include "ffmpeg_types.h"
extern "C" {
// FFmpeg structure for HDR
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/audio_fifo.h>
}

namespace TimeBase
{
inline constexpr AVRational MPEG_TS  {1, 90000};
inline constexpr AVRational Magewell {1, 10000000};
inline constexpr AVRational AUDIO48  {1, 48000};
}

class OutputTS
{
  public:
    using MagCallback = std::function<void (uint8_t*, void*)>;
    using ShutdownCallback = std::function<void (void)>;

    enum EncoderType { UNKNOWN, NV, VAAPI, QSV };

    const int VIDEO_STREAM_ID = 0;
    const int AUDIO_STREAM_ID = 1;

    OutputTS(int verbose, const std::string & video_codec_name,
             const std::string & preset, int quality, int look_ahead,
             bool p010, bool isEco, const std::string & device,
             int extra_hw_frames, float gop_secs,
             ShutdownCallback shutdown,
             MagCallback image_buffer_avail);
    ~OutputTS(void);

    void Shutdown(void);

    void log_packet(std::string where, const AVFormatContext* fmt_ctx,
                    const AVPacket* pkt);

    AVColorSpace getColorSpace(void) const { return m_color_space; }
    AVColorTransferCharacteristic getColorTRC(void) const
                                                   { return m_color_trc; }
    AVColorPrimaries getColorPrimaries(void) const
                                             { return m_color_primaries; }

    void setColorSpace(AVColorSpace c) { m_color_space = c; }
    void setColorTRC(AVColorTransferCharacteristic c) { m_color_trc = c; }
    void setColorPrimaries(AVColorPrimaries c) { m_color_primaries = c; }
    std::string ColorSpaceDesc(void) const;
    bool isHDR(void) const { return m_isHDR; }

    void setLight(AVMasteringDisplayMetadata * display_meta,
                  AVContentLightMetadata * light_meta);

    EncoderType encoderType(void) const { return m_encoderType; }

    struct Packet
    {
        // True when signaling an encoder/stream swap
        bool                is_marker         {false};
        int64_t             dts               {0};
        // Snapshot of the encoder's time base at production time
        AVRational          time_base {0, 1};

        PacketPtr      pkt;
        CodecParamsPtr codec_par;
    };

    struct AudioParams
    {
        int  num_channels    {0};
        bool is_lpcm         {false};
        int  sample_rate     {0};
        int  bits_per_sample {0};
        int  samples_size    {0};

        bool operator==(const AudioParams&) const = default;
    };

    using samples_t = std::vector<uint8_t>;
    struct AudioSamples
    {
        samples_t   data;
        int64_t     timestamp   {-1};
        int         sample_rate {0};
        std::optional<AudioParams> pParams;
    };
    using audioque_t = std::deque<AudioSamples>;

    struct VideoParams
    {
        int             width          {0};
        int             height         {0};

        AVPixelFormat   pix_fmt        {AV_PIX_FMT_NONE};

        int64_t         frame_duration {0};

        bool            is_HDR         {false};

        AVRational      time_base      {0, 1};
        AVRational      frame_rate     {0, 1};

        bool operator==(const VideoParams&) const = default;
    };

    struct VideoImage
    {
        uint8_t*      pImage     {nullptr};
        int           imageSize  {0};
        int64_t       timestamp  {-1};
        void*         pEco       {nullptr};
        std::optional<VideoParams> pParams;
    };
    using imageque_t = std::deque<VideoImage>;

    void setHaveAudio(int samples_per_frame)
        { m_no_audio = false; m_samples_per_frame = samples_per_frame; }
    void AddAudioSamples(AudioSamples&& audio);
    void AddVideoFrame(VideoImage&& image);

  private:
    void process_video(void);

    void process_audio(void);
    bool configure_lpcm(const AudioSamples& audio);

    void initialize_bitstream_parser(void);
    void handle_incoming_samples(const uint8_t* data, size_t size,
                                 uint64_t timestamp);

    bool configure_bitstream(CodecParamsPtr codecpar);
    void process_lpcm(const AudioSamples& audio);
    void encode_ac3_frame(void);
    void process_bitstream(AudioSamples&& audio);
    void flush_audio_pipeline(void);

    bool stream_changed(const AVCodecParameters* current_params,
                        const AVCodecParameters* new_params) const;
    void mux(void);

    // spdlog
    std::shared_ptr<spdlog::logger> m_log;

    // a wrapper around a single output AVStream
    using OutputStream = struct {
        AVBufferRef* hw_device_ctx {nullptr};
        AVBufferRef* hw_frames_ctx {nullptr};
        bool         hw_device     {false};

        AVStream*          st       {nullptr};
        AVCodecContext*    enc      {nullptr};

        int frames_written         {0};
        int samples_count          {0};
    };

    MediaQueue<Packet> m_videoPktQ;
    MediaQueue<Packet> m_audioPktQ;

    imageque_t m_imageQ;
    audioque_t m_audioQ;

    bool rebuild_qsv_frames_context(AVBufferRef* device_ctx_ref,
                                    const VideoParams& params);
    bool open_video(const VideoParams& params);
    bool open_audio_encoder(CodecParamsPtr& codecpar);
    void close_encoder(OutputStream* ost);

    bool open_container(AVCodecParameters* video_params,
                        AVRational video_time_base,
                        AVCodecParameters* audio_params,
                        AVRational audio_time_base);
    void close_container(void);

    bool open_nvidia(const AVCodec* codec, AVDictionary** opt_arg);
    bool open_vaapi(const AVCodec* codec, AVDictionary** opt_arg);
    bool open_qsv(const AVCodec* codec, AVDictionary** opt_arg);

    bool queue_packets(OutputStream* ost,
                       MediaQueue<Packet>& pktQ, bool is_flushing);
    bool encode_frame(OutputStream* ost, AVFrame* frame,
                      MediaQueue<Packet>& pktQ);
    bool flush_packets(OutputStream* ost, MediaQueue<Packet>& pktQ);

    EncoderType     m_encoderType  { UNKNOWN };

    AVFormatContext* m_formatContext {nullptr};
    OutputStream     m_videoStream { 0 };
    OutputStream     m_audioStream { 0 };

    int              m_verbose;

    bool             m_no_audio      {true};
    int              m_samples_per_frame {0};
    int64_t          m_audio_total_samples {0};
    bool             m_bitstream     {false};
    AudioParams      m_audio_params;

    AVAudioFifo*     m_audio_fifo    { nullptr };

    std::string      m_video_codec_name;
    std::string      m_device;
    std::string      m_preset;
    int              m_quality                {-1};
    int              m_look_ahead             {-1};
    float            m_gop_secs               {1.5};
    int              m_input_width            {1280};
    int              m_input_height           {720};
    double           m_input_frame_duration   {0};
    int              m_input_frame_wait_ms    {17};
    AVRational       m_input_frame_rate       {10000000, 166817};
    AVRational       m_input_time_base        {1, 10000000};

    bool             m_interlaced             {false};

    enum AVPixelFormat            m_sw_pix_fmt        {AV_PIX_FMT_NV12};
    bool                          m_p010              {false};
    bool                          m_isEco             {false};
    int                           m_extra_hw_frames   {-1};
    int                           m_gpu_buffers       {-1};
    int                           m_frame_buffers     {10};

    // HDR
    bool                          m_isHDR             {false};
    AVColorSpace                  m_color_space       {AVCOL_SPC_NB};
    AVColorTransferCharacteristic m_color_trc         {AVCOL_TRC_NB};
    AVColorPrimaries              m_color_primaries   {AVCOL_PRI_NB};
    AVMasteringDisplayMetadata*   m_display_primaries {nullptr};
    AVContentLightMetadata*       m_content_light     {nullptr};

    std::mutex              m_container_mutex;

    ShutdownCallback        f_shutdown;
    MagCallback             f_image_buffer_available;
    std::thread             m_mux_thread;
    std::thread             m_video_thread;
    std::thread             m_audio_thread;

    std::mutex              m_pktQ_mutex;
    std::condition_variable m_pktQ_ready;

    std::mutex              m_imageQ_mutex;
    std::condition_variable m_imageQ_ready;
    std::condition_variable m_imageQ_empty;


    std::unique_ptr<BitstreamAudioParser> m_bitstream_parser;

    std::mutex              m_audioQ_mutex;
    std::condition_variable m_audioQ_ready;
    std::condition_variable m_audioQ_empty;

    std::atomic<bool>       m_running      {true};
};

template<>
struct fmt::formatter<OutputTS::AudioParams>
{
    constexpr auto parse(format_parse_context& ctx)
    {
        return ctx.begin();
    }

    template<typename FormatContext>
    auto format(const OutputTS::AudioParams& p,
                FormatContext& ctx) const
    {
        return fmt::format_to(
            ctx.out(),
            "AudioParams[ch={}, pcm={}, rate={}Hz, bits={}, bytes={}]",
            p.num_channels,
            p.is_lpcm ? "LPCM" : "Compressed",
            p.sample_rate,
            p.bits_per_sample,
            p.samples_size);
    }
};
