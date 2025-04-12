#ifndef _AudioIO_h_
#define _AudioIO_h_

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include <string>
#include <deque>
#include <vector>
#include <utility>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

class AudioIO;

class AudioBuffer
{
  public:
    using AudioFrame = std::vector<uint8_t>;

    AudioBuffer(int num_channels, bool is_lpcm,
                int bytes_per_sample, int sample_rate,
                int samples_per_frame, int frame_size,
                AudioIO* parent, int verbose, int id);
    AudioBuffer(const AudioBuffer & rhs) { *this = rhs; }
    ~AudioBuffer(void);
    void Clear(void);

    bool RescanSPDIF(void);
    void OwnBuffer(void);
    void setEoF(void) { m_EoF.store(true); }
    bool isEoF(void) const { return m_EoF.load() == true; }
    void PrintState(const std::string & where,
                    bool force = false) const;
    void PrintPointers(const std::string & where,
                       bool force = false) const;

    AudioBuffer& operator=(const AudioBuffer & rhs);
    bool operator==(const AudioBuffer & rhs);

    bool Add(AudioFrame & buf, int64_t timestamp);
    int Read(uint8_t* dest, uint32_t len);
    AVPacket* ReadSPDIF(void);

    int  Id(void) const { return m_id; }
    bool Empty(void) const { return m_audio_queue.empty(); }
    int  Size(void) const { return m_audio_queue.size() * m_frame_size; }

    void SetReady(bool val) { m_initialized = val; }
    bool IsReady(void) const { return m_initialized; }
    bool Flushed(void) const { return m_flushed; }

    std::string CodecName(void) const { return m_codec_name; }
    const AVChannelLayout* ChannelLayout(void) const { return &m_channel_layout; }
    bool LPCM(void) const { return m_lpcm; }
    int  SampleRate(void) const { return m_sample_rate; }
    int  BytesPerSample(void) const { return m_bytes_per_sample; }
    int  FrameSize(void) const { return m_frame_size; }
    int  BlockSize(void) const { return m_block_size; }

    bool DetectCodec(void);

  private:
    bool open_spdif_context(void);
    bool open_spdif(void);
    void initialized(void);

    using frame_t = struct {
        AudioFrame  frame;
        int64_t timestamp = {-1LL};
    };
    using frameque_t = std::deque<frame_t>;
    frameque_t m_audio_queue;
    frameque_t m_probed_queue;

    std::atomic<bool> m_EoF  {false};
    AVChannelLayout  m_channel_layout;

    AVFormatContext* m_spdif_format_context {nullptr};
    AVIOContext*     m_spdif_avio_context   {nullptr};
    uint8_t*         m_spdif_avio_context_buffer  {nullptr};
    const AVCodec*   m_spdif_codec          {nullptr};
    AVCodecID        m_spdif_codec_id;

    bool             m_lpcm                 {true};
    std::string      m_codec_name;
    int              m_num_channels         {-1};
    int              m_bytes_per_sample     {-1};
    int              m_frame_size           {-1};
    int              m_samples_per_frame    {-1};
    int              m_sample_rate          {-1};
    int              m_block_size           {-1};

    AudioIO*         m_parent               {nullptr};
    bool             m_initialized          {false};
    bool             m_flushed              {false};
    bool             m_probing              {true};

    std::mutex       m_write_mutex;
    std::condition_variable m_new_buffer;
    std::condition_variable m_data_avail;

    int              m_id               {-1};
    int              m_verbose          {0};
    int              m_total_write      {0};
    int              m_total_read       {0};
    size_t           m_pkts_read        {0};
};

class AudioIO
{
    friend AudioBuffer;

  public:
    AudioIO(int verbose = 0);
    ~AudioIO(void) { m_running.store(false); }
    void Shutdown(void);

    bool AddBuffer(int num_channels, bool is_lpcm,
                   int bytes_per_sample, int sample_rate,
                   int samples_per_frame, int frame_size);
    bool      RescanSPDIF(void);
    bool      Add(AudioBuffer::AudioFrame & buf, int64_t timestamp);
    int64_t   Seek(int64_t offset, int whence);
    int       Read(uint8_t* dest, int32_t len);
    AVPacket* ReadSPDIF(void);

    int     BufId(void) const;
    int     LastBufId(void) const { return m_buf_id - 1; }
    int     Buffers(void) const { return m_buffer_q.size(); }
    int     Size(void) const;
    bool    Empty(void) const;
    bool    BlockReady(void) const;
    int64_t TimeStamp(void) const { return m_timestamp; }

    std::string CodecName(void) const { return m_codec_name; }
    const AVChannelLayout* ChannelLayout(void) const;
    int     SampleRate(void) const { return m_sample_rate; }
    int     BytesPerSample(void) const { return m_bytes_per_sample; }
    bool    Bitstream(void) { return !m_lpcm; }

    void    Reset(const std::string & where);
    bool    CodecChanged(void);

  private:

    using buffer_que_t = std::deque<AudioBuffer>;

    buffer_que_t     m_buffer_q;

    std::string      m_codec_name;
#if 0
    AVChannelLayout  m_channel_layout;
#endif
    int              m_sample_rate      {-1};
    int              m_bytes_per_sample {0};

    bool             m_lpcm             {true};
    int64_t          m_timestamp        {0LL};

    mutable std::mutex m_buffer_mutex;
    bool             m_codec_initialized {false};

    int              m_buf_id           {0};
    int              m_verbose          {1};
    std::atomic<bool> m_running         {true};
};

#endif
