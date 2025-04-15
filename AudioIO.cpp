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

    m_EoF                  = false;
    m_spdif_format_context = rhs.m_spdif_format_context;
    m_spdif_avio_context   = rhs.m_spdif_avio_context;
    m_spdif_avio_context_buffer = rhs.m_spdif_avio_context_buffer;
    m_spdif_codec          = rhs.m_spdif_codec;
    m_spdif_codec_id       = rhs.m_spdif_codec_id;

    m_lpcm                 = rhs.m_lpcm;
    av_channel_layout_copy(&m_channel_layout, &rhs.m_channel_layout);
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

    return *this;
}

bool AudioBuffer::operator==(const AudioBuffer & rhs)
{
    if (this == &rhs)
        return true;
    return false;
}

AudioBuffer::AudioBuffer(int num_channels, bool is_lpcm,
                         int bytes_per_sample, int sample_rate,
                         int samples_per_frame, int frame_size,
                         AudioIO* parent, int verbose, int id)
    : m_lpcm(is_lpcm)
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
    m_EoF.store(true);
}

void AudioBuffer::PurgeQueue(void)
{
    const unique_lock<mutex> lock(m_write_mutex);
    m_audio_queue.clear();
    m_probed_queue.clear();
    if (m_verbose > 1)
        cerr << lock_ios()
             << "[" << m_id << "] audio buffer cleared.\n";
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
             << ", TotalBytes: " << m_total_write
             << endl;
    }
}

bool AudioBuffer::Add(AudioFrame & buf, int64_t timestamp)
{
#if 1
    if (static_cast<int32_t>(buf.size()) != m_frame_size)
    {
        cerr << lock_ios() << "\n[" << m_id
             << "] WARNING: AudioBuffer::Add buf size "
             << buf.size() << " != " << m_frame_size << " frame size\n"
             << "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n";
        return false;
    }
#endif

    {
        const unique_lock<mutex> lock(m_write_mutex);
        {
            m_total_write += buf.size();
            m_audio_queue.push_back( {buf, timestamp} );
        }
    }

    m_data_avail.notify_one();
    return true;
}

int AudioBuffer::Read(uint8_t* buf, uint32_t len)
{
    uint8_t* dest = buf;
    size_t   pkt_sz;

    unique_lock<mutex> lock(m_write_mutex);
    if (m_audio_queue.empty())
    {
        if (m_EoF.load() == true)
        {
            if (m_verbose > 2)
            {
                cerr << lock_ios()
                     << "[" << m_id << "] AudioIO::Read: EOF\n";
            }
            m_flushed = true;
            return AVERROR_EOF;
        }

        m_data_avail.wait_for(lock, chrono::microseconds(500));
        if (m_audio_queue.empty())
            return 0;
    }

#if 0 // More reliable detection in OutputTS::write_bitstream_frame ?
    static size_t m_missaligned_pkts = 0;

    if ((len % m_frame_size) % 32 != 0)
    {
        if (m_missaligned_pkts > 3)
        {
            cerr << lock_ios() << "[" << m_id << "] " << m_missaligned_pkts
                 << " out of sync, resetting: "
                 << "Requested " << len
                 << " % " << m_frame_size << " = " << len % m_frame_size
                 << endl;
            SetReady(false);
            return 0;
        }
#if 0
        cerr << lock_ios() << "[" << m_id << "] Missaligned: "
             << m_missaligned_pkts << endl;
#endif
        ++m_missaligned_pkts;
    }
    else
        ++m_missaligned_pkts = 0;
#endif

    uint32_t frm = 0;
    while (frm + m_frame_size <= len)
    {
        pkt_sz = m_audio_queue.front().frame.size();
#if 1
        if (pkt_sz > static_cast<size_t>(m_frame_size))
        {
            cerr << lock_ios() << "\nWARNING: Invalid audio frame size queued: "
                 << pkt_sz << " bytes!\n" << "&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&\n";
            m_audio_queue.pop_front();
            break;
        }
#endif

        copy(m_audio_queue.front().frame.begin(),
             m_audio_queue.front().frame.end(), dest);

        dest += pkt_sz;
        frm += pkt_sz;

        if (m_probing)
            m_probed_queue.push_back(m_audio_queue.front());
        else
            ++m_pkts_read;

        m_parent->m_timestamp = m_audio_queue.front().timestamp;
        m_audio_queue.pop_front();
        if (m_audio_queue.empty())
            break;
    }

    m_total_read += frm;

    return frm;
}

