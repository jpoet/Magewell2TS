/*
 * Copyright (c) 2022 John Patrick Poet
 *
 * Based on:
 *     Muxing.c Copyright (c) 2003 Fabrice Bellard
 *     avio_reading.c Copyright (c) 2014 Stefano Sabatini
 *     encode_audio.c Copyright (c) 2001 Fabrice Bellard
 *     encode_video.c Copyright (c) 2001 Fabrice Bellard
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

/**
 * Output a Transport Stream
 */

#include <unistd.h>
#include <iostream>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sstream>
#include <thread>
#include <cstdlib>
#include <fcntl.h>
#include <chrono>

extern "C" {
#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>
#include <libavutil/imgutils.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

#define BURST_HEADER_SIZE 0x4
#define SYNCWORD1 0xF872
#define SYNCWORD2 0x4E1F

#include "OutputTS.h"

using namespace std;

std::string AV_ts2str(int64_t ts)
{
    char astr[AV_TS_MAX_STRING_SIZE] = { 0 };
    av_ts_make_string(astr, ts);
    return string(astr);
}

std::string AV_ts2timestr(int64_t ts, AVRational* tb)
{
    ostringstream os;
    os << av_q2d(*tb) * ts;
    return os.str();
}

static void log_packet(string where, const AVFormatContext* fmt_ctx,
                       const AVPacket* pkt)
{
    AVRational* time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

    cerr << where << "[" << pkt->stream_index << "] pts: " << pkt->pts
         << " pts_time: " << AV_ts2timestr(pkt->pts, time_base)
         << " dts: " << AV_ts2str(pkt->dts)
         << " dts_time: " << AV_ts2timestr(pkt->dts, time_base)
         << " duration: " << AV_ts2str(pkt->duration)
         << " duration_time: " << AV_ts2timestr(pkt->duration, time_base)
         << endl;
}

AudioIO::AudioIO(int verbose)
    : m_verbose(verbose)
{
}

void AudioIO::AddBuffer(uint8_t* begin, uint8_t* end,
                        int frame_size, bool lpcm,
                        int64_t* timestamps, size_t frame_count)
{
    const std::unique_lock<std::mutex> lock(m_mutex);

    m_buffer_q.push_back(buffer_t(frame_size, begin, end, lpcm,
                                  timestamps, frame_count));
    buffer_que_t::iterator Ibuf = m_buffer_q.end() - 1;
    (*Ibuf).m_own_buffer = true;

    print_pointers(m_buffer_q.back(), "AddBuffer");
}

void AudioIO::print_pointers(const buffer_t & buffer,
                             const string & where, bool force) const
{
    if (force || m_verbose > 4)
    {
        cerr << where << ": "
             << " begin: " << (uint64_t)(buffer.begin)
             << ", end : " << (size_t)(buffer.end - buffer.begin)
             << ", write : " << (size_t)(buffer.write - buffer.begin)
             << ", read : " << (size_t)(buffer.read - buffer.begin)
             << ", frame sz: " << buffer.frame_size
             << ", wrapped: " << (buffer.write_wrapped ? "Yes, " : "No, ")
             << (buffer.lpcm ? " LPCM" : " bistream")
             << " codec: " << buffer.codec_name
             << ", timestamp: " << buffer.m_timestamp << endl;
    }
}

AudioIO::buffer_t& AudioIO::buffer_t::operator=(const buffer_t & rhs)
{
    if (this == &rhs)
        return *this;

    frame_size = rhs.frame_size;
    begin = rhs.begin;
    end = rhs.end;
    write = rhs.write;
    read = rhs.read;
    prev_frame = rhs.prev_frame;
    lpcm = rhs.lpcm;
    write_wrapped = rhs.write_wrapped;
    has_wrapped = rhs.has_wrapped;
    codec_name = rhs.codec_name;

    m_timestamps = rhs.m_timestamps;
    m_frame_cnt = rhs.m_frame_cnt;

    m_own_buffer = false;

    return *this;
}

bool AudioIO::buffer_t::operator==(const buffer_t & rhs)
{
    if (this == &rhs)
        return true;
    return begin == rhs.begin;
}

int64_t AudioIO::buffer_t::TimeStamp(uint8_t* P) const
{
    size_t idx = static_cast<uint32_t>(P - begin) / frame_size;
#if 1
    float fidx = (P - begin) / frame_size;
    if ((float)idx != fidx)
        cerr << "buffer_t::TimeStamp: calced idx: " << fidx << endl;
#endif
    return m_timestamps[idx];
}

size_t AudioIO::Size(void) const
{
    if (m_buffer_q.empty())
        return 0;

    size_t sz = 0;
    buffer_que_t::const_iterator Ibuf;
    for (Ibuf = m_buffer_q.begin(); Ibuf != m_buffer_q.end(); ++Ibuf)
    {
        if ((*Ibuf).write_wrapped)
            sz += ((*Ibuf).end - (*Ibuf).read) +
                  ((*Ibuf).write - (*Ibuf).begin);
        else
            sz += (*Ibuf).write - (*Ibuf).read;
    }
    return sz;
}

bool AudioIO::Empty(void) const
{
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

int64_t AudioIO::TimeStamp(void) const
{
    if (m_buffer_q.empty())
        return 0LL;
    return m_buffer_q.begin()->m_timestamp;
}

string AudioIO::CodecName(void) const
{
    if (m_buffer_q.empty())
        return string();
    return m_buffer_q.begin()->codec_name;
}

void AudioIO::SetCodecName(const string & rhs)
{
    if (!m_buffer_q.empty())
        m_buffer_q.begin()->codec_name = rhs;
}

int AudioIO::Add(uint8_t* Pframe, size_t len, int64_t timestamp)
{
    const std::unique_lock<std::mutex> lock(m_mutex);

    if (m_buffer_q.empty())
    {
        cerr << "ERROR: No audio buffers to Add to\n";
        return 0;
    }
    buffer_que_t::iterator Ibuf = m_buffer_q.end() - 1;

    (*Ibuf).write = Pframe + len;
    print_pointers(*Ibuf, "      Add");

    if ((*Ibuf).write > (*Ibuf).end)
    {
        cerr << "ERR: Add audio to " << (uint64_t)Pframe
             << " - " << (*Ibuf).write
             << " which is greater than the end " << (uint64_t)(*Ibuf).end
             << endl;
    }

    if ((*Ibuf).prev_frame == (*Ibuf).end)
    {
        (*Ibuf).has_wrapped = true;
        (*Ibuf).write_wrapped = true;
        if (m_verbose > 4)
            cerr << "AudioIO::Add: wrapped\n";
    }
    else if (Pframe < (*Ibuf).prev_frame)
    {
        cerr << "ERR: Add audio to " << (uint64_t)Pframe
             << " which is less than " << (uint64_t)(*Ibuf).prev_frame
             << " but not the begining " << (uint64_t)(*Ibuf).begin
             << endl;
    }
    (*Ibuf).prev_frame = (*Ibuf).write;

    if ((*Ibuf).write_wrapped && (*Ibuf).read < (*Ibuf).write)
    {
        if (m_verbose > 0 && (*Ibuf).read != (*Ibuf).begin)
        {
            cerr << "ERR: Overwrote buffer begin, moving read\n";
            print_pointers(*Ibuf, "      Add", true);
            (*Ibuf).read = (*Ibuf).write;
        }
    }

    return 0;
}

int AudioIO::Read(uint8_t* dest, size_t len)
{
    const std::unique_lock<std::mutex> lock(m_mutex);
    uint8_t* Pend;
    size_t   sz;

    if (m_buffer_q.empty())
    {
        cerr << "ERROR: No audio buffers to Read from\n";
        return 0;
    }

    buffer_que_t::iterator Ibuf = m_buffer_q.begin();

    if ((*Ibuf).Empty())
    {
        if (m_buffer_q.size() > 1)
        {
            if (m_verbose > 2)
                cerr << " AduioIO::Read: EOF\n";
            return AVERROR_EOF;
        }
        if (m_verbose > 5)
            cerr << "AudioIO::Read: buffer is empty\n";
        return 0;
    }

    if ((*Ibuf).write_wrapped)
    {
        if ((*Ibuf).read == (*Ibuf).end)
        {
            Pend = (*Ibuf).write;
            (*Ibuf).read = (*Ibuf).begin;
            (*Ibuf).write_wrapped = false;
        }
        else if ((*Ibuf).read > (*Ibuf).end)
        {
            cerr << "Read has passed the end!\n";
            cerr << "Audio write: " << (size_t)((*Ibuf).write - (*Ibuf).begin)
                 << " read: " << (size_t)((*Ibuf).read - (*Ibuf).begin)
                 << " end: " << (size_t)((*Ibuf).end - (*Ibuf).begin)
                 << endl;
            exit(-1);
        }
        else
            Pend = (*Ibuf).end;
    }
    else
    {
        if ((*Ibuf).read > (*Ibuf).write)
        {
            cerr << "ERR: Read has passed Write!\n";
            print_pointers(*Ibuf, "     Read", true);
            exit(-1);
        }
        Pend = (*Ibuf).write;
    }

    if (Pend - (*Ibuf).read < len)
    {
        sz = Pend - (*Ibuf).read;
        if (m_verbose > 4)
        {
            cerr << "AudioIO::Read: Requested " << len
                 << " bytes, but only " << sz << " bytes available\n";
            m_report_next = 10;
        }
    }
    else
        sz = len;

    (*Ibuf).m_timestamp = (*Ibuf).TimeStamp((*Ibuf).read);
    if (dest != nullptr)
        memcpy(dest, (*Ibuf).read, sz);
    (*Ibuf).read += sz;
    print_pointers(*Ibuf, "     Read", m_report_next);
    if (m_report_next > 0)
        --m_report_next;

    return sz;
}

int64_t AudioIO::Seek(int64_t offset, int whence)
{
    const std::unique_lock<std::mutex> lock(m_mutex);
    int force = whence & AVSEEK_FORCE;
    whence &= ~AVSEEK_FORCE;
    int size = whence & AVSEEK_SIZE;
    whence &= ~AVSEEK_SIZE;

    if (force)
        return -1;

#if 1
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
          cerr << "whence = " << whence << endl;
          break;
    }
    if (m_verbose > 3)
        cerr << "Seeking from " << whence_str << " to " << offset << endl;
#endif

    if (m_buffer_q.empty())
    {
        cerr << "ERROR: No audio buffers to Seek in\n";
        return 0;
    }
    buffer_que_t::iterator Ibuf = m_buffer_q.begin();

    if ((*Ibuf).read == (*Ibuf).write)
        return 0;

    int64_t desired = offset;

    if (whence == SEEK_END)
    {
        (*Ibuf).read = (*Ibuf).write + (*Ibuf).frame_size;
        if (desired > 0)
            return -1;
    }
    else if (whence == SEEK_SET)
    {
        cerr << "\nAudioIO whence == SEEK_SET\n\n";
        return -1;
    }
    else if (whence != SEEK_CUR)
        return -1;

    if (desired < 0)
    {
        desired = -desired; // To keep my head from exploading
        // Backwards
        if ((*Ibuf).write_wrapped)
        {
            if ((*Ibuf).read - desired < (*Ibuf).write)
                (*Ibuf).read = (*Ibuf).write;
            else
                (*Ibuf).read -= desired;
        }
        else
        {
            if ((*Ibuf).read - desired < (*Ibuf).begin)
            {
                if ((*Ibuf).has_wrapped)
                // Wrap
                    (*Ibuf).read = (*Ibuf).end - ((*Ibuf).begin - ((*Ibuf).read - desired));
                else
                    (*Ibuf).read = (*Ibuf).begin;
            }
            else
                (*Ibuf).read -= desired;
        }
    }
    else if (desired > 0)
    {
        size_t len = Read(nullptr, desired);
        if (len < desired)
            return -1;
        return 0;
    }

    (*Ibuf).m_timestamp = (*Ibuf).TimeStamp((*Ibuf).read);
    return 0;
}

bool AudioIO::Bitstream(void)
{
    const std::unique_lock<std::mutex> lock(m_mutex);

    return (m_buffer_q.empty() ? false : !m_buffer_q.front().lpcm);
}

bool AudioIO::BitstreamChanged(bool is_lpcm)
{
    const std::unique_lock<std::mutex> lock(m_mutex);
    if (m_buffer_q.empty())
        return true;
    if (m_buffer_q.back().lpcm != is_lpcm)
    {
        if (m_verbose > 0)
        {
            if (is_lpcm)
                cerr << "Bitstream -> LPCM\n";
            else
                cerr << "LPCM -> Bitstream\n";
        }
        return true;
    }
    return false;
}

bool AudioIO::CodecChanged(void)
{
    const std::unique_lock<std::mutex> lock(m_mutex);

    if (m_buffer_q.size() < 2)
        return false;

    string codec = m_buffer_q.front().codec_name;
    while (m_buffer_q.size() > 1 && m_buffer_q.front().Empty())
        m_buffer_q.pop_front();

    if (codec == m_buffer_q.front().codec_name)
        return false;

    if (m_verbose > 0)
        cerr << "New audio buffer codec: "
             << (m_buffer_q.front().lpcm ? "LPCM" : "Bitstream")
             << endl;

    return true;
}

static int read_packet(void* opaque, uint8_t* buf, int buf_size)
{
    AudioIO* q = reinterpret_cast<AudioIO* >(opaque);

    return q->Read(buf, buf_size);
}

static int64_t seek_packet(void* opaque, int64_t offset, int whence)
{
    AudioIO* q = reinterpret_cast<AudioIO* >(opaque);

    return q->Seek(offset, whence);
}


OutputTS::OutputTS(int verbose_level, const string & video_codec_name,
                   const string & preset, int quality, int look_ahead,
                   bool no_audio, const string & device,
                   MagCallback image_buffer_avail)
    : m_audioIO(verbose_level)
    , m_verbose(verbose_level)
    , m_video_codec_name(video_codec_name)
    , m_preset(preset)
    , m_quality(quality)
    , m_look_ahead(look_ahead)
    , m_no_audio(no_audio)
    , m_device("/dev/dri/" + device)
    , m_image_buffer_available(image_buffer_avail)
{
    if (m_video_codec_name.find("qsv") != string::npos)
        m_encoderType = EncoderType::QSV;
    else if (m_video_codec_name.find("vaapi") != string::npos)
        m_encoderType = EncoderType::VAAPI;
    else if (m_video_codec_name.find("nvenc") != string::npos)
        m_encoderType = EncoderType::NV;
    else
    {
        m_encoderType = EncoderType::UNKNOWN;
        cerr << "'" << m_video_codec_name << "' not supported.\n";
    }

    /* allocate the output media context */
    avformat_alloc_output_context2(&m_output_format_context,
                                   NULL, "mpegts", m_filename.c_str());
    if (!m_output_format_context)
    {
        cerr << "Could not create output format context.\n";
        m_error = true;
    }
}

/* Add an output stream. */
void OutputTS::add_stream(OutputStream* ost, AVFormatContext* oc,
                          const AVCodec* *codec)
{
    AVCodecContext* codec_context;
    int idx;

    ost->tmp_pkt = av_packet_alloc();
    if (!ost->tmp_pkt)
    {
        cerr << "Could not allocate AVPacket\n";
        exit(1);
    }

    ost->st = avformat_new_stream(oc, NULL);
    if (!ost->st)
    {
        cerr << "Could not allocate stream\n";
        exit(1);
    }
    ost->st->id = oc->nb_streams-1;
    codec_context = avcodec_alloc_context3(*codec);
    if (!codec_context)
    {
        cerr << "Could not alloc an encoding context\n";
        exit(1);
    }
    ost->enc = codec_context;

    ost->next_pts = 0;
    switch ((*codec)->type)
    {
        case AVMEDIA_TYPE_AUDIO:
          ost->enc->sample_fmt  = (*codec)->sample_fmts ?
                                  (*codec)->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
          ost->enc->bit_rate    = 192000;
          if ((*codec)->supported_samplerates)
          {
              ost->enc->sample_rate = (*codec)->supported_samplerates[0];
              for (idx = 0; (*codec)->supported_samplerates[idx]; ++idx)
              {
                  if ((*codec)->supported_samplerates[idx] == m_audio_sample_rate)
                  {
                      ost->enc->sample_rate = m_audio_sample_rate;
                      break;
                  }
              }
          }
          else
              ost->enc->sample_rate = 48000;
          av_channel_layout_copy(&ost->enc->ch_layout, &m_channel_layout);
          ost->st->time_base = (AVRational){ 1, ost->enc->sample_rate };

          if (ost->enc->codec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
          {
              ost->enc->thread_type = FF_THREAD_SLICE;
              if (m_verbose > 0)
                  cerr << " Audio = THREAD SLICE\n";
          }
          else if (ost->enc->codec->capabilities & AV_CODEC_CAP_FRAME_THREADS)
          {
              ost->enc->thread_type = FF_THREAD_FRAME;
              if (m_verbose > 0)
                  cerr << " Audio = THREAD FRAME\n";
          }

          if (m_verbose > 2)
          {
              cerr << "Audio time base " << ost->st->time_base.num << "/"
                   << ost->st->time_base.den << "\n";
          }
          break;

        case AVMEDIA_TYPE_VIDEO:
          ost->enc->codec_id = (*codec)->id;

//          ost->enc->bit_rate = m_video_bitrate;
          /* Resolution must be a multiple of two. */
          ost->enc->width    = m_input_width;
          ost->enc->height   = m_input_height;
          /* timebase: This is the fundamental unit of time (in
           * seconds) in terms of which frame timestamps are
           * represented. For fixed-fps content, timebase should be
           * 1/framerate and timestamp increments should be identical
           * to 1. */
          ost->st->time_base = AVRational{m_input_frame_rate.den,
              m_input_frame_rate.num};
          ost->enc->time_base       = ost->st->time_base;
#if 0
          ost->enc->gop_size      = 12; /* emit one intra frame every twelve frames at most */
#endif

          if (m_encoderType == EncoderType::QSV)
              ost->enc->pix_fmt = AV_PIX_FMT_QSV;
          else if (m_encoderType == EncoderType::VAAPI)
              ost->enc->pix_fmt = AV_PIX_FMT_VAAPI;
          else
              ost->enc->pix_fmt = AV_PIX_FMT_YUV420P;

          if (ost->enc->codec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
          {
              ost->enc->thread_type = FF_THREAD_SLICE;
              if (m_verbose > 0)
                  cerr << " Video = THREAD SLICE\n";
          }
          else if (ost->enc->codec->capabilities & AV_CODEC_CAP_FRAME_THREADS)
          {
              ost->enc->thread_type = FF_THREAD_FRAME;
              if (m_verbose > 0)
                  cerr << " Video = THREAD FRAME\n";
          }

          if (m_verbose > 2)
          {
              cerr << "Output stream< Video: " << ost->enc->width
                   << "x" << ost->enc->height
                   << " time_base: " << ost->st->time_base.num
                   << "/" << ost->st->time_base.den
                   << (m_interlaced ? 'i' : 'p')
                   << "\n";
          }
          break;

        default:
          break;
    }

    /* Some formats want stream headers to be separate. */
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        ost->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
}

bool OutputTS::open_audio(void)
{
    if (m_no_audio)
        return true;

    std::unique_lock<std::mutex> lock(m_audio_mutex);
    m_audio_cond.wait(lock, [&]{return m_audio_ready;});

    close_stream(m_output_format_context, &m_audio_stream);

    const AVCodec* audio_codec = nullptr;

    if (m_audioIO.Bitstream())
    {
        if (open_spdif())
        {
            m_audioIO.SetCodecName(m_spdif_codec->name);
            m_audio_codec_name = m_spdif_codec->name;
            m_channel_layout = AV_CHANNEL_LAYOUT_5POINT1;
            if (m_verbose > 0)
                cerr << "--> Bitstream: (" << m_audio_codec_name << ")\n";
        }
        else
        {
            m_audio_codec_name = "unknown bitstream";
        }
    }
    else
    {
        m_audio_codec_name = m_audioIO.CodecName();
        m_channel_layout = AV_CHANNEL_LAYOUT_STEREO;
    }

    audio_codec = avcodec_find_encoder_by_name(m_audio_codec_name.c_str());

    if (!audio_codec)
    {
        cerr << "Could not find audio encoder for '"
             << m_audio_codec_name << "'\n";
        m_error = true;
        return false;
    }

    add_stream(&m_audio_stream, m_output_format_context,
               &audio_codec);

    OutputStream* ost = &m_audio_stream;
    AVFormatContext* oc = m_output_format_context;
    const AVCodec* codec = audio_codec;
    AVCodecContext* c = m_audio_stream.enc;
    AVDictionary* opt = NULL;
    int nb_samples;
    int ret;

    if ((ret = avcodec_open2(c, codec, &opt)) < 0)
    {
        cerr << "Could not open audio codec: " << AVerr2str(ret) << endl;
        exit(1);
    }

    if (c->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
        nb_samples = 10000;
    else
        nb_samples = c->frame_size;

    ost->frame     = alloc_audio_frame(c->sample_fmt, &c->ch_layout,
                                      c->sample_rate, nb_samples);
    ost->tmp_frame = alloc_audio_frame(AV_SAMPLE_FMT_S16, &c->ch_layout,
                                      c->sample_rate, nb_samples);

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if (ret < 0)
    {
        cerr << "Could not copy the stream parameters\n";
        exit(1);
    }

    /* create resampler context */
    ost->swr_ctx = swr_alloc();
    if (!ost->swr_ctx)
    {
        cerr << "Could not allocate resampler context\n";
        exit(1);
    }

    /* set options */
    av_opt_set_chlayout  (ost->swr_ctx, "in_chlayout",
                          &c->ch_layout,     0);
    av_opt_set_int       (ost->swr_ctx, "in_sample_rate",
                          c->sample_rate,    0);
    av_opt_set_sample_fmt(ost->swr_ctx, "in_sample_fmt",
                          AV_SAMPLE_FMT_S16, 0);
    av_opt_set_chlayout  (ost->swr_ctx, "out_chlayout",
                          &c->ch_layout,     0);
    av_opt_set_int       (ost->swr_ctx, "out_sample_rate",
                          c->sample_rate,    0);
    av_opt_set_sample_fmt(ost->swr_ctx, "out_sample_fmt",
                          c->sample_fmt,     0);

    /* initialize the resampling context */
    if ((ret = swr_init(ost->swr_ctx)) < 0)
    {
        cerr << "Failed to initialize the resampling context\n";
        exit(1);
    }

    return true;
}

bool OutputTS::open_video(void)
{
    close_stream(m_output_format_context, &m_video_stream);

    AVDictionary* opt = NULL;
    const AVCodec* video_codec =
        avcodec_find_encoder_by_name(m_video_codec_name.c_str());
    if (video_codec)
    {
        if (m_verbose > 0)
        {
            cerr << "Video codec: " << video_codec->id << " : "
                 << video_codec->name << " '"
                 << video_codec->long_name << "' "
                 << endl;
        }
    }
    else
    {
        cerr << "Could not find video encoder for '"
             << m_video_codec_name << "'\n";
        m_error = true;
        return false;
    }

    /* Add the audio and video streams using the default format codecs
     * and initialize the codecs. */
    add_stream(&m_video_stream, m_output_format_context,
               &video_codec);

    /* Now that all the parameters are set, we can open the audio and
     * video codecs and allocate the necessary encode buffers. */
    switch (m_encoderType)
    {
        case EncoderType::QSV:
          if (!open_qsv(video_codec, &m_video_stream, opt))
              exit(1);
          break;
        case EncoderType::VAAPI:
          if (!open_vaapi(video_codec, &m_video_stream, opt))
              exit(1);
          break;
        case EncoderType::NV:
          if (!open_nvidia(video_codec, &m_video_stream, opt))
              exit(1);
          break;
        default:
          cerr << "Could not determine video encoder type.\n";
          m_error = true;
          exit(1);
    }

    return true;
}

bool OutputTS::open_container(void)
{
    int ret;
    AVDictionary* opt = NULL;

    m_fmt = m_output_format_context->oformat;

    if (m_verbose > 0)
        av_dump_format(m_output_format_context, 0, m_filename.c_str(), 1);

    /* open the output file, if needed */
    if (!(m_fmt->flags & AVFMT_NOFILE)) {
        ret = avio_open(&m_output_format_context->pb,
                        m_filename.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            cerr << "Could not open '" << m_filename << "': "
                 << AVerr2str(ret) << endl;
            return false;
        }
    }

    /* Write the stream header, if any. */
    ret = avformat_write_header(m_output_format_context, &opt);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file: %s\n",
                AVerr2str(ret));
        return false;
    }

    return true;
}

void OutputTS::setAudioParams(int num_channels, bool is_lpcm,
                              int bytes_per_sample, int sample_rate,
                              int samples_per_frame, int frame_size,
                              uint8_t* capture_buf, size_t capture_buf_size,
                              int64_t* timestamps, size_t frame_count)

{
    if (m_audioIO.BitstreamChanged(is_lpcm) || true)
    {
        m_audioIO.AddBuffer(capture_buf,
                            capture_buf + capture_buf_size,
                            frame_size, is_lpcm,
                            timestamps, frame_count);

        if (m_verbose > 1)
        {
            cerr << "setAudioParams " << (is_lpcm ? "LPCM" : "Bitstream") << endl;
        }

        m_audio_channels = num_channels;
        m_audio_bytes_per_sample = bytes_per_sample;
        m_audio_sample_rate = sample_rate;
        m_audio_samples_per_frame = samples_per_frame;

        m_audio_block_size = 8 * bytes_per_sample * m_audio_samples_per_frame;
        m_audio_ready = true;
        m_audio_cond.notify_one();
    }
}

void OutputTS::setVideoParams(int width, int height, bool interlaced,
                              AVRational time_base, double frame_duration,
                              AVRational frame_rate)
{
    m_input_width = width;
    m_input_height = height;
    m_interlaced = interlaced;
    m_input_time_base = time_base;
    m_input_frame_duration = frame_duration;
    m_input_frame_wait_ms = frame_duration / 10000 / 2;
    m_input_frame_rate = frame_rate;

    double fps = static_cast<double>(frame_rate.num) / frame_rate.den;

    if (m_verbose > 1)
    {
        cerr << "Video: " << width << "x" << height
             << " fps: " << fps
             << (m_interlaced ? 'i' : 'p')
             << "\n";

        if (m_verbose > 2)
            cerr << "Video Params set\n";
    }

    if (!m_initialized)
    {
        open_audio();
        open_video();
        open_container();

        if (m_image_ready_thread.joinable())
            m_image_ready_thread.join();
        m_image_ready_thread = std::thread(&OutputTS::Write, this);

        m_initialized = true;
    }
}

OutputTS::~OutputTS(void)
{
    if (m_output_format_context)
        av_write_trailer(m_output_format_context);

    /* Close each codec. */
    if (have_video)
        close_stream(m_output_format_context, &m_video_stream);
    if (have_audio)
        close_stream(m_output_format_context, &m_audio_stream);

    if (!(m_fmt && m_fmt->flags & AVFMT_NOFILE))
        /* Close the output file. */
        avio_closep(&m_output_format_context->pb);

    /* free the stream */
    avformat_free_context(m_output_format_context);
}

void OutputTS::addAudio(uint8_t* buf, size_t len, int64_t timestamp)
{
    m_audioIO.Add(buf, len, timestamp);
}

std::string OutputTS::AVerr2str(int code)
{
    char astr[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_make_error_string(astr, AV_ERROR_MAX_STRING_SIZE, code);
    return string(astr);
}

bool OutputTS::write_frame(AVFormatContext* fmt_ctx,
                           AVCodecContext* codec_ctx,
                           AVFrame* frame,
                           OutputStream* ost)
{
    int ret;
    AVPacket* pkt = ost->tmp_pkt;

    if (ost->prev_pts >= frame->pts)
        ++frame->pts;
    ost->prev_pts = frame->pts;

    // send the frame to the encoder
    ret = avcodec_send_frame(codec_ctx, frame);
    if (ret < 0)
    {
        if (m_verbose > 0)
        {
            cerr << "Error sending a frame to the encoder: "
                 << AVerr2str(ret) << "\n";
        }
        return false;
    }

    while (ret >= 0)
    {
        ret = avcodec_receive_packet(codec_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        else if (ret < 0)
        {
            if (m_verbose > 0)
            {
                cerr << "Error encoding a frame: AVerr2str(ret)\n";
            }
            return false;
        }

        /* rescale output packet timestamp values from codec to stream
         * timebase */
        av_packet_rescale_ts(pkt, codec_ctx->time_base, ost->st->time_base);

        pkt->stream_index = ost->st->index;

        if (ost->prev_dts >= pkt->dts)
            pkt->dts = ost->prev_dts + 1;
        ost->prev_dts = pkt->dts;

        if (pkt->pts < pkt->dts)
            pkt->pts = pkt->dts;

        /* Write the compressed frame to the media file. */

#if 0
        log_packet("write_frame", fmt_ctx, pkt);
#endif
        ret = av_interleaved_write_frame(fmt_ctx, pkt);
        /* pkt is now blank (av_interleaved_write_frame() takes ownership of
         * its contents and resets pkt), so that no unreferencing is necessary.
         * This would be different if one used av_write_frame(). */
        if (ret < 0)
        {
            if (m_verbose > 0)
            {
                cerr << "Error while writing packet: " << AVerr2str(ret)
                     << "\n";
            }

            if (m_verbose > 1)
            {
                cerr << "Codec time base " << codec_ctx->time_base.num
                     << "/" << codec_ctx->time_base.den
                     << "\n"
                     << "Stream          " << ost->st->time_base.num
                     << "/" << ost->st->time_base.den
                     << "\n";

                log_packet("write_frame", fmt_ctx, pkt);
            }
            return false;
        }
    }

    return ret == AVERROR_EOF ? false : true;
}


void OutputTS::close_stream(AVFormatContext* oc, OutputStream* ost)
{
    avcodec_free_context(&ost->enc);
    ost->enc = nullptr;
    av_frame_free(&ost->frame);
    ost->frame = nullptr;
    av_frame_free(&ost->tmp_frame);
    ost->tmp_frame = nullptr;
    av_packet_free(&ost->tmp_pkt);
    ost->tmp_pkt = nullptr;
    swr_free(&ost->swr_ctx);
    ost->swr_ctx = nullptr;
#if 0
    avcodec_free_context(&ost->st);
    ost->st == nullptr;
#endif
}

/**************************************************************/
/* audio output */

AVFrame* OutputTS::alloc_audio_frame(enum AVSampleFormat sample_fmt,
                                     const AVChannelLayout* channel_layout,
                                     int sample_rate, int nb_samples)
{
    AVFrame* frame = av_frame_alloc();
    int ret;

    if (!frame)
    {
        cerr << "Error allocating an audio frame\n";
        exit(1);
    }

    frame->format = sample_fmt;
    av_channel_layout_copy(&frame->ch_layout, channel_layout);
    frame->sample_rate = sample_rate;
    frame->nb_samples = nb_samples;

    if (nb_samples)
    {
        ret = av_frame_get_buffer(frame, 0);
        if (ret < 0)
        {
            cerr << "Error allocating an audio buffer\n";
            exit(1);
        }
    }

    return frame;
}

bool OutputTS::open_spdif_context(void)
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
        if (m_verbose > 0)
        {
            cerr << "Unable to allocate spdif format context.\n";
        }
        return false;
    }

    m_spdif_avio_context_buffer =
        reinterpret_cast<uint8_t* >(av_malloc(m_spdif_avio_context_buffer_size));
    if (!m_spdif_avio_context_buffer)
    {
        if (m_verbose > 0)
        {
            cerr << "Unable to allocate spdif avio context buffer.\n";
        }
        return false;
    }

    m_spdif_avio_context = avio_alloc_context(m_spdif_avio_context_buffer,
                                              m_spdif_avio_context_buffer_size,
                                              0,
                                              reinterpret_cast<void* >(&m_audioIO),
                                              read_packet,
                                              nullptr,
                                              seek_packet);
    if (!m_spdif_avio_context)
    {
        if (m_verbose > 0)
        {
            cerr << "Unable to allocate audio input avio context.\n";
        }
        return false;
    }

    m_spdif_format_context->pb = m_spdif_avio_context;

    const AVInputFormat* spdif_fmt = av_find_input_format("spdif");

    if (0 > avformat_open_input(&m_spdif_format_context, NULL,
                                spdif_fmt, NULL))
    {
        if (m_verbose > 0)
        {
            cerr << "Could not open spdif input.\n";
        }
        return false;
    }

    return true;
}

bool OutputTS::open_spdif(void)
{
    /* retrieve stream information */
    const AVInputFormat* fmt = nullptr;
    int ret;
    int idx;

    int try_cnt = 5;
    for (idx = 0; idx < try_cnt; ++idx)
    {
        open_spdif_context();

        if ((ret = av_probe_input_buffer(m_spdif_avio_context,
                                         &fmt, "", nullptr, 0,
                                         8 * m_audio_block_size)) != 0)
        {
            if (m_verbose > 0)
            {
                cerr << "Failed to probe spdif input: "
                     << AVerr2str(ret) << endl;
            }
            continue;
        }

        if (m_verbose > 1)
        {
            cerr << "--> Detected fmt '" << fmt->name
                 << "' '" << fmt->long_name << "'\n";
        }

        if (0 > avformat_find_stream_info(m_spdif_format_context, NULL))
        {
            if (m_verbose > 0)
            {
                cerr << "Could not find stream information\n";
            }
            continue;
        }

        if (m_spdif_format_context->nb_streams < 1)
        {
            if (m_verbose > 0)
            {
                cerr << "No streams found in SPDIF.\n";
            }
            continue;
        }

        int audio_stream_idx = 0;

        AVStream* audio_stream =
            m_spdif_format_context->streams[audio_stream_idx];
        if (audio_stream == nullptr)
        {
            if (m_verbose > 0)
            {
                cerr << "COULD NOT FIND AUDIO" << endl;
            }
        }

        if (m_verbose > 0)
        {
            /* dump input information to stderr */
            av_dump_format(m_spdif_format_context, 0, "pipe:0", 0);
        }

        if (!audio_stream)
        {
            if (m_verbose > 0)
            {
                cerr << "Could not find audio stream in spdif input.\n";
            }
            continue;
        }

        m_spdif_codec_id = audio_stream->codecpar->codec_id;

        break;
    }

    if (idx >= try_cnt)
    {
        cerr << "GAVE UP trying to find S/PDIF codec\n";
        exit(-1);
    }

    m_audioIO.Seek(-m_spdif_avio_context_buffer_size, SEEK_CUR);

    /* Find a decoder for the audio stream. */
    if (!(m_spdif_codec = avcodec_find_decoder(m_spdif_codec_id)))
    {
        if (m_verbose > 0)
        {
            cerr << "Could not find input codec "
                 << m_spdif_codec_id << "\n";
        }
        return false;
    }

    return true;
}

AVFrame* OutputTS::get_pcm_audio_frame(OutputStream* ost)
{
    AVFrame* frame = ost->tmp_frame;

    int j, i;
    uint8_t* q = (uint8_t*)frame->data[0];
    size_t got;

    size_t bytes = ost->enc->ch_layout.nb_channels * frame->nb_samples * 2;
    if (m_audioIO.Size() < bytes)
    {
        if (m_verbose > 5)
            cerr << "Not enough audio data.\n";
        return nullptr;
    }
    if ((got = m_audioIO.Read(q, bytes)) == 0)
        return nullptr;

    frame->pts = m_audioIO.TimeStamp();
#if 0
    if (frame->pts != ost->next_pts)
    {
        cerr << "WARNING: PCM audio TS " << frame->pts
             << " expected to be " << ost->next_pts
             << " diff " << frame->pts - ost->next_pts
             << " expected " << frame->nb_samples
             << endl;
    }
#endif
    ost->frame->pts = av_rescale_q(frame->pts, m_input_time_base,
                                   ost->enc->time_base);

    ost->next_pts = frame->pts + frame->nb_samples;

    return frame;
}

bool OutputTS::write_pcm_frame(AVFormatContext* oc, OutputStream* ost)
{
    AVCodecContext* enc_ctx = ost->enc;
    AVFrame* frame = get_pcm_audio_frame(ost);
    int dst_nb_samples = 0;
    int ret = 0;

    if (!frame)
        return false;

    /* convert samples from native format to destination codec format,
     * using the resampler */
    /* compute destination number of samples */
    dst_nb_samples = av_rescale(swr_get_delay(ost->swr_ctx,
                                              enc_ctx->sample_rate)
                                + frame->nb_samples,
                                enc_ctx->sample_rate,
                                enc_ctx->sample_rate);
    av_assert0(dst_nb_samples == frame->nb_samples);

    /* when we pass a frame to the encoder, it may keep a reference to it
     * internally;
     * make sure we do not overwrite it here
     */
    if (0 > av_frame_make_writable(ost->frame))
    {
        if (m_verbose > 0)
        {
            cerr << "write_pcm_frame: Failed to make frame writable\n";
        }
        return false;
    }

    /* convert to destination format */
    ret = swr_convert(ost->swr_ctx,
                      ost->frame->data, dst_nb_samples,
                      const_cast<const uint8_t** >(frame->data),
                      frame->nb_samples);
    if (ret < 0)
    {
        if (m_verbose > 0)
        {
            cerr << "write_pcm_frame: Error while converting\n";
        }
        return false;
    }

    frame = ost->frame;
    frame->pts = av_rescale_q(m_audioIO.TimeStamp(),
                              m_input_time_base,
                              enc_ctx->time_base);

    ost->samples_count += dst_nb_samples;

    return write_frame(oc, enc_ctx, frame, ost);
}

bool OutputTS::write_bitstream_frame(AVFormatContext* oc, OutputStream* ost)
{
    if (m_audioIO.Size() < m_spdif_avio_context_buffer_size)
    {
        if (m_verbose > 4)
            cerr << "write_bitstream_frame: Only "
                 << m_audioIO.Size() << " bytes available. "
                 <<  m_spdif_avio_context_buffer_size << " desired.\n";
        return false;
    }

    AVPacket* pkt = av_packet_alloc();
    if (!pkt)
    {
        if (m_verbose > 0)
        {
            cerr << "Could not allocate pkt for spdif input.\n";
        }
        return false;
    }

    double ret =  av_read_frame(m_spdif_format_context, pkt);
    if (0 > ret)
    {
        av_packet_free(&pkt);
        if (ret != AVERROR_EOF && m_verbose > 0)
            cerr << "Failed to read spdif frame: (" << ret << ") "
                 << AVerr2str(ret) << endl;
        return false;
    }

    pkt->pts = av_rescale_q(m_audioIO.TimeStamp(),
                            m_input_time_base,
                            ost->st->time_base);
    ost->next_pts = m_audioIO.TimeStamp() +
                    (ost->frame->nb_samples /* * m_audio_channels */);

    pkt->duration = pkt->pts;
    pkt->dts = pkt->pts;

    pkt->stream_index = ost->st->index;

    /* Write the frame to the media file. */
#if 0
    log_packet("write_bitstream_frame", oc, pkt);
#endif
    ret = av_interleaved_write_frame(oc, pkt);
    /* pkt is now blank (av_interleaved_write_frame() takes ownership of
     * its contents and resets pkt), so that no unreferencing is necessary.
     * This would be different if one used av_write_frame(). */
    if (ret < 0)
    {
        if (m_verbose > 0)
        {
            cerr << "Error while writing audio packet: " << AVerr2str(ret)
                 << "\n";
        }
        return false;
    }

    return true;
}

/*
 * encode one audio frame and send it to the muxer
 */
bool OutputTS::write_audio_frame(AVFormatContext* oc, OutputStream* ost)
{
#if 0
    cerr << "OutputTS::write_audio_frame\n";
#endif

    if (m_audioIO.CodecChanged())
    {
#if 0 // Take a step back for stability
        open_audio();
        open_container();
#endif
    }

    if (m_audioIO.Bitstream())
        return write_bitstream_frame(oc, ost);
    else
        return write_pcm_frame(oc, ost);
}



/**************************************************************/
/* video output */

AVFrame* OutputTS::alloc_picture(enum AVPixelFormat pix_fmt,
                                 int width, int height)
{
    AVFrame* picture;
    int ret;

    picture = av_frame_alloc();
    if (!picture)
        return nullptr;

    picture->format = pix_fmt;
    picture->width  = width;
    picture->height = height;

    /* allocate the buffers for the frame data */
    ret = av_frame_get_buffer(picture, 0);
    if (ret < 0)
    {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pix_fmt);
        cerr << "Could not allocate " << desc->name
             << " video frame of " << width << "x" << height
             << " : " << AV_ts2str(ret) << endl;
        return nullptr;
    }

    return picture;
}

bool OutputTS::open_nvidia(const AVCodec* codec,
                           OutputStream* ost, AVDictionary* opt_arg)
{
    int ret;
    AVCodecContext* ctx = ost->enc;
    AVDictionary* opt = NULL;

    av_dict_copy(&opt, opt_arg, 0);

    if (!m_preset.empty())
    {
        av_opt_set(ctx->priv_data, "preset", m_preset.c_str(), 0);
        if (m_verbose > 0)
            cerr << "Using preset " << m_preset << " for "
                 << m_video_codec_name << endl;
    }

    av_opt_set(ctx->priv_data, "tune", "hq", 0);
    av_opt_set(ctx->priv_data, "rc", "constqp", 0);

    av_opt_set_int(ctx->priv_data, "cq", m_quality, 0);
    if (m_look_ahead >= 0)
    {
        av_opt_set_int(ctx->priv_data, "rc-lookahead", m_look_ahead, 0);
        av_opt_set_int(ctx->priv_data, "surfaces", 50, 0);
    }
    av_opt_set_int(ctx->priv_data, "b", 0, 0);
    av_opt_set_int(ctx->priv_data, "minrate", 4000000, 0);
    av_opt_set_int(ctx->priv_data, "maxrate", 25000000, 0);
    av_opt_set_int(ctx->priv_data, "bufsize", 400000000, 0);

    av_opt_set_int(ctx->priv_data, "bf", 0, 0);
    av_opt_set_int(ctx->priv_data, "b_ref_mode", 0, 0);

    /* open the codec */
    ret = avcodec_open2(ctx, codec, &opt);
    av_dict_free(&opt);
    if (ret < 0)
    {
        if (m_verbose > 0)
            cerr << "Could not open video codec: "
                 << AVerr2str(ret) << "\n";
        m_error = true;
        return false;
    }

    /* allocate and init a re-usable frame */
    ost->frame = alloc_picture(ctx->pix_fmt, ctx->width, ctx->height);
    if (!ost->frame)
    {
        if (m_verbose > 0)
            cerr << "Could not allocate NVIDIA video frame\n";
        m_error = true;
        return false;
    }

    /* If the output format is not YUV420P, then a temporary YUV420P
     * picture is needed too. It is then converted to the required
     * output format. */
    ost->tmp_frame = NULL;
    if (ctx->pix_fmt != AV_PIX_FMT_YUV420P)
    {
        ost->tmp_frame = alloc_picture(AV_PIX_FMT_YUV420P,
                                       ctx->width, ctx->height);
        if (!ost->tmp_frame)
        {
            cerr << "Could not allocate temporary picture." << endl;
            m_error = true;
            return false;
        }
    }

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, ctx);
    if (ret < 0)
    {
        cerr << "Could not copy the stream parameters." << endl;
        m_error = true;
        return false;
    }

    return true;
}

