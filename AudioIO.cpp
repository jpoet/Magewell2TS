#include "AudioIO.h"
#include "lock_ios.h"

#include <unistd.h>
#include <iostream>
#include <stdio.h>
#include <iomanip>
#include <csignal>

using namespace std;
using namespace s6_lock_ios;

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

    m_write_loop_cnt = rhs.m_write_loop_cnt;
    m_read_loop_cnt = rhs.m_read_loop_cnt;
    m_begin         = rhs.m_begin;
    m_end           = rhs.m_end;
    m_write         = rhs.m_write;
    m_read          = rhs.m_read;
    m_lpcm          = rhs.m_lpcm;

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
    int idx = static_cast<uint32_t>(P - m_begin) / m_frame_size;
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
    , m_write(Pbegin)
    , m_read(Pbegin)
    , m_timestamps(timestamps)
    , m_lpcm(is_lpcm)
    , m_num_channels(num_channels)
    , m_bytes_per_sample(bytes_per_sample)
    , m_frame_size(frame_size)
    , m_samples_per_frame(samples_per_frame)
    , m_sample_rate(sample_rate)
    , m_parent(parent)
    , m_id(id)
    , m_verbose(verbose)
{
    m_block_size = 8 * m_bytes_per_sample * m_samples_per_frame * 8;
}

AudioBuffer::~AudioBuffer(void)
{
    if (m_own_buffer)
    {
        m_EoF.store(true);
        delete[] m_begin;
        delete[] m_timestamps;
    }
}

void AudioBuffer::Clear(void)
{
    const std::unique_lock<std::mutex> lock(m_write_mutex);
    m_read = m_write;
    m_write_loop_cnt = 0;
    m_read_loop_cnt = 0;
    if (m_verbose > 1)
        cerr << lock_ios()
             << "[" << m_id << "] audio buffer cleared.\n";
}

bool AudioBuffer::RescanSPDIF(void)
{
    return DetectCodec();
}

void AudioBuffer::OwnBuffer(void)
{
    m_own_buffer = true;
    m_EoF.store(false);

    if (m_lpcm)
    {
        m_codec_name = "ac3";
        m_channel_layout = AV_CHANNEL_LAYOUT_STEREO;
    }

    PrintPointers("OwnBuffer");
    PrintState("Audio");
}

void AudioBuffer::PrintState(const string & where, bool force) const
{
    if (force || m_verbose > 0)
    {
        string loc = "[" + std::to_string(m_id) + "] " + where + " ";
        cerr << lock_ios()
             << loc
             << (m_lpcm ? "LPCM" : "Bitstream")
             << " Codec: " << (m_codec_name.empty() ? "Unknown" : m_codec_name)
             << ", Channels: " << m_num_channels
             << ", BytesPerSample: " << m_bytes_per_sample
             << ",\n" << string(loc.size(), ' ')
             << "FrameSize: " << m_frame_size
             << ", SamplesPerFrame: " << m_samples_per_frame
             << ", SampleRate: " << m_sample_rate
             << ",\n" << string(loc.size(), ' ')
             << "BlockSize: " << m_block_size
             << ", TotalBytes: " << m_total
             << endl;
    }
}

void AudioBuffer::PrintPointers(const string & where, bool force) const
{
    if (force || m_verbose > 4)
    {
        string loc = "[" + std::to_string(m_id) + "] " + where + " ";
        cerr << lock_ios()
             << loc
             << "begin: " << (uint64_t)(m_begin)
             << ", end : " << (int)(m_end - m_begin)
             << ", write : " << std::setw(6) << (int)(m_write - m_begin)
             << ", read : " << std::setw(6) << (int)(m_read - m_begin)
             << ", frame sz: " << m_frame_size
             << ", loops write: " << m_write_loop_cnt
             << " read: " << m_read_loop_cnt
             << (m_lpcm ? " LPCM" : " bistream")
             << " codec: " << m_codec_name
//             << ", timestamp: " << m_timestamp
             << ", size " << Size()
             << endl;
    }
}

