/*
 * Copyright (c) 2022-2025 John Patrick Poet
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "AudioIO.h"
#include "lock_ios.h"

#include <unistd.h>
#include <iostream>
#include <stdio.h>
#include <iomanip>
#include <csignal>

//#define DUMP_FFMPEG_BITSTREAM
#ifdef DUMP_FFMPEG_BITSTREAM
#include <fstream>
#endif


using namespace std;
using namespace s6_lock_ios;

/**
 * @brief Convert AV error code to string representation
 *
 * @param code Error code to convert
 * @return String representation of error code
 */
static std::string AVerr2str(int code)
{
    char astr[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_make_error_string(astr, AV_ERROR_MAX_STRING_SIZE, code);
    return string(astr);
}

/**
 * @brief Assignment operator implementation
 *
 * Copies all member variables from another AudioBuffer object
 *
 * @param rhs Right-hand side object to copy from
 * @return Reference to this object
 */
AudioBuffer& AudioBuffer::operator=(const AudioBuffer & rhs)
{
    if (this == &rhs)
        return *this;

    // Reset EOF flag
    m_EoF                  = false;

    // Copy S/PDIF context pointers
    m_spdif_format_context = rhs.m_spdif_format_context;
    m_spdif_avio_context   = rhs.m_spdif_avio_context;
    m_spdif_avio_context_buffer = rhs.m_spdif_avio_context_buffer;
    m_spdif_codec          = rhs.m_spdif_codec;
    m_spdif_codec_id       = rhs.m_spdif_codec_id;

    // Copy audio properties
    m_lpcm                 = rhs.m_lpcm;
    av_channel_layout_copy(&m_channel_layout, &rhs.m_channel_layout);
    m_codec_name           = rhs.m_codec_name;
    m_num_channels         = rhs.m_num_channels;
    m_bytes_per_sample     = rhs.m_bytes_per_sample;
    m_frame_size           = rhs.m_frame_size;
    m_samples_per_frame    = rhs.m_samples_per_frame;
    m_sample_rate          = rhs.m_sample_rate;
    m_block_size           = rhs.m_block_size;

    // Copy other properties
    m_parent               = rhs.m_parent;
    m_id                   = rhs.m_id;
    m_verbose              = rhs.m_verbose;

    return *this;
}

/**
 * @brief Equality operator implementation
 *
 * @param rhs Right-hand side object to compare with
 * @return true if objects are equal, false otherwise
 */
bool AudioBuffer::operator==(const AudioBuffer & rhs)
{
    if (this == &rhs)
        return true;
    return false;
}

/**
 * @brief Constructor for AudioBuffer
 *
 * Initializes an audio buffer with specified parameters and sets up basic properties
 *
 * @param num_channels Number of audio channels
 * @param is_lpcm Flag indicating if audio is LPCM format
 * @param bytes_per_sample Number of bytes per audio sample
 * @param sample_rate Audio sample rate
 * @param samples_per_frame Number of samples per audio frame
 * @param frame_size Size of audio frame in bytes
 * @param parent Pointer to parent AudioIO object
 * @param verbose Verbose level for logging
 * @param id Unique identifier for this buffer
 */
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
    // Calculate block size based on audio parameters
    m_block_size = 8 * m_bytes_per_sample * m_samples_per_frame * 8;
}

/**
 * @brief Destructor for AudioBuffer
 *
 * Sets EOF flag to signal destruction
 */
AudioBuffer::~AudioBuffer(void)
{
    m_EoF.store(true);
}

/**
 * @brief Clear all queued audio frames
 *
 * Acquires mutex lock and clears both audio and probed queues
 */
void AudioBuffer::PurgeQueue(void)
{
    const unique_lock<mutex> lock(m_write_mutex);
    m_audio_queue.clear();
    m_probed_queue.clear();
    if (m_verbose > 1)
        cerr << lock_ios()
             << "[" << m_id << "] audio buffer cleared.\n";
}