bool OutputTS::open_vaapi(const AVCodec* codec,
                          OutputStream* ost, AVDictionary* opt_arg)
{
    int ret;
    AVCodecContext* ctx = ost->enc;
    AVDictionary* opt = nullptr;
    AVBufferRef* hw_frames_ref;
    AVHWFramesContext* frames_ctx = nullptr;

    av_dict_copy(&opt, opt_arg, 0);

    av_opt_set(ctx->priv_data, "rc_mode", "ICQ", 0);
//    av_opt_set_int(ctx->priv_data, "minrate", 1000, 0);
    av_opt_set_int(ctx->priv_data, "maxrate", 25000000, 0);
    av_opt_set_int(ctx->priv_data, "bufsize", 400000000, 0);
    av_opt_set_int(ctx->priv_data, "bf", 0, 0);
    av_opt_set_int(ctx->priv_data, "qp", 25, 0); // 25 is default

    vector<std::string> drivers{ "iHD", "i965" };
    vector<std::string>::iterator Idriver;
    for (Idriver = drivers.begin(); Idriver != drivers.end(); ++Idriver)
    {
        static string envstr = "LIBVA_DRIVER_NAME=" + *Idriver;
        char* env = envstr.data();
        putenv(env);

        if ((ret = av_hwdevice_ctx_create(&ost->hw_device_ctx,
                                          AV_HWDEVICE_TYPE_VAAPI,
                                          m_device.c_str(), opt, 0)) < 0)
            cerr << "Failed to open VAPPI driver '" << *Idriver << "'\n";
        else
            break;
    }
    if (Idriver == drivers.end())
    {
        cerr << "Failed to create a VAAPI device. Error code: "
             << AVerr2str(ret) << endl;
        m_error = true;
        return false;
    }

    if (m_verbose > 0)
        cerr << "Using VAAPI driver '" << *Idriver << "'\n";

    /* set hw_frames_ctx for encoder's AVCodecContext */
    if (!(hw_frames_ref = av_hwframe_ctx_alloc(ost->hw_device_ctx)))
    {
        cerr << "Failed to create VAAPI frame context.\n";
        m_error = true;
        return false;
    }
    frames_ctx = reinterpret_cast<AVHWFramesContext* >(hw_frames_ref->data);
    frames_ctx->format    = AV_PIX_FMT_VAAPI;
    frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width     = m_input_width;
    frames_ctx->height    = m_input_height;
    frames_ctx->initial_pool_size = 20;
    if ((ret = av_hwframe_ctx_init(hw_frames_ref)) < 0)
    {
        cerr << "Failed to initialize VAAPI frame context."
             << "Error code: " << AVerr2str(ret) << endl;
        av_buffer_unref(&hw_frames_ref);
        m_error = true;
        return false;
    }
    ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    if (!ctx->hw_frames_ctx)
    {
        ret = AVERROR(ENOMEM);
        cerr << "Failed to allocate hw frame buffer. "
             << "Error code: " << AVerr2str(ret) << endl;
        av_buffer_unref(&hw_frames_ref);
        m_error = true;
        return false;
    }
    av_buffer_unref(&hw_frames_ref);

    if ((ret = avcodec_open2(ctx, codec, &opt)) < 0)
    {
        cerr << "Cannot open video encoder codec. Error code: "
             << AVerr2str(ret) << endl;
        m_error = true;
        return false;
    }

    /* allocate and init a re-usable frame */
    ost->frame = alloc_picture(frames_ctx->sw_format,
                               frames_ctx->width, frames_ctx->height);
    if (!ost->frame)
    {
        if (m_verbose > 0)
            cerr << "Could not allocate VAAPI video frame\n";
        m_error = true;
        return false;
    }

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, ctx);
    if (ret < 0)
    {
        cerr << "Could not copy the stream parameters." << endl;
        m_error = true;
        return false;
    }

    return true;
}

