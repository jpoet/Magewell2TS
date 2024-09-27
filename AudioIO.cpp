#include "AudioIO.h"

#include <unistd.h>
#include <iostream>
#include <stdio.h>
#include <iomanip>

using namespace std;

static std::string AVerr2str(int code)
{
    char astr[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_make_error_string(astr, AV_ERROR_MAX_STRING_SIZE, code);
    return string(astr);
}

AudioBuffer& AudioBuffer::operator=(const AudioBuffer & rhs)
{
    if (this == &rhs)
        return *this;

    m_EoF           = rhs.m_EoF;
    m_loop_cnt      = rhs.m_loop_cnt;
    m_begin         = rhs.m_begin;
    m_end           = rhs.m_end;
    m_write         = rhs.m_write;
    m_read          = rhs.m_read;
    m_prev_frame    = rhs.m_prev_frame;
    m_lpcm          = rhs.m_lpcm;
    m_write_wrapped = rhs.m_write_wrapped;

    m_timestamps = rhs.m_timestamps;
    m_frame_cnt  = rhs.m_frame_cnt;

    m_own_buffer = false;

    m_spdif_format_context = rhs.m_spdif_format_context;
    m_spdif_avio_context   = rhs.m_spdif_avio_context;
    m_spdif_avio_context_buffer = rhs.m_spdif_avio_context_buffer;
    m_spdif_codec          = rhs.m_spdif_codec;
    m_spdif_codec_id       = rhs.m_spdif_codec_id;

    m_codec_name           = rhs.m_codec_name;
    m_num_channels         = rhs.m_num_channels;
    m_bytes_per_sample     = rhs.m_bytes_per_sample;
    m_frame_size           = rhs.m_frame_size;
    m_samples_per_frame    = rhs.m_samples_per_frame;
    m_sample_rate          = rhs.m_sample_rate;
    m_block_size           = rhs.m_block_size;

    m_parent               = rhs.m_parent;
    m_id                   = rhs.m_id;
    m_verbose              = rhs.m_verbose;
    m_report_next          = rhs.m_report_next;

    return *this;
}

bool AudioBuffer::operator==(const AudioBuffer & rhs)
{
    if (this == &rhs)
        return true;
    return m_begin == rhs.m_begin;
}

int64_t AudioBuffer::get_timestamp(uint8_t* P) const
{
    size_t idx = static_cast<uint32_t>(P - m_begin) / m_frame_size;
    return m_timestamps[idx];
}

AudioBuffer::AudioBuffer(uint8_t* Pbegin, uint8_t* Pend,
                         int num_channels, bool is_lpcm,
                         int bytes_per_sample, int sample_rate,
                         int samples_per_frame, int frame_size,
                         int64_t* timestamps,
                         AudioIO* parent, int verbose, int id)
    : m_begin(Pbegin)
    , m_end(Pend)
    , m_num_channels(num_channels)
    , m_lpcm(is_lpcm)
    , m_bytes_per_sample(bytes_per_sample)
    , m_sample_rate(sample_rate)
    , m_samples_per_frame(samples_per_frame)
    , m_frame_size(frame_size)
    , m_write(Pbegin)
    , m_read(Pbegin)
    , m_timestamps(timestamps)
    , m_parent(parent)
    , m_verbose(verbose)
    , m_id(id)
{
    m_block_size = 8 * m_bytes_per_sample * m_samples_per_frame;
}

AudioBuffer::~AudioBuffer(void)
{
    if (m_own_buffer)
    {
        m_EoF = true;
        if (m_detect_thread.joinable())
            m_detect_thread.join();
        delete[] m_begin;
        delete[] m_timestamps;
    }
}

void AudioBuffer::OwnBuffer(void)
{
    m_own_buffer = true;

    if (m_lpcm)
    {
        std::unique_lock<std::mutex> lock(m_parent->m_codec_mutex);
        m_codec_name = "eac3";
        m_channel_layout = AV_CHANNEL_LAYOUT_STEREO;
        m_parent->m_codec_ready = true;
        m_parent->m_codec_cond.notify_one();
    }
    else
        m_detect_thread = std::thread(&AudioBuffer::detect_codec, this);

    PrintPointers("AddBuffer");
    PrintState("AddBuffer", true);
}

void AudioBuffer::PrintState(const string & where, bool force) const
{
    if (force || m_verbose > 0)
    {
        string loc = "[" + std::to_string(m_id) + "] " + where + " ";
        cerr << loc
             << (m_lpcm ? "LPCM" : "Bitstream")
             << " Codec: " << m_codec_name
             << ", Channels: " << m_num_channels
             << ", BytesPerSample: " << m_bytes_per_sample
             << ", FrameSize: " << m_frame_size
             << "\n" << string(loc.size(), ' ')
             << "SamplesPerFrame: " << m_samples_per_frame
             << ", SampleRate: " << m_sample_rate
             << ", BlockSize: " << m_block_size
             << ", TotalBytes: " << m_total
             << endl;
    }
}

void AudioBuffer::PrintPointers(const string & where, bool force) const
{
    if (force || m_verbose > 4)
    {
        string loc = "[" + std::to_string(m_id) + "] " + where + " ";
        cerr << loc
             << "begin: " << (uint64_t)(m_begin)
             << ", end : " << (size_t)(m_end - m_begin)
             << ", write : " << std::setw(6) << (size_t)(m_write - m_begin)
             << ", read : " << std::setw(6) << (size_t)(m_read - m_begin)
             << ", frame sz: " << m_frame_size
             << ", wrapped: " << (m_write_wrapped ? "Yes, " : "No, ")
             << (m_lpcm ? " LPCM" : " bistream")
             << " codec: " << m_codec_name
//             << ", timestamp: " << m_timestamp
             << endl;
    }
}

int AudioBuffer::Add(uint8_t* Pframe, size_t len, int64_t timestamp)
{
    const std::unique_lock<std::mutex> lock(m_write_mutex);

    m_total += len;

    m_write = Pframe + len;
    PrintPointers("      Add");

    if (m_write > m_end)
    {
        cerr << "WARNING: [" << m_id << "] Add audio to " << (uint64_t)Pframe
             << " - " << m_write
             << " which is greater than the end " << (uint64_t)m_end
             << endl;
    }

    if (m_prev_frame == m_end)
    {
        ++m_loop_cnt;
        m_write_wrapped = true;
        if (m_verbose > 4)
            cerr << "AudioIO::Add: wrapped\n";
    }
    else if (Pframe < m_prev_frame)
    {
        cerr << "WARNING: [" << m_id << "] Add audio to " << (uint64_t)Pframe
             << " which is less than " << (uint64_t)m_prev_frame
             << " but not the begining " << (uint64_t)m_begin
             << endl;
    }
    m_prev_frame = m_write;

    if (m_write_wrapped && m_read < m_write)
    {
        if (m_verbose > 0 && m_read != m_begin)
        {
            cerr << "WARNING: [" << m_id
                 << "] Overwrote buffer begin, moving read\n";
            PrintPointers("      Add", true);
            m_read = m_write;
        }
    }

    return 0;
}

int AudioBuffer::Read(uint8_t* dest, size_t len)
{
    uint8_t* Pend;
    size_t   sz;

    if (Empty())
    {
        if (m_EoF)
        {
            if (m_verbose > 0)
                cerr << "[" << m_id << "] AudioIO::Read: EOF\n";
            return AVERROR_EOF;
        }

        if (m_verbose > 5)
            cerr << "[" << m_id << "] AudioIO::Read: buffer is empty\n";
        return 0;
    }

    const std::unique_lock<std::mutex> lock(m_write_mutex);

    if (m_write_wrapped)
    {
        if (m_read == m_end)
        {
            Pend = m_write;
            m_read = m_begin;
            m_write_wrapped = false;
        }
        else if (m_read > m_end)
        {
            cerr << "[" << m_id << "] Read has passed the end!\n";
            cerr << "[" << m_id << "] Audio write: "
                 << (size_t)(m_write - m_begin)
                 << " read: " << (size_t)(m_read - m_begin)
                 << " end: " << (size_t)(m_end - m_begin)
                 << endl;
            exit(-1);
        }
        else
            Pend = m_end;
    }
    else
    {
        if (m_read > m_write)
        {
            cerr << "WARNING: [" << m_id << "] Read has passed Write!\n";
            PrintPointers("     Read", true);
            exit(-1);
        }
        Pend = m_write;
    }

    if (Pend - m_read < len)
    {
        sz = Pend - m_read;
        if (m_verbose > 4)
        {
            cerr << "[" << m_id << "] AudioIO::Read: Requested " << len
                 << " bytes, but only " << sz << " bytes available\n";
            m_report_next = 10;
        }
    }
    else
        sz = len;

    if (dest != nullptr)
        memcpy(dest, m_read, sz);
    m_read += sz;
    PrintPointers("     Read", m_report_next);
    if (m_report_next > 0)
        --m_report_next;

    m_parent->m_timestamp = get_timestamp(m_read - m_frame_size);
//    cerr << "\nTS: " << m_parent->m_timestamp << endl;

    return sz;
}

int64_t AudioBuffer::Seek(int64_t offset, int whence)
{
    if (m_read == m_write)
        return 0;

    int force = whence & AVSEEK_FORCE;
    whence &= ~AVSEEK_FORCE;
    int size = whence & AVSEEK_SIZE;
    whence &= ~AVSEEK_SIZE;

    if (force)
        return -1;

    string whence_str;
    switch (whence)
    {
        case SEEK_END:
          whence_str = "END";
          break;
        case SEEK_SET:
          whence_str = "SET";
          break;
        case SEEK_CUR:
          whence_str = "CURRENT";
          break;
        default:
          whence_str = "UNHANDLED";
          cerr << "[" << m_id << "] whence = " << whence << endl;
          break;
    }
    if (m_verbose > 3)
        cerr << "[" << m_id << "] Seeking from " << whence_str
             << " to " << offset << endl;

    int64_t desired = offset;

    if (whence == SEEK_END)
    {
        m_read = m_write + m_frame_size;
        if (desired > 0)
            return -1;
    }
    else if (whence == SEEK_SET)
    {
        // Basically rewind as much as we can and seek from there.
        if (m_loop_cnt)
            m_read = m_write;
        else
            m_read = m_begin;
        desired = std::max(desired, static_cast<int64_t>(0));
    }
    else if (whence != SEEK_CUR)
        return -1;

    if (desired < 0)
    {
        // Backwards
        desired = -desired; // To keep my head from exploading
        if (m_write_wrapped)
        {
            if (m_read - desired < m_write)
                m_read = m_write;
            else
                m_read -= desired;
        }
        else
        {
            if (m_read - desired < m_begin)
            {
                if (m_loop_cnt)
                {
                    // Wrapped
                    m_read = m_end - (m_begin - (m_read - desired));
                    if (m_read < m_write)
                        m_read = m_write;
                }
                else
                    m_read = m_begin;
            }
            else
                m_read -= desired;
        }
        m_parent->m_timestamp = get_timestamp(m_read - m_frame_size);
    }
    else if (desired > 0)
    {
        size_t len = Read(nullptr, desired);
        if (len < desired)
            return -1;
        return 0;
    }

    return 0;
}

AVPacket* AudioBuffer::ReadSPDIF(void)
{
    if (Size() < m_block_size)
    {
        if (m_verbose > 4)
            cerr << "[" << m_id << "] write_bitstream_frame: Only "
                 << Size() << " bytes available. "
                 << m_block_size << " desired.\n";
        return nullptr;
    }

    AVPacket* pkt = av_packet_alloc();
    if (!pkt)
    {
        cerr << "WARNING: [" << m_id
             << "] Could not allocate pkt for spdif input.\n";
        return nullptr;
    }

    double ret =  av_read_frame(m_spdif_format_context, pkt);
    if (0 > ret)
    {
        av_packet_free(&pkt);
        if (ret != AVERROR_EOF && m_verbose > 0)
            cerr << "WARNING: [" << m_id
                 << "] Failed to read spdif frame: (" << ret << ") "
                 << AVerr2str(ret) << endl;
        return nullptr;
    }

    return pkt;
}

static int read_packet(void* opaque, uint8_t* buf, int buf_size)
{
    AudioBuffer* q = reinterpret_cast<AudioBuffer* >(opaque);

    return q->Read(buf, buf_size);
}

static int64_t seek_packet(void* opaque, int64_t offset, int whence)
{
    AudioBuffer* q = reinterpret_cast<AudioBuffer* >(opaque);

    return q->Seek(offset, whence);
}

bool AudioBuffer::open_spdif_context(void)
{
    if (!(m_spdif_format_context = avformat_alloc_context()))
    {
        cerr << "WARNING: [" << m_id
             << "] Unable to allocate spdif format context.\n";
        return false;
    }

    m_spdif_avio_context_buffer =
        reinterpret_cast<uint8_t* >(av_malloc(m_frame_size));
    if (!m_spdif_avio_context_buffer)
    {
        cerr << "WARNING: [" << m_id
             << "] Unable to allocate spdif avio context buffer.\n";
        return false;
    }

    m_spdif_avio_context = avio_alloc_context(m_spdif_avio_context_buffer,
                                              m_block_size,
                                              0,
                                              reinterpret_cast<void* >(this),
                                              read_packet,
                                              nullptr,
                                              seek_packet);
    if (!m_spdif_avio_context)
    {
        cerr << "WARNING: [" << m_id
             << "] Unable to allocate audio input avio context.\n";
        return false;
    }

    m_spdif_format_context->pb = m_spdif_avio_context;

    const AVInputFormat* spdif_fmt = av_find_input_format("spdif");

    if (0 > avformat_open_input(&m_spdif_format_context, NULL,
                                spdif_fmt, NULL))
    {
        cerr << "WARNING: [" << m_id << "] Could not open spdif input.\n";
        return false;
    }

    return true;
}

bool AudioBuffer::open_spdif(void)
{
    /* retrieve stream information */
    const AVInputFormat* fmt = nullptr;
    int ret;
    int idx;

    open_spdif_context();

    int try_cnt = 5;
    for (idx = 0; idx < try_cnt; ++idx)
    {
        if ((ret = av_probe_input_buffer(m_spdif_avio_context,
                                         &fmt, "", nullptr, 0,
                                         /* m_block_size * 8 */ 0)) != 0)
        {
            if (m_verbose > 0)
            {
                cerr << "WARNING: [" << m_id << "] Failed to probe spdif input: "
                     << AVerr2str(ret) << endl;
            }
            continue;
        }

        if (m_verbose > 1)
        {
            cerr << "[" << m_id << "] --> Detected fmt '" << fmt->name
                 << "' '" << fmt->long_name << "'\n";
        }

        if (0 > avformat_find_stream_info(m_spdif_format_context, NULL))
        {
            cerr << "WARNING: [" << m_id
                 << "] Could not find stream information\n";
            continue;
        }

        if (m_spdif_format_context->nb_streams < 1)
        {
            cerr << "WARNING: [" << m_id << "] No streams found in SPDIF.\n";
            continue;
        }

        int audio_stream_idx = 0;

        AVStream* audio_stream =
            m_spdif_format_context->streams[audio_stream_idx];

        if (m_verbose > 0)
        {
            /* dump input information to stderr */
            av_dump_format(m_spdif_format_context, 0, "pipe:0", 0);
        }

        if (!audio_stream)
        {
            cerr << "WARNING: [" << m_id
                 << "] Could not find audio stream in spdif input.\n";
            continue;
        }

        m_spdif_codec_id = audio_stream->codecpar->codec_id;

        break;
    }

    if (idx >= try_cnt)
    {
        cerr << "ERROR: [" << m_id << "] GAVE UP trying to find S/PDIF codec\n";
        return false;
    }

    std::unique_lock<std::mutex> lock(m_parent->m_codec_mutex);

    /* Find a decoder for the audio stream. */
    if (!(m_spdif_codec = avcodec_find_decoder(m_spdif_codec_id)))
    {
        cerr << "WARNING: [" << m_id << "] Could not find input audio codec "
             << m_spdif_codec_id << "\n";
        m_codec_name = "Unknown";
        return false;
    }

    m_codec_name = m_spdif_codec->name;
    m_channel_layout = AV_CHANNEL_LAYOUT_5POINT1;

    /* The AVIO buffer is not timestamp aware, so discard it
     */
    avio_flush(m_spdif_avio_context);
    avformat_flush(m_spdif_format_context);
    /* Now make that data available again */
    Seek(0, SEEK_SET);

    return true;
}

void AudioBuffer::detect_codec(void)
{
    if (open_spdif())
    {
        m_parent->m_codec_mutex.lock();
        m_parent->m_codec_ready = true;
        m_parent->m_codec_mutex.unlock();
        m_parent->m_codec_cond.notify_one();
        PrintState("SPDIF", true);
    }
    else
        PrintState("SPDIF", true);
}

size_t AudioBuffer::Size(void) const
{
    if (m_write_wrapped)
        return (m_end - m_read) +
            (m_write - m_begin);
    else
        return m_write - m_read;
}

/************************************************
 * AudioIO
 ************************************************/
AudioIO::AudioIO(int verbose)
    : m_verbose(verbose)
{
}

void AudioIO::SetEoF(void)
{
    buffer_que_t::iterator Ibuf;
    for (Ibuf = m_buffer_q.begin(); Ibuf != m_buffer_q.end(); ++Ibuf)
        (*Ibuf).setEoF();
}

void AudioIO::AddBuffer(uint8_t* Pbegin, uint8_t* Pend,
                        int num_channels, bool is_lpcm,
                        int bytes_per_sample, int sample_rate,
                        int samples_per_frame, int frame_size,
                        int64_t* timestamps)
{
    const std::unique_lock<std::mutex> lock(m_buffer_mutex);
    buffer_que_t::iterator Ibuf;

    if (!m_buffer_q.empty())
    {
        Ibuf = m_buffer_q.end() - 1;
        (*Ibuf).setEoF();
    }

    m_buffer_q.push_back(AudioBuffer(Pbegin, Pend,
                                     num_channels, is_lpcm,
                                     bytes_per_sample, sample_rate,
                                     samples_per_frame, frame_size,
                                     timestamps,
                                     this, m_verbose, m_buf_id++));

    Ibuf = m_buffer_q.end() - 1;
    (*Ibuf).OwnBuffer();
}

bool AudioIO::WaitForReady(void)
{
    // Wait for codecs to be detected.
    while (m_running.load() == true)
    {
        m_buffer_mutex.lock();
        while (m_buffer_q.size() > 1 && m_buffer_q.front().Empty())
            m_buffer_q.pop_front();

        if (!m_buffer_q.empty())
        {
            buffer_que_t::iterator Ibuf = m_buffer_q.begin();
            m_buffer_mutex.unlock();

            if (!(*Ibuf).CodecName().empty())
            {
                const std::unique_lock<std::mutex> lock(m_codec_mutex);
                m_codec_name = (*Ibuf).CodecName();
                m_channel_layout = (*Ibuf).ChannelLayout();
                m_sample_rate = (*Ibuf).SampleRate();
                m_bytes_per_sample = (*Ibuf).BytesPerSample();
                m_lpcm = (*Ibuf).LPCM();
                return true;
            }
        }
        else
            m_buffer_mutex.unlock();

        std::unique_lock<std::mutex> lock(m_codec_mutex);
        m_codec_cond.wait(lock, [&]{return m_codec_ready;});

        m_codec_ready = false;
    }
    return false;
}

size_t AudioIO::Size(void) const
{
    const std::unique_lock<std::mutex> lock(m_buffer_mutex);

    if (m_buffer_q.empty())
        return 0;

    size_t sz = 0;
    buffer_que_t::const_iterator Ibuf;
    for (Ibuf = m_buffer_q.begin(); Ibuf != m_buffer_q.end(); ++Ibuf)
    {
        sz += (*Ibuf).Size();
    }

    return sz;
}

bool AudioIO::Empty(void) const
{
    const std::unique_lock<std::mutex> lock(m_buffer_mutex);

    if (m_buffer_q.empty())
        return true;

    buffer_que_t::const_iterator Ibuf;
    for (Ibuf = m_buffer_q.begin(); Ibuf != m_buffer_q.end(); ++Ibuf)
    {
        if (!(*Ibuf).Empty())
            return false;
    }
    return true;
}

int AudioIO::Add(uint8_t* Pframe, size_t len, int64_t timestamp)
{
//    const std::unique_lock<std::mutex> lock(m_buffer_mutex);

    if (m_buffer_q.empty())
    {
        cerr << "WARNING: No audio buffers to Add to\n";
        return 0;
    }
    buffer_que_t::iterator Ibuf = m_buffer_q.end() - 1;

    return (*Ibuf).Add(Pframe, len, timestamp);
}

int64_t AudioIO::Seek(int64_t offset, int whence)
{
    const std::unique_lock<std::mutex> lock(m_buffer_mutex);

    if (m_buffer_q.empty())
    {
        cerr << "WARNING: No audio buffers to Seek in\n";
        return 0;
    }
    buffer_que_t::iterator Ibuf = m_buffer_q.begin();
    return (*Ibuf).Seek(offset, whence);
}

int AudioIO::Read(uint8_t* dest, size_t len)
{
//    const std::unique_lock<std::mutex> lock(m_buffer_mutex);

    if (m_buffer_q.empty())
    {
        cerr << "WARNING: No audio buffers to Read from\n";
        return 0;
    }

    buffer_que_t::iterator Ibuf = m_buffer_q.begin();
    return (*Ibuf).Read(dest, len);
}

AVPacket* AudioIO::ReadSPDIF(void)
{
//    const std::unique_lock<std::mutex> lock(m_buffer_mutex);

    if (m_buffer_q.empty())
    {
        cerr << "WARNING: No audio buffers to Read from\n";
        return 0;
    }

    buffer_que_t::iterator Ibuf = m_buffer_q.begin();
    return (*Ibuf).ReadSPDIF();
}

bool AudioIO::CodecChanged(void)
{
    {
        const std::unique_lock<std::mutex> lock(m_buffer_mutex);
        if (m_buffer_q.size() < 2)
            return false;
    }

    string codec = m_codec_name;
    int    bytes_per_sample = m_bytes_per_sample;
    WaitForReady();
    if (codec == m_codec_name && bytes_per_sample == m_bytes_per_sample)
        return false;

    if (m_verbose > 0)
    {
        buffer_que_t::iterator Ibuf = m_buffer_q.begin();
        cerr << "New audio buffer codec:\n";
        (*Ibuf).PrintState("New");
    }

    return true;
}
