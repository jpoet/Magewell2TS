#ifndef _OutputTS_h_
#define _OutputTS_h_

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include <string>
#include <vector>
#include <deque>
#include <utility>
#include <mutex>
#include <condition_variable>
#include <atomic>

class AudioIO
{
  public:
    AudioIO(int verbose = 0);
    ~AudioIO(void) { ; }

    void AddBuffer(uint8_t* begin, uint8_t* end,
                   int frame_size, bool lpcm,
                   int64_t* timestamps, size_t frame_count);

    int Add(uint8_t* Pframe, size_t len, int64_t timestamp);
    int Read(uint8_t* dest, size_t len);
    int64_t Seek(int64_t offset, int whence);
    void Rewind(void);

    bool Bitstream(void);

    size_t Buffers(void) const { return m_buffer_q.size(); }
    size_t Size(void) const;
    bool   Empty(void) const;
    int64_t TimeStamp(void) const;
    std::string CodecName(void) const;
    void setCodecName(const std::string & rhs);

  private:
    class buffer_t
    {
      public:
        buffer_t(int frame_sz, uint8_t* Pbegin,
                 uint8_t* Pend, bool is_lpcm,
                 int64_t* timestamps, size_t frame_count)
            : frame_size(frame_sz)
            , begin(Pbegin)
            , end(Pend)
            , write(Pbegin)
            , read(Pbegin)
            , lpcm(is_lpcm)
            , m_timestamps(timestamps)
            , m_frame_cnt(frame_count)
        {
            codec_name = lpcm ? "ac3" : "unknown";
#if 0
            std::cerr << "BUFFER " << (lpcm ? "LPCM" : "bistream")
                      << " " << codec_name << std::endl;
#endif
        }
        buffer_t(const buffer_t & rhs)
        { *this = rhs; }
        buffer_t& operator=(const buffer_t & rhs);
        bool operator==(const buffer_t & rhs);

        ~buffer_t(void)
        {
            if (m_own_buffer)
            {
//                std::cerr << "Deleting[] " << (uint64_t)begin << std::endl;
                delete[] begin;
                delete[] m_timestamps;
            }
        }

        bool Empty(void) const
        { return read == write; }

        int64_t TimeStamp(uint8_t* P) const;

        int      frame_size    {-1};
        uint8_t* begin         {nullptr};
        uint8_t* end           {nullptr};
        uint8_t* write         {nullptr};
        uint8_t* read          {nullptr};
        uint8_t* prev_frame    {nullptr};
        bool     lpcm          {true};
        bool     write_wrapped {false};
        std::string codec_name;

        int64_t* m_timestamps  {nullptr};
        size_t   m_frame_cnt   {0};
        int64_t  m_timestamp   {0LL};
        bool     m_own_buffer  {false};
    };

    using buffer_que_t = std::deque<buffer_t>;

    void    print_pointers(const buffer_t & buffer) const;

    buffer_que_t         m_buffer_q;
    std::string          m_codec_name;

    std::mutex           m_mutex;

    int                  m_verbose     {1};
};

class OutputTS
{
  public:
    enum EncoderType { UNKNOWN, NV, VAAPI, QSV };

    OutputTS(int verbose, const std::string & video_codec_name,
             const std::string & preset, int quality, int look_ahead,
             bool no_audio, const std::string & device);
    ~OutputTS(void);

    EncoderType encoderType(void) const { return m_encoderType; }
    void setAudioParams(int num_channels, bool is_lpcm,
                        int bytes_per_sample, int sample_rate,
                        int samples_per_frame, int frame_size,
                        uint8_t* capture_buf, size_t capture_buf_size,
                        int64_t* timestamps, size_t frame_count);
    void setVideoParams(int width, int height, bool interlaced,
                        AVRational time_base, AVRational frame_rate);
    void addAudio(uint8_t* buf, size_t len, int64_t timestamp);
    bool Write(uint8_t*  pImage, uint32_t imageSize, int64_t timestamp);

  private:
    // a wrapper around a single output AVStream
    using OutputStream = struct {
        AVStream* st;
        AVBufferRef*    hw_device_ctx  {nullptr};
        AVCodecContext* enc;

        /* pts of the next frame that will be generated */
        int64_t next_pts;
        int samples_count;

        AVFrame* frame;
        AVFrame* tmp_frame;
        int64_t  prev_pts        {-1};
        int64_t  prev_dts        {-1};

        AVPacket* tmp_pkt;

        struct SwrContext* swr_ctx;
    };

    void add_stream(OutputStream* ost, AVFormatContext* oc,
                    const AVCodec* *codec);
    static void close_stream(AVFormatContext* oc, OutputStream* ost);

    bool open_spdif_context(void);
    bool open_spdif(void);

    bool open_audio(void);
    bool open_video(void);
    bool open_container(void);

    static std::string AVerr2str(int code);

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
                   uint32_t imageSize, int64_t timestamp);
    bool qsv_vaapi_encode(AVFormatContext* oc,
                      OutputStream* ost, uint8_t*  pImage,
                      uint32_t imageSize, int64_t timestamp);
    bool write_video_frame(AVFormatContext* oc, OutputStream* ost,
                           uint8_t* pImage, uint32_t imageSize,
                           int64_t timestamp);

    bool Bitstream(void)
    { return m_audioIO.Bitstream(); }

    EncoderType     m_encoderType  { UNKNOWN };

    AudioIO         m_audioIO;

    const AVOutputFormat* m_fmt   {nullptr};
    AVFormatContext* m_output_format_context {nullptr};
    OutputStream m_video_stream { 0 };
    OutputStream m_audio_stream { 0 };
    int have_video {0};
    int have_audio {0};

    std::string      m_filename                   {"pipe:1"};
    std::string      m_audio_codec_name;
    int              m_audio_channels             {-1};
    int              m_audio_bytes_per_sample     {-1};
    int              m_audio_frame_size           {-1};
    int              m_audio_samples_per_frame    {-1};
    int              m_audio_sample_rate          {-1};
    int              m_audio_block_size           {-1};

    bool             m_error                {false};

    AVFormatContext* m_spdif_format_context {nullptr};
    AVIOContext*     m_spdif_avio_context   {nullptr};
    uint8_t*         m_spdif_avio_context_buffer  {nullptr};
    const size_t     m_spdif_avio_context_buffer_size {4096};
    const AVCodec*   m_spdif_codec          {nullptr};
    AVCodecID        m_spdif_codec_id;
    AVChannelLayout  m_channel_layout;
    bool             m_no_audio             {false};

    std::string      m_video_codec_name;
    std::string      m_device;
    std::string      m_preset;
    int              m_quality                 {-1};
    int              m_look_ahead              {-1};
    int              m_input_width             {1280};
    int              m_input_height            {720};
    AVRational       m_input_frame_rate        {10000000, 166817};
    AVRational       m_input_time_base         {1, 10000000};

    bool             m_interlaced             {false};

    int              m_verbose;

    std::mutex              m_mutex;
};

#endif