bool OutputTS::open_qsv(const AVCodec* codec,
                        OutputStream* ost, AVDictionary* opt_arg)
{
    int    ret;

    AVCodecContext* ctx = ost->enc;
    AVDictionary* opt = nullptr;
    AVBufferRef* hw_frames_ref;
    AVHWFramesContext* frames_ctx = nullptr;

    av_dict_copy(&opt, opt_arg, 0);

    if (!m_preset.empty())
    {
        av_opt_set(ctx->priv_data, "preset", m_preset.c_str(), 0);
        if (m_verbose > 0)
            cerr << "Using preset " << m_preset << " for "
                 << m_video_codec_name << endl;
    }

    av_opt_set(ctx->priv_data, "scenario", "livestreaming", 0);
#if 0
    av_opt_set_int(ctx->priv_data, "extbrc", 1, 0);
    av_opt_set_int(ctx->priv_data, "adaptive_i", 1, 0);

//    av_opt_set_int(ctx->priv_data, "bf", 0, 0);
#endif

    ctx->global_quality = m_quality;

    if (m_look_ahead >= 0)
    {
        if (m_video_codec_name == "hevc_qsv")
            av_opt_set_int(ctx->priv_data, "look_ahead", 1, 0);
        av_opt_set_int(ctx->priv_data, "look_ahead_depth", m_look_ahead, 0);
    }
    av_opt_set_int(ctx->priv_data, "extra_hw_frames", m_look_ahead, 0);

    av_opt_set(ctx->priv_data, "skip_frame", "insert_dummy", 0);

    // Make sure env doesn't prevent QSV init.
    static string envstr = "LIBVA_DRIVER_NAME";
    char* env = envstr.data();
    unsetenv(env);

    av_dict_set(&opt, "child_device", m_device.c_str(), 0);
    if ((ret = av_hwdevice_ctx_create(&ost->hw_device_ctx,
                                      AV_HWDEVICE_TYPE_QSV,
                                      m_device.c_str(), opt, 0)) != 0)
    {
        cerr << "Failed to open QSV on " << m_device << "\n";
        return false;
    }

    if (m_verbose > 0)
        cerr << "Using QSV\n";

    /* set hw_frames_ctx for encoder's AVCodecContext */
    if (!(hw_frames_ref = av_hwframe_ctx_alloc(ost->hw_device_ctx)))
    {
        cerr << "Failed to create QSV frame context.\n";
        m_error = true;
        return false;
    }
    frames_ctx = reinterpret_cast<AVHWFramesContext* >(hw_frames_ref->data);
    frames_ctx->format    = AV_PIX_FMT_QSV;
    frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width     = m_input_width;
    frames_ctx->height    = m_input_height;
    frames_ctx->initial_pool_size = 20;
    if ((ret = av_hwframe_ctx_init(hw_frames_ref)) < 0)
    {
        cerr << "Failed to initialize QSV frame context."
             << "Error code: " << AVerr2str(ret) << endl;
        av_buffer_unref(&hw_frames_ref);
        m_error = true;
        return false;
    }
    ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    if (!ctx->hw_frames_ctx)
    {
        ret = AVERROR(ENOMEM);
        cerr << "Failed to allocate hw frame buffer. "
             << "Error code: " << AVerr2str(ret) << endl;
        av_buffer_unref(&hw_frames_ref);
        m_error = true;
        return false;
    }
    av_buffer_unref(&hw_frames_ref);

    if ((ret = avcodec_open2(ctx, codec, &opt)) < 0)
    {
        cerr << "Cannot open video encoder codec. Error code: "
             << AVerr2str(ret) << endl;
        m_error = true;
        return false;
    }

    /* allocate and init a re-usable frame */
    ost->frame = alloc_picture(frames_ctx->sw_format,
                               frames_ctx->width, frames_ctx->height);
    if (!ost->frame)
    {
        if (m_verbose > 0)
            cerr << "Could not allocate QSV video frame\n";
        m_error = true;
        return false;
    }

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, ctx);
    if (ret < 0)
    {
        cerr << "Could not copy the stream parameters." << endl;
        m_error = true;
        return false;
    }

    return true;
}