AVPacket* AudioBuffer::ReadSPDIF(void)
{
    {
        unique_lock<mutex> lock(m_write_mutex);
        while (Size() < m_block_size && m_EoF.load() == false)
            m_data_avail.wait_for(lock, chrono::microseconds(500));
    }

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

    if (ret < 0)
    {
        av_packet_free(&pkt);
        if (ret != AVERROR_EOF && m_verbose > 0)
            cerr << lock_ios()
                 << "WARNING: [" << m_id
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

void AudioBuffer::initialized(void)
{
    unique_lock<mutex> lock(m_write_mutex);
    copy(m_probed_queue.begin(), m_probed_queue.end(),
         std::inserter(m_audio_queue, m_audio_queue.begin()));
    m_probed_queue.clear();
    m_probing = false;
    m_initialized = true;
    PrintState("Init");
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
                                              nullptr);
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

    open_spdif_context();
    m_spdif_format_context->correct_ts_overflow = 1;

    int try_cnt = 1;
    for (idx = 0; idx < try_cnt; ++idx)
    {
        if (m_EoF.load())
        {
            cerr << lock_ios()
                 << "WARNING: [" << m_id << "] Abort S/PDIF scan due EoF.\n";
            m_flushed = true;
            return false;
        }

        if ((ret = av_probe_input_buffer(m_spdif_avio_context,
                                         &fmt, "", nullptr, 0,
                                         0)) != 0)
        {
            cerr << lock_ios()
                 << "WARNING: [" << m_id << "] Failed to probe spdif input: "
                 << AVerr2str(ret) << endl;
            continue;
        }

        if (m_verbose > 1)
        {
            cerr << lock_ios()
                 << "[" << m_id << "] --> Detected fmt '" << fmt->name
                 << "' '" << fmt->long_name << "'" << endl;
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

#if 0
    cerr << lock_ios() << "Probed S/PDIF with "
         << (int)(m_read - m_begin) << " bytes "
         << (int)(m_read - m_begin) / m_frame_size << " buffer.\n";
#endif

    /* The AVIO buffer is not timestamp aware, so discard it
     */
    avio_flush(m_spdif_avio_context);
    avformat_flush(m_spdif_format_context);
    return true;
}

bool AudioBuffer::DetectCodec(void)
{
    cerr << lock_ios() << "Detecting codec\n";

    if (m_lpcm)
    {
         m_codec_name = "ac3";
         m_channel_layout = AV_CHANNEL_LAYOUT_STEREO;
         initialized();
         return true;
    }

    int idx = 0;
    m_probing = true;
    while (m_EoF.load() == false)
    {
        if (m_verbose > 5)
            cerr << lock_ios()
                 << "\n[" << m_id << "] Detect codec (try " << ++idx << ")\n";

        if (open_spdif())
        {
            PrintState("SPDIF");
            initialized();
            return true;
        }

        if (idx > 11)
        {
#if 0
            raise(SIGHUP);
#endif
            break;
        }

        unique_lock<mutex> lock(m_write_mutex);
        m_data_avail.wait_for(lock, chrono::microseconds(500));
    }

    initialized();
    setEoF();
    return false;
}

/************************************************
 * AudioIO
 ************************************************/
AudioIO::AudioIO(DiscardImageCallback discard, int verbose)
    : f_discard_images(discard)
    , m_verbose(verbose)
{
}

void AudioIO::Shutdown(void)
{
    if (m_running.exchange(false))
    {
        const std::unique_lock<std::mutex> lock(m_buffer_mutex);

        buffer_que_t::iterator Ibuf;
        for (Ibuf = m_buffer_q.begin(); Ibuf != m_buffer_q.end(); ++Ibuf)
            (*Ibuf).setEoF();
    }
}

bool AudioIO::AddBuffer(int num_channels, bool is_lpcm,
                        int bytes_per_sample, int sample_rate,
                        int samples_per_frame, int frame_size)
{
    {
        const unique_lock<mutex> lock(m_buffer_mutex);
        buffer_que_t::iterator Ibuf;

        if (m_running.load() == false)
            return false;

        if (!m_buffer_q.empty())
        {
            Ibuf = m_buffer_q.end() - 1;
            (*Ibuf).setEoF();
        }

        m_buffer_q.push_back(AudioBuffer(num_channels, is_lpcm,
                                         bytes_per_sample, sample_rate,
                                         samples_per_frame, frame_size,
                                         this, m_verbose, m_buf_id++));
        Ibuf = m_buffer_q.end() - 1;

        if (m_verbose > 2)
        {
            cerr << lock_ios()
                 << "[" << (*Ibuf).Id() << "] "
                 << "AddBuffer(num_channels = " << num_channels << "\n"
                 << "               is_lpcm = "
                 << (is_lpcm ? "true" : "false") << "\n"
                 << "      bytes_per_sample = " << bytes_per_sample << "\n"
                 << "           sample_rate = " << sample_rate << "\n"
                 << "     samples_per_frame = " << samples_per_frame << "\n"
                 << "            frame_size = " << frame_size << "\n"
                 << ")\n";
        }
    }

    return true;
}

bool AudioIO::RescanSPDIF(void)
{
    if (!m_buffer_q.empty())
        return m_buffer_q.begin()->DetectCodec();
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
    return (*Ibuf).isEoF() || (*Ibuf).Size() > (*Ibuf).BlockSize();
}


bool AudioIO::Add(AudioBuffer::AudioFrame & buf, int64_t timestamp)
{
    const unique_lock<mutex> lock(m_buffer_mutex);

    if (m_buffer_q.empty())
    {
        cerr << lock_ios()
             << "WARNING: No audio buffers to Add to\n";
        return 0;
    }

    buffer_que_t::iterator Ibuf = m_buffer_q.end() - 1;
#if 1
    if (static_cast<int32_t>(buf.size()) != (*Ibuf).FrameSize())
    {
        cerr << lock_ios() << "\nWARNING: AudioIO::Add buf size: "
             << buf.size() << " expected " << (*Ibuf).FrameSize() << "\n"
             << "%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%\n";
        return false;
    }
#endif

    return (*Ibuf).Add(buf, timestamp);
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
    return (*Ibuf).ReadSPDIF();
}

const AVChannelLayout* AudioIO::ChannelLayout(void) const
{
    const lock_guard<mutex> lock(m_buffer_mutex);
    if (m_buffer_q.empty())
        return nullptr;
    return m_buffer_q.begin()->ChannelLayout();
}

void AudioIO::PurgeQueue(void)
{
    const lock_guard<mutex> lock(m_buffer_mutex);
    if (m_buffer_q.empty())
        return;
    return m_buffer_q.begin()->PurgeQueue();
}

void AudioIO::Reset(const string & where)
{
    if (m_verbose > 2)
        cerr << lock_ios() << "AudioIO Reset by " << where << endl;
    const lock_guard<mutex> lock(m_buffer_mutex);
    if (m_buffer_q.empty())
        return;
    return m_buffer_q.begin()->SetReady(false);
}

bool AudioIO::CodecChanged(void)
{
    {
        const lock_guard<mutex> lock(m_buffer_mutex);
        while (!m_buffer_q.empty() && m_buffer_q.begin()->Flushed())
            m_buffer_q.pop_front();
        if (m_buffer_q.empty())
        {
            m_codec_name.clear();
            return false;
        }

        if (m_buffer_q.begin()->IsReady())
            return false;
    }

    buffer_que_t::iterator Ibuf = m_buffer_q.begin();
    if (!(*Ibuf).LPCM())
        f_discard_images(true);

    if (!(*Ibuf).DetectCodec())
    {
#if 1
        cerr << lock_ios()
             << "Failed to detect S/PDIF\n";
#endif
        m_codec_name.clear();
        return false;
    }

    if (!(*Ibuf).LPCM())
        f_discard_images(false);

    if (m_codec_name != (*Ibuf).CodecName())
    {
        if (m_verbose > 1)
            cerr << lock_ios()
                 << "Audio codec '" << m_codec_name << "' -> '"
                 << (*Ibuf).CodecName() << "'" << endl;
        m_codec_name = (*Ibuf).CodecName();
    }

    m_sample_rate = (*Ibuf).SampleRate();

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

    (*Ibuf).SetReady(true);

    return true;
}