int AudioBuffer::Add(uint8_t* Pframe, int len, int64_t timestamp)
{
    {
        const std::unique_lock<std::mutex> lock(m_write_mutex);

        m_total += len;

        PrintPointers("      Add");
        m_write = Pframe + len;

        if (m_write > m_end)
        {
            cerr << lock_ios()
                 << "WARNING: [" << m_id << "] Add audio to " << (uint64_t)Pframe
                 << " - " << m_write
                 << " which is greater than the end " << (uint64_t)m_end
                 << endl;
        }

        if (m_write == m_end)
        {
            ++m_write_loop_cnt;
            m_write = m_begin;
        }

        if (!m_EoF && m_write <= m_read && (m_write + len) >= m_read)
        {
            // Next write /could/ pass the read point!
            if (m_verbose > 0)
            {
                cerr << lock_ios() << "WARNING [" << m_id
                     << "] Next AUDIO write could overwrite read position.\n";
                PrintPointers("      Add", true);
            }
        }
    }

    m_data_avail.notify_one();

    return 0;
}

int AudioBuffer::Read(uint8_t* dest, int32_t len)
{
    uint8_t* Pend;
    int   sz;

#if 0
    cerr << "\n[" << m_id << "R]";
#endif

    if (Empty())
    {
        std::unique_lock<std::mutex> lock(m_write_mutex);
        m_data_avail.wait_for(lock, chrono::microseconds(1500),
                              [this]{ return !Empty() || m_EoF.load(); });

        if (m_EoF.load() == true)
        {
            if (m_verbose > 2)
            {
                cerr << lock_ios()
                     << "[" << m_id << "] AudioIO::Read: EOF\n";
            }
            m_flushed = true;
            m_parent->StateChanged("Read flushed");
            return AVERROR_EOF;
        }

        if (Empty())
            return 0;
    }

#if 0
    if (empty_cnt)
    {
        if (m_verbose > 2)
            cerr << lock_ios()
                 << "INFO: [" << m_id
                 << "] AudioBuffer::Read: contiguously called " << empty_cnt
                 << " times with no data available.\n";
        empty_cnt = 0;
    }
#endif

    const std::unique_lock<std::mutex> lock(m_write_mutex);

    if (m_write_loop_cnt > m_read_loop_cnt)
    {
        if (m_read == m_end)
        {
            Pend = m_write;
            m_read = m_begin;
            ++m_read_loop_cnt;
        }
        else if (m_read > m_end)
        {
            cerr << lock_ios()
                 << "[" << m_id << "] Read has passed the end!\n"
                 << "[" << m_id << "] Audio write: "
                 << (int)(m_write - m_begin)
                 << " read: " << (int)(m_read - m_begin)
                 << " end: " << (int)(m_end - m_begin)
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
            cerr << lock_ios()
                 << "WARNING: [" << m_id << "] Read has passed Write!\n";
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
            cerr << lock_ios()
                 << "[" << m_id << "] AudioIO::Read: Requested " << len
                 << " bytes, but only " << sz << " bytes available\n";
            m_report_next = 10;
        }
    }
    else
        sz = len;

    if (dest != nullptr)
        memcpy(dest, m_read, sz);
    m_parent->m_timestamp = get_timestamp(m_read);
    m_read += sz;
    PrintPointers("     Read", m_report_next);
    if (m_report_next > 0)
        --m_report_next;

#if 0
    cerr << lock_ios()
         << "\n[" << m_id << "] TS: " << m_parent->m_timestamp
         << " sz " << sz << " Frames " << sz / m_frame_size << endl;
#endif

    m_frame_cnt += sz;
    return sz;
}