bool OutputTS::nv_encode(AVFormatContext* oc,
                         OutputStream* ost,
                         uint8_t* pImage,
                         int64_t  timestamp)
{
    AVCodecContext* ctx = ost->enc;

#if 0
    /* when we pass a frame to the encoder, it may keep a reference to it
     * internally; make sure we do not overwrite it here */
    if (av_frame_make_writable(ost->frame) < 0)
    {
        if (m_verbose > 0)
        {
            cerr << "get_video_frame: Make frame writable failed.\n";
        }
        exit(1);
    }
#endif

    // YUV 4:2:0
    size_t size = ctx->width * ctx->height;
    memcpy(ost->frame->data[0], pImage, size);
    memcpy(ost->frame->data[1],
           pImage + size, size / 4);
    memcpy(ost->frame->data[2], pImage + size * 5 / 4, size  / 4);
    m_image_buffer_available(pImage);

    ost->frame->pts = av_rescale_q_rnd(timestamp, m_input_time_base,
                                       ctx->time_base,
                      static_cast<AVRounding>(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));

    ost->next_pts = timestamp + 1;

    return write_frame(oc, ost->enc, ost->frame, ost);
}

bool OutputTS::qsv_vaapi_encode(AVFormatContext* oc,
                                OutputStream* ost, uint8_t*  pImage,
                                int64_t timestamp)
{
    AVCodecContext* enc_ctx = ost->enc;
    static AVFrame* hw_frame = nullptr;
    int    ret;

    int64_t pts = av_rescale_q(timestamp, m_input_time_base,
                               enc_ctx->time_base);

    size_t size = enc_ctx->width * enc_ctx->height;
    memcpy(ost->frame->data[0], pImage, size);
    memcpy(ost->frame->data[1], pImage + size, size / 2);
    m_image_buffer_available(pImage);

    if (!(hw_frame = av_frame_alloc()))
    {
        cerr << "Failed to allocate hw frame.";
        m_error = true;
        return false;
    }

    if ((ret = av_hwframe_get_buffer(enc_ctx->hw_frames_ctx,
                                     hw_frame, 0)) < 0)
    {
        cerr << "Failed to get hw buffer: "
             << AV_ts2str(ret) << endl;
        m_error = true;
        return false;
    }

    if (!hw_frame->hw_frames_ctx)
    {
        cerr << "Failed to allocate hw frame CTX.\n";
        m_error = true;
        return false;
    }

    if ((ret = av_hwframe_transfer_data(hw_frame,
                                        ost->frame, 0)) < 0)
    {
        cerr << "Error while transferring frame data to surface: "
             << AV_ts2str(ret) << endl;
        m_error = true;
        return false;
    }

    hw_frame->pts = pts;
    ost->next_pts = timestamp + 1;

    ret = write_frame(oc, enc_ctx, hw_frame, ost);
    av_frame_free(&hw_frame);

    return ret;
}

