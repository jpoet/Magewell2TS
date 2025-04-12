#ifndef _OutputTS_h_
#define _OutputTS_h_

#include <string>
#include <vector>
#include <deque>
#include <utility>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>

#include "AudioIO.h"

class OutputTS
{
  public:
    using MagCallback = std::function<void (uint8_t*)>;
    using ShutdownCallback = std::function<void (void)>;

    enum EncoderType { UNKNOWN, NV, VAAPI, QSV };

    OutputTS(int verbose, const std::string & video_codec_name,
             const std::string & preset, int quality, int look_ahead,
             bool no_audio, const std::string & device,
             ShutdownCallback shutdown,
             MagCallback image_buffer_avail);
    ~OutputTS(void);

    void Shutdown(bool from_above = false);

    EncoderType encoderType(void) const { return m_encoderType; }
    bool setAudioParams(int num_channels, bool is_lpcm,
                        int bytes_per_sample, int sample_rate,
                        int samples_per_frame, int frame_size);
    bool setVideoParams(int width, int height, bool interlaced,
                        AVRational time_base, double frame_duration,
                        AVRational frame_rate);
    bool addAudio(AudioBuffer::AudioFrame & buf, int64_t timestamp);
    void ClearImageQueue(void);
    void Write(void);
    bool AddVideoFrame(uint8_t*  pImage,
                       uint32_t imageSize, int64_t timestamp);

  private:
    // a wrapper around a single output AVStream
    using OutputStream = struct {
        AVBufferRef* hw_device_ctx {nullptr};
        bool         hw_device     {false};

        AVStream* st               {nullptr};
        AVCodecContext* enc        {nullptr};

        /* pts of the next frame that will be generated */
        int64_t next_pts           {-1};

        int samples_count          {0};

        AVFrame* frame             {nullptr};
        AVFrame* tmp_frame         {nullptr};
        int64_t  prev_pts          {-1};
        int64_t  prev_audio_pts    {-1};
        int64_t  prev_dts          {-1};

        AVPacket* tmp_pkt          {nullptr};

        struct SwrContext* swr_ctx {nullptr};
    };

    using imagepkt_t = struct {
        int64_t  timestamp;
        uint8_t* image;
    };
    using imageque_t = std::deque<imagepkt_t>;
    imageque_t m_imagequeue;

    bool add_stream(OutputStream* ost, AVFormatContext* oc,
                    const AVCodec* *codec);
    static void close_stream(OutputStream* ost);

    bool open_audio(void);
    bool open_video(void);
    bool open_container(void);
    void close_container(void);
    void close_encoder(OutputStream* ost);

    bool write_frame(AVFormatContext* fmt_ctx, AVCodecContext* c,
                     AVFrame* frame, OutputStream* ost);
    // Audio output
    static AVFrame* alloc_audio_frame(enum AVSampleFormat sample_fmt,
                                      const AVChannelLayout* channel_layout,
                                      int sample_rate, int nb_samples);
    AVFrame* get_pcm_audio_frame(OutputStream* ost);

    bool write_pcm_frame(AVFormatContext* oc, OutputStream* ost);
    bool write_bitstream_frame(AVFormatContext* oc, OutputStream* ost);
    bool write_audio_frame(AVFormatContext* oc, OutputStream* ost);

    // Video output
    static AVFrame* alloc_picture(enum AVPixelFormat pix_fmt,
                                  int width, int height);
    bool open_nvidia(const AVCodec* codec, OutputStream* ost,
                     AVDictionary* opt_arg);
    bool open_vaapi(const AVCodec* codec, OutputStream* ost,
                    AVDictionary* opt_arg);
    bool open_qsv(const AVCodec* codec, OutputStream* ost,
                  AVDictionary* opt_arg);
    bool nv_encode(AVFormatContext* oc,
                   OutputStream* ost, uint8_t* pImage,
                   int64_t timestamp);
    bool qsv_vaapi_encode(AVFormatContext* oc,
                      OutputStream* ost, uint8_t*  pImage,
                      int64_t timestamp);

    EncoderType     m_encoderType  { UNKNOWN };

    AudioIO*        m_audioIO;

    const AVOutputFormat* m_fmt   {nullptr};
    AVFormatContext* m_output_format_context {nullptr};
    OutputStream m_video_stream { 0 };
    OutputStream m_audio_stream { 0 };

    int              m_verbose;

    std::string      m_filename               {"pipe:1"};

    bool             m_no_audio               {false};
    int              m_slow_audio_cnt         {0};

    std::string      m_video_codec_name;
    std::string      m_device;
    std::string      m_preset;
    int              m_quality                {-1};
    int              m_look_ahead             {-1};
    int              m_input_width            {1280};
    int              m_input_height           {720};
    double           m_input_frame_duration   {0};
    int              m_input_frame_wait_ms    {17};
    AVRational       m_input_frame_rate       {10000000, 166817};
    AVRational       m_input_time_base        {1, 10000000};

    bool             m_interlaced             {false};

    std::mutex              m_container_mutex;

    ShutdownCallback        f_shutdown;
    MagCallback             f_image_buffer_available;
    std::thread             m_image_ready_thread;
    std::mutex              m_imagepkt_mutex;
    std::mutex              m_imagequeue_mutex;
    std::condition_variable m_image_ready;
    std::condition_variable m_image_queue_empty;

    std::atomic<bool>       m_running      {true};
    std::atomic<bool>       m_init_needed  {true};
    std::mutex              m_ready_mutex;
    std::condition_variable m_ready_cond;
};

#endif
