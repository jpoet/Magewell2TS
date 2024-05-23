#ifndef _OutputTS_h_
#define _OutputTS_h_

#include <string>
#include <vector>
#include <deque>
#include <utility>
#include <mutex>
#include <condition_variable>
#include <atomic>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

class PktQueue
{
  public:
    PktQueue(int verbose);
    ~PktQueue(void) { ; }

    int Push(uint8_t* buf, size_t len, int64_t timestamp);
    int Pop(uint8_t* dest, size_t len);
    int Seek(int64_t offset, int whence);

    void SetEOL(bool val);
    void WaitEOL(void);

    size_t CalcSize(void);
    size_t Size(void) const { return m_size; }
    size_t Buffered(void) const { return m_queue.size(); }
    bool   Empty(void) const { return m_Icur != m_queue.end(); }
    void   Clear(void);
    int64_t TimeStamp(void) { return m_timestamp; }

    using vec_t    = std::vector<uint8_t>;
    using pkt_t    = struct {
        int64_t timestamp;
        int64_t frame_num;
        vec_t   data;
    };
    using que_t    = std::deque<pkt_t>;

  private:
    que_t                m_queue;
    que_t::iterator      m_Icur;
    size_t               m_size        {0};
    size_t               m_remainder   {0};
    std::mutex           m_mutex;
    int64_t              m_timestamp;


    bool                 m_EOL          {false};
    int64_t              m_EOLtimestamp {-1};
    bool                 m_EOLtriggered {true};
    std::condition_variable m_EOLtrigger;

    int64_t              m_frame_num   {0};

    int                  m_verbose     {1};
};

class OutputTS
{
  public:
    enum EncoderType { UNKNOWN, NV, VAAPI, QSV };

    OutputTS(int verbose, const std::string & video_codec_name,
             int look_ahead, bool no_audio,
             const std::string & device);
    ~OutputTS(void);

    EncoderType encoderType(void) const { return m_encoderType; }
    void setAudioParams(int num_channels, int bytes_per_sample,
                        int samples_per_frame);
    void setVideoParams(int width, int height, bool interlaced,
                        AVRational time_base, AVRational frame_rate);

    bool AudioReady(void);
    void addPacket(uint8_t* buf, int buf_size, int64_t timestamp);
    bool Write(uint8_t*  pImage, uint32_t imageSize, int64_t timestamp);

  private:
    // a wrapper around a single output AVStream
    using OutputStream = struct {
        AVStream* st;
        AVBufferRef*    hw_device_ctx  {nullptr};
        AVCodecContext* enc;

        /* pts of the next frame that will be generated */
        int64_t next_pts;
        int64_t next_timestamp;
        int samples_count;

        AVFrame* frame;
        AVFrame* tmp_frame;
        int64_t  prev_pts        {-1};
        int64_t  prev_dts        {-1};

        AVPacket* tmp_pkt;

        float t, tincr, tincr2;

        struct SwsContext* sws_ctx;
        struct SwrContext* swr_ctx;
    };

    void open_streams(void);

    static std::string AVerr2str(int code);

    bool write_frame(AVFormatContext* fmt_ctx, AVCodecContext* c,
                     AVFrame* frame, OutputStream* ost);
    void add_stream(OutputStream* ost, AVFormatContext* oc,
                    const AVCodec* *codec);
    static void close_stream(AVFormatContext* oc, OutputStream* ost);

    // Audio output
    void detect_audio(void);
    static AVFrame* alloc_audio_frame(enum AVSampleFormat sample_fmt,
                                      const AVChannelLayout* channel_layout,
                                      int sample_rate, int nb_samples);
    bool open_spdif_context(void);
    bool open_spdif(void);
    static void open_audio(AVFormatContext* oc, const AVCodec* codec,
                           OutputStream* ost, AVDictionary* opt_arg);
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
                   uint32_t imageSize, int64_t timestamp);
    bool vaapi_encode(AVFormatContext* oc,
                      OutputStream* ost, uint8_t*  pImage,
                      uint32_t imageSize, int64_t timestamp);
    bool qsv_encode(AVFormatContext* oc,
                    OutputStream* ost, uint8_t*  pImage,
                    uint32_t imageSize, int64_t timestamp);
    bool write_video_frame(AVFormatContext* oc, OutputStream* ost,
                           uint8_t*  pImage, uint32_t imageSize,
                           int64_t timestamp);

    EncoderType     m_encoderType  { UNKNOWN };

    PktQueue         m_packet_queue;

    const AVOutputFormat* fmt   {nullptr};
    AVFormatContext* m_output_format_context {nullptr};
    OutputStream m_video_stream { 0 };
    OutputStream m_audio_stream { 0 };
    int have_video {0};
    int have_audio {0};

    int              m_audio_channels             {-1};
    int              m_audio_bytes_per_sample     {-1};
    int              m_audio_samples_per_frame    {-1};
    int              m_audio_block_size           {-1};
//    int              m_audio_detect_blocks        {3};

    std::thread      m_audio_detect_thread;

    bool             m_bitstream            {false};
    bool             m_init                 {false};
    bool             m_error                {false};
    bool             m_audio_detect         {false};

    AVFormatContext* m_spdif_format_context {nullptr};
    AVIOContext*     m_spdif_avio_context   {nullptr};
    uint8_t*         m_spdif_avio_context_buffer  {nullptr};
    const size_t     m_spdif_avio_context_buffer_size {4096};
    const AVCodec*   m_spdif_codec          {nullptr};
    AVCodecID        m_spdif_codec_id;
    std::string      m_audio_codec_name;
    AVChannelLayout  m_channel_layout;
    bool             m_no_audio             {false};

    std::string      m_video_codec_name        {"hevc_nvenc"};
    std::string      m_device;
//    std::string      m_driver;
    int              m_look_ahead              {-1};
    int              m_input_width             {1280};
    int              m_input_height            {720};
    AVRational       m_input_frame_rate        {10000000, 166817};
    AVRational       m_input_time_base         {1, 10000000};

    bool             m_interlaced             {false};

    int              m_verbose;

    std::condition_variable m_audio_detected;
    std::mutex              m_detect_mutex;
    std::mutex              m_detecting_mutex;

    std::mutex              m_mutex;
};

#endif
