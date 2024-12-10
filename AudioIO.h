#ifndef _AudioIO_h_
#define _AudioIO_h_

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include <string>
#include <deque>
#include <utility>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

class AudioIO;

class AudioBuffer
{
  public:
    AudioBuffer(uint8_t* Pbegin, uint8_t* Pend,
                int num_channels, bool is_lpcm,
                int bytes_per_sample, int sample_rate,
                int samples_per_frame, int frame_size,
                int64_t* timestamps,
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

    int Add(uint8_t* Pframe, int len, int64_t timestamp);
    int Read(uint8_t* dest, int len);
    AVPacket* ReadSPDIF(void);

    int64_t Seek(int64_t offset, int whence);
    void set_mark(void);
    void return_to_mark(void);

    int  Id(void) const { return m_id; }
    bool Empty(void) const { return m_read == m_write; }
    int  Size(void) const;
    int  Tell(void) const { return m_frame_cnt; }

    std::string CodecName(void) const { return m_codec_name; }
    AVChannelLayout ChannelLayout(void) const { return m_channel_layout; }
    bool LPCM(void) const { return m_lpcm; }
    int  SampleRate(void) const { return m_sample_rate; }
    int  BytesPerSample(void) const { return m_bytes_per_sample; }
    int  FrameSize(void) const { return m_frame_size; }
    int  BlockSize(void) const { return m_block_size; }

    bool DetectCodec(void);

  private:
    bool open_spdif_context(void);
    bool open_spdif(void);
    int64_t get_timestamp(uint8_t* P) const;

    std::atomic<bool> m_EoF  {false};
    uint32_t m_loop_cnt      {0};
    uint8_t* m_begin         {nullptr};
    uint8_t* m_end           {nullptr};
    uint8_t* m_write         {nullptr};
    uint8_t* m_read          {nullptr};
    uint8_t* m_prev_frame    {nullptr};
    bool     m_write_wrapped {false};
    AVChannelLayout  m_channel_layout;

    int64_t* m_timestamps  {nullptr};
    int      m_frame_cnt   {0};
    bool     m_own_buffer  {false};

    int              m_mark;

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

    std::mutex       m_write_mutex;

    int              m_id               {-1};
    int              m_verbose          {0};
    int              m_total            {0};
    int              m_report_next      {0};
};

class AudioIO
{
    friend AudioBuffer;

  public:
    AudioIO(int verbose = 0);
    ~AudioIO(void) { m_running.store(false); }
    void Shutdown(void);

    bool AddBuffer(uint8_t* Pbegin, uint8_t* Pend,
                   int num_channels, bool is_lpcm,
                   int bytes_per_sample, int sample_rate,
                   int samples_per_frame, int frame_size,
                   int64_t* timestamps);
    bool      RescanSPDIF(void);
    int       Add(uint8_t* Pframe, int len, int64_t timestamp);
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
    AVChannelLayout ChannelLayout(void) const { return m_channel_layout; }
    int     SampleRate(void) const { return m_sample_rate; }
    int     BytesPerSample(void) const { return m_bytes_per_sample; }

    bool    Bitstream(void) { return !m_lpcm; }
    bool    CodecChanged(void);

  private:

    using buffer_que_t = std::deque<AudioBuffer>;

    buffer_que_t     m_buffer_q;

    std::string      m_codec_name;
    AVChannelLayout  m_channel_layout;
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