AVPacket* AudioBuffer::ReadSPDIF(void)
{
#if 0
    cerr << "[" << m_id << "S]";
#endif

#if 0
    if (Size() < m_frame_size)
    {
        if (m_verbose > 1)
            cerr << lock_ios()
                 << "[" << m_id << "] ReadSPDIF: Only "
                 << Size() << " bytes available. "
                 << m_frame_size << " desired.\n";
        return nullptr;
    }
#endif

    AVPacket* pkt = av_packet_alloc();
    if (!pkt)
    {
        cerr << lock_ios()
             << "WARNING: [" << m_id
             << "] Could not allocate pkt for spdif input.\n";
        return nullptr;
    }

    if (m_spdif_format_context == nullptr)
    {
        cerr << lock_ios()
             << "WARNING: [" << m_id << "] S/PDIF context is invalid.\n";
        return nullptr;
    }

    double ret =  av_read_frame(m_spdif_format_context, pkt);
#if 0
    cerr << lock_ios()
         << "ReadSPDIF [" << pkt->stream_index << "] pts: " << pkt->pts
         << " dts: " << AVerr2str(pkt->dts)
         << " duration: " << AVerr2str(pkt->duration)
         << " size: " << pkt->size
         << endl;
#endif
#if 0
    cerr << lock_ios() << "[" << m_id << "] ReadSPDIF ret " << ret
         << " ts " << m_parent->m_timestamp << endl;

#endif

    if (0 > ret)
    {
        av_packet_free(&pkt);
        if (ret != AVERROR_EOF && m_verbose > 0)
            cerr << lock_ios()
                 << "WARNING: [" << m_id
                 << "] Failed to read spdif frame: (" << ret << ") "
                 << AVerr2str(ret) << endl;
        return nullptr;
    }


#if 0
    cerr << lock_ios() << "[" << m_id << "] ReadSPDIF Good "
         << " ts " << m_parent->m_timestamp << endl;

#endif
    return pkt;
}

int64_t AudioBuffer::Seek(int64_t offset, int whence)
{
    if (m_read == m_write)
        return 0;

    int force = whence & AVSEEK_FORCE;
    whence &= ~AVSEEK_FORCE;
#if 0
    int size = whence & AVSEEK_SIZE;
#endif
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
          cerr << lock_ios()
               << "[" << m_id << "] whence = " << whence << endl;
          break;
    }
    if (m_verbose > 2)
        cerr << lock_ios()
             << "[" << m_id << "] Seeking from " << whence_str
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
        if (m_write_loop_cnt > m_read_loop_cnt)
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
        if (m_write_loop_cnt > m_read_loop_cnt)
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
                if (m_read_loop_cnt)
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
        m_parent->m_timestamp = get_timestamp(m_read /* - m_frame_size */);
#if 0
        cerr << lock_ios() << " AudioBuffer::Seek TS "
             << m_parent->m_timestamp << endl;
#endif
    }
    else if (desired > 0)
    {
        int len = Read(nullptr, desired);
        if (len < desired)
            return -1;
        return 0;
    }

    return 0;
}

void AudioBuffer::set_mark(void)
{
    m_mark = m_frame_cnt;
}

void AudioBuffer::return_to_mark(void)
{
    Seek(m_mark - m_frame_cnt, SEEK_CUR);
#if 0
    cerr << lock_ios() << "%%%%%%%%%%%%%%% "
         << "return_to_mark: " <<m_parent->m_timestamp << endl;
#endif
}

static int read_packet(void* opaque, uint8_t* buf, int buf_size)
{
    AudioBuffer* q = reinterpret_cast<AudioBuffer* >(opaque);

#if 1
    return q->Read(buf, buf_size);
#else
    int ret = q->Read(buf, buf_size);
    cerr << lock_ios() << " read_packet: " << ret << endl;
    return ret;
#endif
}

static int64_t seek_packet(void* opaque, int64_t offset, int whence)
{
    AudioBuffer* q = reinterpret_cast<AudioBuffer* >(opaque);

    return q->Seek(offset, whence);
}