/**
 * @brief Print current buffer state for debugging
 *
 * @param where Location identifier for logging
 * @param force Force printing regardless of verbose level
 */
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

/**
 * @brief Add an audio frame to the buffer queue
 *
 * Adds an audio frame to the queue and notifies waiting readers
 *
 * @param buf Pointer to audio frame data to add
 * @param timestamp Timestamp for the frame
 * @return true if successful, false otherwise
 */
bool AudioBuffer::Add(AudioFrame *& buf, int64_t timestamp)
{
#if 0
    if (buf->size() != m_frame_size)
    {
        cerr << lock_ios() << "WARNING: Adding audio buffer with size "
             << buf->size() << ". expected " << m_frame_size << endl;
    }
#endif

    {
        // Acquire write mutex and add frame to queue
        const unique_lock<mutex> lock(m_write_mutex);
        {
            m_total_write += buf->size();
            m_audio_queue.push_back( {buf, timestamp} );
        }
    }

    // Notify waiting readers that new data is available
    m_data_avail.notify_one();
    return true;
}

/**
 * @brief Read audio data from buffer
 *
 * Reads audio data from the queue, handling EOF conditions and buffering
 *
 * @param buf Destination buffer to read into
 * @param len Length of data to read
 * @return Number of bytes read, or AVERROR_EOF on EOF
 */
int AudioBuffer::Read(uint8_t* buf, uint32_t len)
{
    uint8_t* dest = buf;
    size_t   pkt_sz;

    unique_lock<mutex> lock(m_write_mutex);

    // Wait for data to become available or EOF
    while (m_audio_queue.empty())
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
        if (m_probing)
            return 0;

        m_data_avail.wait_for(lock, chrono::microseconds(100));
    }

    int64_t ts = m_audio_queue.front().timestamp;

    AudioFrame* frame;
    uint32_t frm = 0;
    // Process frames until requested length is filled or queue is empty
    while (frm + m_frame_size <= len)
    {
        frame = m_audio_queue.begin()->frame;
        pkt_sz = frame->size();

        // Copy frame data to destination
        copy(frame->begin(), frame->end(), dest);

        dest += pkt_sz;
        frm += pkt_sz;

        // Move frame to probed queue if probing, otherwise delete it
        if (m_probing)
            m_probed_queue.push_back(m_audio_queue.front());
        else
        {
            ++m_pkts_read;
            delete frame;  // Important: This is where memory is freed
        }

        // Update timestamp if needed
        if (ts == m_parent->m_timestamp)
            ts = m_audio_queue.front().timestamp;

        // Remove processed frame from queue
        m_audio_queue.pop_front();
        if (m_audio_queue.empty())
            break;
    }

    // Update parent timestamp
    m_parent->m_timestamp = ts;
    m_total_read += frm;

    return frm;
}

/**
 * @brief Read audio frame from S/PDIF input
 *
 * Reads an audio frame directly from S/PDIF input using FFmpeg's av_read_frame
 *
 * @return Pointer to AVPacket containing audio frame, or nullptr on error
 */
AVPacket* AudioBuffer::ReadSPDIF(void)
{
    AVPacket* pkt = av_packet_alloc();
    if (!pkt)
    {
        cerr << lock_ios()
             << "WARNING: [" << m_id
             << "] Could not allocate pkt for spdif input.\n";
        return nullptr;
    }

    // Check if S/PDIF context is valid
    if (m_spdif_format_context == nullptr)
    {
        cerr << lock_ios()
             << "WARNING: [" << m_id << "] S/PDIF context is invalid.\n";
        return nullptr;
    }

    // Wait for data to become available or EOF
    while (m_audio_queue.empty())
    {
        if (m_EoF.load() == true)
            break;
        unique_lock<mutex> lock(m_write_mutex);
        m_data_avail.wait_for(lock, chrono::microseconds(100));
    }

    // Read frame from S/PDIF context
    int ret = av_read_frame(m_spdif_format_context, pkt);

    if (ret < 0)
    {
        // Free packet on error
        av_packet_free(&pkt);
        if (ret != AVERROR_EOF && m_verbose > 0)
            cerr << lock_ios()
                 << "WARNING: [" << m_id
                 << "] Failed to read spdif frame: (" << ret << ") "
                 << AVerr2str(ret) << endl;
        return nullptr;
    }

#ifdef DUMP_FFMPEG_BITSTREAM
    static ofstream fraw("ffmpeg-bitstream.bin", ofstream::binary);

    fraw.write(reinterpret_cast<const char*>(pkt->data), pkt->size);
#endif

    return pkt;
}