void OutputTS::Write(void)
{
    std::unique_lock<std::mutex> lock(m_imagepkt_mutex);

    uint8_t* pImage;
    uint64_t timestamp;

    for (;;)
    {
        m_image_ready.wait_for(lock,
                               std::chrono::milliseconds(m_input_frame_wait_ms));

        for (;;)
        {
            if (!m_no_audio)
            {
                while (m_video_stream.next_pts > m_audio_stream.next_pts)
                {
                    if (!write_audio_frame(m_output_format_context,
                                           &m_audio_stream))
                        break;
                }
            }

            {
                const std::unique_lock<std::mutex> lock(m_imagequeue_mutex);
                if (m_imagequeue.empty())
                    break;

                pImage    = m_imagequeue.front().image;
                timestamp = m_imagequeue.front().timestamp;
                m_imagequeue.pop_front();
            }

            if (m_encoderType == EncoderType::NV)
                nv_encode(m_output_format_context, &m_video_stream,
                          pImage, timestamp);
            else if (m_encoderType == EncoderType::QSV ||
                     m_encoderType == EncoderType::VAAPI)
                qsv_vaapi_encode(m_output_format_context, &m_video_stream,
                                 pImage, timestamp);
            else
            {
                cerr << "Unknown encoderType.\n";
                return;
            }
        }
    }
}

bool OutputTS::VideoFrame(uint8_t* pImage, uint32_t imageSize,
                          int64_t timestamp)
{
    const std::unique_lock<std::mutex> lock(m_imagequeue_mutex);
    m_imagequeue.push_back(imagepkt_t{timestamp, pImage});

    m_image_ready.notify_one();

    return true;
}