bool AudioBuffer::open_spdif_context(void)
{
    if (m_spdif_format_context)
    {
        avformat_close_input(&m_spdif_format_context);
        m_spdif_format_context = nullptr;

        avio_context_free(&m_spdif_avio_context);
        m_spdif_avio_context = nullptr;

#if 0
        av_free(m_spdif_avio_context_buffer);
        m_spdif_avio_context_buffer = nullptr;
#endif

        avformat_free_context(m_spdif_format_context);
        m_spdif_format_context = nullptr;
    }

    if (!(m_spdif_format_context = avformat_alloc_context()))
    {
        cerr << lock_ios()
             << "WARNING: [" << m_id
             << "] Unable to allocate spdif format context.\n";
        return false;
    }

    m_spdif_avio_context_buffer =
        reinterpret_cast<uint8_t* >(av_malloc(m_frame_size));
    if (!m_spdif_avio_context_buffer)
    {
        cerr << lock_ios()
             << "WARNING: [" << m_id
             << "] Unable to allocate spdif avio context buffer.\n";
        return false;
    }

    m_spdif_avio_context = avio_alloc_context(m_spdif_avio_context_buffer,
                                              m_frame_size,
                                              0,
                                              reinterpret_cast<void* >(this),
                                              read_packet,
                                              nullptr,
                                              seek_packet);
    if (!m_spdif_avio_context)
    {
        cerr << lock_ios()
             << "WARNING: [" << m_id
             << "] Unable to allocate audio input avio context.\n";
        return false;
    }

    m_spdif_format_context->pb = m_spdif_avio_context;

    const AVInputFormat* spdif_fmt = av_find_input_format("spdif");

    if (0 > avformat_open_input(&m_spdif_format_context, NULL,
                                spdif_fmt, NULL))
    {
        cerr << lock_ios()
             << "WARNING: [" << m_id << "] Could not open spdif input.\n";
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

    if (m_verbose > 1)
        cerr << lock_ios()
             << "[" << m_id << "] Scanning S/PDIF\n";

    set_mark();

    if (m_spdif_format_context == nullptr)
        open_spdif_context();

    int try_cnt = 1;
    for (idx = 0; idx < try_cnt; ++idx)
    {
        if (m_EoF.load())
        {
            cerr << lock_ios()
                 << "WARNING: [" << m_id << "] Abort S/PDIF scan due EoF.\n";
            m_flushed = true;
            m_parent->StateChanged("open_spdif EoF");
            return false;
        }

        if ((ret = av_probe_input_buffer(m_spdif_avio_context,
                                         &fmt, "", nullptr, 0,
                                         /* m_block_size * 50 */ 0)) != 0)
        {
#if 0
            if (!m_codec_name.empty())
            {
                return_to_mark();
                cerr << lock_ios()
                     << "Re-initilized S/PDIF. Skipping rest of bitstream scan.\n";
                return true;
            }
#endif
            cerr << lock_ios()
                 << "WARNING: [" << m_id << "] Failed to probe spdif input: "
                 << AVerr2str(ret) << endl;
            continue;
        }

        if (m_verbose > 1)
        {
            cerr << lock_ios()
                 << "[" << m_id << "] --> Detected fmt '" << fmt->name
                 << "' '" << fmt->long_name << "'\n";
        }

        if (0 > avformat_find_stream_info(m_spdif_format_context, NULL))
        {
            cerr << lock_ios()
                 << "WARNING: [" << m_id
                 << "] Could not find stream information\n";
            continue;
        }

        if (m_spdif_format_context->nb_streams < 1)
        {
            cerr << lock_ios()
                 << "WARNING: [" << m_id << "] No streams found in SPDIF.\n";
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
            cerr << lock_ios()
                 << "WARNING: [" << m_id
                 << "] Could not find audio stream in spdif input.\n";
            continue;
        }

        m_spdif_codec_id = audio_stream->codecpar->codec_id;

        break;
    }

    if (idx >= try_cnt)
        return false;

    /* Find a decoder for the audio stream. */
    if (!(m_spdif_codec = avcodec_find_decoder(m_spdif_codec_id)))
    {
        cerr << lock_ios()
             << "WARNING: [" << m_id << "] Could not find input audio codec "
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

    return_to_mark();
    return true;
}

bool AudioBuffer::DetectCodec(void)
{
    int idx = 0;
    while (m_EoF.load() == false)
    {
        if (m_verbose > 5)
            cerr << lock_ios()
                 << "\n[" << m_id << "] Detect codec (try " << ++idx << ")\n";

        if (open_spdif())
        {
            PrintState("SPDIF");
            return true;
        }
        if (idx > 11)
        {
#if 0
            std::raise(SIGHUP);
#endif
            break;
        }
    }

    setEoF();
    return false;
}

int AudioBuffer::Size(void) const
{
    if (m_write_loop_cnt > m_read_loop_cnt)
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
    m_codec_changed_thread = std::thread(&AudioIO::codec_changed, this);
}

AudioIO::~AudioIO(void)
{
    m_running.store(false);
    if (m_codec_changed_thread.joinable())
        m_codec_changed_thread.join();
}

void AudioIO::Shutdown(void)
{
    m_running.store(false);

    const std::unique_lock<std::mutex> lock(m_buffer_mutex);

    buffer_que_t::iterator Ibuf;
    for (Ibuf = m_buffer_q.begin(); Ibuf != m_buffer_q.end(); ++Ibuf)
        (*Ibuf).setEoF();
}

bool AudioIO::AddBuffer(uint8_t* Pbegin, uint8_t* Pend,
                        int num_channels, bool is_lpcm,
                        int bytes_per_sample, int sample_rate,
                        int samples_per_frame, int frame_size,
                        int64_t* timestamps)
{
    {
        const std::unique_lock<std::mutex> lock(m_buffer_mutex);
        buffer_que_t::iterator Ibuf;

        if (m_running.load() == false)
            return false;

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

#if 0
    m_codec_initialized = false;
#endif
    StateChanged("AddBuffer");

    return true;
}

bool AudioIO::RescanSPDIF(void)
{
    if (!m_buffer_q.empty())
        return m_buffer_q.begin()->RescanSPDIF();
    return false;
}

int AudioIO::BufId(void) const
{
    const std::unique_lock<std::mutex> lock(m_buffer_mutex);

    if (m_buffer_q.empty())
        return 0;

    buffer_que_t::const_iterator Ibuf = m_buffer_q.begin();
    return (*Ibuf).Id();
}

bool AudioIO::Ready(void) const
{
    const std::unique_lock<std::mutex> lock(m_buffer_mutex);

    if (m_buffer_q.empty())
        return 0;

    buffer_que_t::const_iterator Ibuf = m_buffer_q.begin();
    return (*Ibuf).Initialized();
}

int AudioIO::Size(void) const
{
    const std::unique_lock<std::mutex> lock(m_buffer_mutex);

    if (m_buffer_q.empty())
        return 0;

    int sz = 0;
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

bool AudioIO::BlockReady(void) const
{
    const std::unique_lock<std::mutex> lock(m_buffer_mutex);

    if (m_buffer_q.empty())
    {
        cerr << lock_ios()
             << "q empty\n";
        return false;
    }

    buffer_que_t::const_iterator Ibuf = m_buffer_q.begin();
    return (*Ibuf).Size() > (*Ibuf).BlockSize();
}

int AudioIO::Add(uint8_t* Pframe, int len, int64_t timestamp)
{
//    const std::unique_lock<std::mutex> lock(m_buffer_mutex);

    if (m_buffer_q.empty())
    {
        cerr << lock_ios()
             << "WARNING: No audio buffers to Add to\n";
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
        cerr << lock_ios()
             << "WARNING: No audio buffers to Seek in\n";
        return 0;
    }
    buffer_que_t::iterator Ibuf = m_buffer_q.begin();
    return (*Ibuf).Seek(offset, whence);
}

int AudioIO::Read(uint8_t* dest, int len)
{
    if (m_buffer_q.empty())
    {
        cerr << lock_ios()
             << "WARNING: No audio buffers to Read from\n";
        return 0;
    }

    buffer_que_t::iterator Ibuf = m_buffer_q.begin();
#if 0
    if ((*Ibuf).Id() > 1)
    {
        cerr << "[" << (*Ibuf).Id() << "] Read\n";
    }
#endif
    return (*Ibuf).Read(dest, len);
}

AVPacket* AudioIO::ReadSPDIF(void)
{
    if (m_buffer_q.empty())
    {
        cerr << lock_ios()
             << "WARNING: No audio buffers to Read from\n";
        return 0;
    }

    buffer_que_t::iterator Ibuf = m_buffer_q.begin();

#if 0
    if ((*Ibuf).Id() > 1)
    {
        cerr << "[" << (*Ibuf).Id() << "] ReadSPDIF\n";
    }
#endif

    return (*Ibuf).ReadSPDIF();
}

bool AudioIO::ChangePending(void)
{
    std::unique_lock<std::mutex> lock(m_codec_mutex);
    return m_codec_changed && !m_codec_initialized;
}

bool AudioIO::CodecChanged(bool & ready)
{
    std::unique_lock<std::mutex> lock(m_codec_mutex);
    ready = m_codec_initialized;
#if 0
    cerr << lock_ios() << "CodecChanged: "
         << (m_codec_changed ? "True" : "False") << " "
         << (m_codec_initialized ? "True" : "False") << endl;
#endif
    if (m_codec_changed && m_codec_initialized)
    {
        m_codec_initialized = false;
        m_codec_changed = false;
        return true;
    }
    return m_codec_changed;
}

void AudioIO::StateChanged(const string & where)
{
    {
        std::unique_lock<std::mutex> changed_lock(m_codec_mutex);
        m_state_changed = true;
    }
    if (m_verbose > 1)
        cerr << lock_ios() << "******************************* "
             << "AudioIO StateChanged by " << where << endl;
    m_changing.notify_one();
}

void AudioIO::codec_changed(void)
{
    while (m_running.load())
    {
        std::unique_lock<std::mutex> changed_lock(m_codec_mutex);
        m_changing.wait(changed_lock, /*  chrono::milliseconds(10)); */
                        [this]{ return m_state_changed || !m_running.load(); });
#if 1
    cerr << lock_ios() << " codec_changed woken up  ##########################\n";
#endif
        m_state_changed = false;

        if (!m_running.load())
            break;

        {
            const std::unique_lock<std::mutex> lock(m_buffer_mutex);
            while (!m_buffer_q.empty() && m_buffer_q.front().Flushed())
            {
#if 1
                cerr << lock_ios() << "**************** codec_changed: Popped stale buffer\n";
#endif
                m_buffer_q.pop_front();
            }

            if (m_buffer_q.empty())
            {
                m_codec_name.clear();
#if 1
                cerr << lock_ios() << " ############ Audio buffer Q empty\n";
#endif
                continue;
            }

            if (m_buffer_q.front().Initialized())
            {
                cerr << lock_ios() << "[" << m_buffer_q.front().Id()
                     << "] ########### Audio already init.\n";
                continue;
            }
        }

        cerr << lock_ios() << "$$$$$$$$$$$$$$$$$$ AUDIO STATE CHANGED\n";

        m_codec_changed = true;
        m_codec_initialized = false;
        changed_lock.unlock();

        buffer_que_t::iterator Ibuf = m_buffer_q.begin();
        if ((*Ibuf).CodecName().empty())
        {
            if (!(*Ibuf).DetectCodec())
            {
#if 0
                cerr << lock_ios()
                     << "Failed to detect S/PDIF\n";
#endif
                m_codec_name.clear();
                continue;
            }
        }

        if (m_codec_name != (*Ibuf).CodecName())
        {
            if (m_verbose > 1)
                cerr << lock_ios()
                     << "Audio codec '" << m_codec_name << "' -> '"
                     << (*Ibuf).CodecName() << "'" << endl;
            m_codec_name = (*Ibuf).CodecName();
        }

        if (m_sample_rate != (*Ibuf).SampleRate())
        {
            if (m_verbose > 1)
                cerr << lock_ios()
                     << "Audio sample rate " << m_sample_rate << " -> "
                     << (*Ibuf).SampleRate() << endl;
            m_sample_rate = (*Ibuf).SampleRate();
        }
        if (m_bytes_per_sample != (*Ibuf).BytesPerSample())
        {
            if (m_verbose > 1)
                cerr << lock_ios()
                     << "Audio bytes per sample " << m_bytes_per_sample << " -> "
                     << (*Ibuf).BytesPerSample() << endl;
            m_bytes_per_sample = (*Ibuf).BytesPerSample();
        }
        if (m_lpcm != (*Ibuf).LPCM())
        {
            if (m_verbose > 1)
                cerr << lock_ios()
                     << "Audio " << (m_lpcm ? "LPCM" : "Bitstream")
                     << " -> "
                     << ((*Ibuf).LPCM() ? "LPCM" : "Bitstream")
                     << endl;
            m_lpcm = (*Ibuf).LPCM();
        }

        char layout_str[64] {0};
        char new_layout_str[64] {0};
        AVChannelLayout  layout = (*Ibuf).ChannelLayout();
        av_channel_layout_describe(&m_channel_layout, layout_str, 64);
        av_channel_layout_describe(&layout, new_layout_str, 64);
        if (memcmp(layout_str, new_layout_str, 64) != 0)
            m_channel_layout = layout;

        changed_lock.lock();
        (*Ibuf).SetInit(true);
        m_codec_initialized = true;
    }
}