/**
 * @brief Read callback function for AVIO context
 *
 * This function is used as a callback for reading data through AVIO context
 *
 * @param opaque Pointer to AudioBuffer object
 * @param buf Buffer to read data into
 * @param buf_size Size of buffer
 * @return Number of bytes read, or negative error code
 */
static int read_packet(void* opaque, uint8_t* buf, int buf_size)
{
    AudioBuffer* q = reinterpret_cast<AudioBuffer* >(opaque);

    return q->Read(buf, buf_size);
}

/**
 * @brief Initialize buffer after probing
 *
 * Copies probed frames to main queue and sets initialization state
 */
void AudioBuffer::initialized(void)
{
    unique_lock<mutex> lock(m_write_mutex);
    // Move probed frames to main queue
    copy(m_probed_queue.begin(), m_probed_queue.end(),
         std::inserter(m_audio_queue, m_audio_queue.begin()));
    m_probed_queue.clear();
    m_probing = false;
    m_initialized = true;
    PrintState("Init");
}

/**
 * @brief Open S/PDIF format context
 *
 * Allocates and initializes the S/PDIF format context for audio input
 *
 * @return true if successful, false otherwise
 */
bool AudioBuffer::open_spdif_context(void)
{
    // Free existing context if present
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

    // Allocate new format context
    if (!(m_spdif_format_context = avformat_alloc_context()))
    {
        cerr << lock_ios()
             << "WARNING: [" << m_id
             << "] Unable to allocate spdif format context.\n";
        return false;
    }

    // Allocate buffer for AVIO context
    m_spdif_avio_context_buffer =
        reinterpret_cast<uint8_t* >(av_malloc(m_frame_size));
    if (!m_spdif_avio_context_buffer)
    {
        cerr << lock_ios()
             << "WARNING: [" << m_id
             << "] Unable to allocate spdif avio context buffer.\n";
        return false;
    }

    // Create AVIO context
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

    // Find S/PDIF input format
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

/**
 * @brief Open S/PDIF input for audio format detection
 *
 * Probes and opens S/PDIF input to detect audio codec parameters
 *
 * @return true if successful, false otherwise
 */
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

        // Probe input buffer for format detection
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

        // Find stream information
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
        av_channel_layout_copy(&m_channel_layout,
                               &audio_stream->codecpar->ch_layout);

        // Handle channel layout for EAC3 encoder compatibility
        if (m_channel_layout.nb_channels > 6)
            // HACK!  FFmpeg complains:
            /* Specified channel layout '7.1' is not supported by the
             * eac3 encoder
             */
            m_channel_layout = AV_CHANNEL_LAYOUT_5POINT1;

        m_sample_rate = audio_stream->codecpar->sample_rate;
        if (m_verbose > 1)
            cerr << "Bistream sample rate: " << m_sample_rate << "\n"
                 << "          frame size: "
                 << audio_stream->codecpar->frame_size
                 << endl;

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

/**
 * @brief Detect audio codec from input
 *
 * Detects audio codec by probing S/PDIF input or using LPCM parameters
 *
 * @return true if successful, false otherwise
 */
bool AudioBuffer::DetectCodec(void)
{
    cerr << lock_ios() << "Detecting codec\n";

    // Handle LPCM case directly
    if (m_lpcm)
    {
         m_codec_name = "ac3";
         if (m_num_channels == 2)
             m_channel_layout = AV_CHANNEL_LAYOUT_STEREO;
         else if (m_num_channels == 6)
             m_channel_layout = AV_CHANNEL_LAYOUT_5POINT1;
         else
         {
             cerr << lock_ios()
                  << "WARNING: " << m_num_channels
                  << " channels is not supported.\n";
             m_channel_layout = AV_CHANNEL_LAYOUT_STEREO;
         }
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
/**
 * @brief Constructor for AudioIO
 *
 * Initializes AudioIO with a discard callback and verbose level
 *
 * @param discard Callback function for discarding images
 * @param verbose Verbose level for logging
 */
AudioIO::AudioIO(DiscardImageCallback discard, int verbose)
    : f_discard_images(discard)
    , m_verbose(verbose)
{
}

/**
 * @brief Shutdown audio I/O system
 *
 * Signals all buffers to stop and sets running flag to false
 */
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

/**
 * @brief Add a new audio buffer with specified parameters
 *
 * Creates a new audio buffer and adds it to the queue
 *
 * @param num_channels Number of audio channels
 * @param is_lpcm Flag indicating LPCM format
 * @param bytes_per_sample Bytes per audio sample
 * @param sample_rate Audio sample rate
 * @param samples_per_frame Samples per audio frame
 * @param frame_size Size of audio frame in bytes
 * @return true if successful, false otherwise
 */
bool AudioIO::AddBuffer(int num_channels, bool is_lpcm,
                        int bytes_per_sample, int sample_rate,
                        int samples_per_frame, int frame_size)
{
    {
        const unique_lock<mutex> lock(m_buffer_mutex);
        buffer_que_t::iterator Ibuf;

        if (m_running.load() == false)
            return false;

        // Set EOF on previous buffer if exists
        if (!m_buffer_q.empty())
        {
            Ibuf = m_buffer_q.end() - 1;
            (*Ibuf).setEoF();
        }

        // Create new buffer and add to queue
        m_buffer_q.push_back(AudioBuffer(num_channels, is_lpcm,
                                         bytes_per_sample, sample_rate,
                                         samples_per_frame, frame_size,
                                         this, m_verbose, m_buf_id++));
        m_Iback = m_buffer_q.end() - 1;

        if (m_verbose > 2)
        {
            cerr << lock_ios()
                 << "[" << (*m_Iback).Id() << "] "
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

/**
 * @brief Rescan S/PDIF input for audio format detection
 *
 * @return true if successful, false otherwise
 */
bool AudioIO::RescanSPDIF(void)
{
    if (!m_buffer_q.empty())
        return m_buffer_q.begin()->DetectCodec();
    return false;
}

/**
 * @brief Get current buffer ID
 *
 * @return Current buffer ID
 */
int AudioIO::BufId(void) const
{
    const std::unique_lock<std::mutex> lock(m_buffer_mutex);

    if (m_buffer_q.empty())
        return 0;

    buffer_que_t::const_iterator Ibuf = m_buffer_q.begin();
    return (*Ibuf).Id();
}

/**
 * @brief Get total size of buffered audio data
 *
 * @return Total size in bytes
 */
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

/**
 * @brief Check if all buffers are empty
 *
 * @return true if empty, false otherwise
 */
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

/**
 * @brief Check if first buffer is ready for data
 *
 * @return true if ready, false otherwise
 */
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


/**
 * @brief Add audio frame to the last buffer
 *
 * @param buf Pointer to audio frame data
 * @param timestamp Timestamp for the frame
 * @return true if successful, false otherwise
 */
bool AudioIO::Add(AudioBuffer::AudioFrame *& buf, int64_t timestamp)
{
    const unique_lock<mutex> lock(m_buffer_mutex);

    if (m_buffer_q.empty())
    {
        cerr << lock_ios()
             << "WARNING: No audio buffers to Add to\n";
        return 0;
    }

#if 0
    buffer_que_t::iterator Ibuf = m_buffer_q.end() - 1;
    if (static_cast<int32_t>(buf.size()) != (*Ibuf).FrameSize())
    {
        cerr << lock_ios() << "\nWARNING: AudioIO::Add buf size: "
             << buf.size() << " expected " << (*Ibuf).FrameSize() << "\n"
             << "%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%\n";
        return false;
    }
#endif

    return (*m_Iback).Add(buf, timestamp);
}

/**
 * @brief Read audio data from first buffer
 *
 * @param dest Destination buffer to read into
 * @param len Length of data to read
 * @return Number of bytes read
 */
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

/**
 * @brief Read audio frame from S/PDIF input
 *
 * @return Pointer to AVPacket containing audio frame, or nullptr on error
 */
AVPacket* AudioIO::ReadSPDIF(void)
{
    if (m_buffer_q.empty())
    {
        cerr << lock_ios()
             << "WARNING: No audio buffers to Read from\n";
        return 0;
    }

    return m_buffer_q.begin()->ReadSPDIF();
}

/**
 * @brief Get channel layout from first buffer
 *
 * @return Pointer to channel layout structure or nullptr if empty
 */
const AVChannelLayout* AudioIO::ChannelLayout(void) const
{
    const lock_guard<mutex> lock(m_buffer_mutex);
    if (m_buffer_q.empty())
        return nullptr;
    return m_buffer_q.begin()->ChannelLayout();
}

/**
 * @brief Clear all audio queues
 *
 * Clears audio queues in the first buffer
 */
void AudioIO::PurgeQueue(void)
{
    const lock_guard<mutex> lock(m_buffer_mutex);
    if (m_buffer_q.empty())
        return;
    return m_buffer_q.begin()->PurgeQueue();
}

/**
 * @brief Reset audio system
 *
 * @param where Location identifier for logging
 */
void AudioIO::Reset(const string & where)
{
    if (m_verbose > 2)
        cerr << lock_ios() << "AudioIO Reset by " << where << endl;
    const lock_guard<mutex> lock(m_buffer_mutex);
    if (m_buffer_q.empty())
        return;
    return m_buffer_q.begin()->SetReady(false);
}

/**
 * @brief Check if audio codec has changed
 *
 * @return true if codec changed, false otherwise
 */
bool AudioIO::CodecChanged(void)
{
    {
        const lock_guard<mutex> lock(m_buffer_mutex);
        // Remove flushed buffers
        while (!m_buffer_q.empty() && m_buffer_q.begin()->Flushed())
            m_buffer_q.pop_front();
        if (m_buffer_q.empty())
        {
            m_codec_name.clear();
            return false;
        }

        // If buffer is already initialized, no change
        if (m_buffer_q.begin()->IsReady())
            return false;
    }

    buffer_que_t::iterator Ibuf = m_buffer_q.begin();
    if (!(*Ibuf).LPCM())
        f_discard_images(true);

    // Detect new codec
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

    // Update codec parameters if changed
    if (m_codec_name != (*Ibuf).CodecName())
    {
        if (m_verbose > 1)
            cerr << lock_ios()
                 << "Audio codec '" << m_codec_name << "' -> '"
                 << (*Ibuf).CodecName() << "'" << endl;
        m_codec_name = (*Ibuf).CodecName();
    }

    if (m_num_channels != (*Ibuf).NumChannels())
    {
        if (m_verbose > 1)
            cerr << lock_ios()
                 << "Audio channels " << m_num_channels << " -> "
                 << (*Ibuf).NumChannels() << "\n";
        m_num_channels = (*Ibuf).NumChannels();
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
