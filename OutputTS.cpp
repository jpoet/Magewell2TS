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

extern "C" {
#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>
#include <libavutil/imgutils.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
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

PktQueue::PktQueue(int verbose)
    : m_Icur(m_queue.begin())
    , m_verbose(verbose)
{
}

int PktQueue::Push(uint8_t* buf, size_t len, int64_t timestamp)
{
    const std::unique_lock<std::mutex> lock(m_mutex);

    int idx = std::distance(m_queue.begin(), m_Icur);

    m_queue.push_back(pkt_t{timestamp, ++m_frame_num, vec_t(buf, buf+len)});

    m_size += len;
    m_Icur = m_queue.begin() + idx; // Iterator can become invalid

    if (m_verbose > 3)
    {
        cerr << "Pushed "
             << (*m_Icur).frame_num
             << " : " << (*m_Icur).timestamp
             << " : " << (*m_Icur).data.size() << " bytes\n";
    }

    return len;
}

int PktQueue::Pop(uint8_t* dest, size_t len)
{
    const std::unique_lock<std::mutex> lock(m_mutex);

    if (m_EOL)
    {
        if (m_verbose > 3)
        {
            cerr << "EOL triggered\n";
        }

        m_EOLtriggered = true;
        m_EOLtrigger.notify_one();

        return AVERROR_EOF;
    }

    size_t desired = len;
    size_t copied  = 0;

    if (m_remainder)
    {
        que_t::iterator Iprev = std::prev(m_Icur);

        m_timestamp = (*Iprev).timestamp;

        if (len < m_remainder)
        {
            memcpy(dest,
                   &((*Iprev).data.data()[(*Iprev).data.size() - m_remainder]),
                   len);

            m_remainder -= len;
            m_size -= len;

            return len;
        }

        memcpy(dest,
               &((*Iprev).data.data()[(*Iprev).data.size() - m_remainder]),
               m_remainder);

        if (m_verbose > 3)
        {
            cerr << "Popped "
                 << (*Iprev).frame_num
                 << " : " << (*Iprev).timestamp
                 << " : " << m_remainder
                 << " bytes @ " << (*Iprev).data.size() - m_remainder
                 << " " << (*Iprev).data.size()
                 << " in pkt.\n";
        }

        copied = m_remainder;
        desired -= m_remainder;
        m_remainder = 0;
    }

    if (m_Icur == m_queue.end())
    {
        m_size -= copied;
        return copied;
    }

    m_timestamp = (*m_Icur).timestamp;

    if (m_EOLtimestamp >= 0 && m_EOLtimestamp <= m_timestamp)
    {
        if (m_verbose > 3)
            cerr << "EOL set\n";
        m_EOL = true;
    }

    while (copied < len && m_Icur != m_queue.end())
    {
        if (desired < (*m_Icur).data.size())
        {
            memcpy(&dest[copied], (*m_Icur).data.data(), desired);
            m_remainder = (*m_Icur).data.size() - desired;

            if (m_verbose > 3)
            {
                cerr << "Popped " << (*m_Icur).frame_num
                     << " : " << (*m_Icur).timestamp
                     << " : " << desired << " bytes @ 0 with "
                     << m_remainder << " left with "
                     << (*m_Icur).data.size() << " total\n";
            }

            copied += desired;
            desired = 0;
        }
        else
        {
            memcpy(&dest[copied], (*m_Icur).data.data(),
                   (*m_Icur).data.size());

            if (m_verbose > 3)
            {
                cerr << "Popped " << (*m_Icur).frame_num
                     << " : " << (*m_Icur).timestamp
                     << " : " << (*m_Icur).data.size()
                     << " bytes @ 0 (all)\n";
            }

            copied += (*m_Icur).data.size();
            desired -= (*m_Icur).data.size();
        }

        ++m_Icur;
    }


    /* We must always save at least AVPROBE_PADDING_SIZE  to allow
       for seeking backwards. */
    int cached = std::distance(m_queue.begin(), m_Icur);
    if (cached > 256)
    {
        int idx = std::distance(m_queue.begin(), m_Icur);

        cached -= 256;
        m_queue.erase(m_queue.begin(), m_queue.begin() + cached);

        // Iterator should still be valid, but be safe
        m_Icur = m_queue.begin() + idx - cached;
    }

    m_size -= copied;

    if (m_verbose > 4)
    {
        cerr << "Popped " << m_timestamp << " : " << copied << " bytes, "
             << m_size << " left\n";
    }

    m_timestamp = std::prev(m_Icur)->timestamp;

    return copied;
}

int PktQueue::Seek(int64_t offset, int whence)
{
    const std::unique_lock<std::mutex> lock(m_mutex);

    int desired = offset;

    if (m_verbose > 3)
    {
        cerr << "Seek Size: " << m_size << " buffered: " << m_queue.size()
             << endl;
    }

    if (m_queue.empty())
        return 0;

    if (whence == SEEK_END)
    {
        m_Icur = m_queue.end();
        m_remainder = 0;

        if (desired < 0)
        {
            desired = -desired;
            do
            {
                --m_Icur;
                desired -= (*m_Icur).data.size();
            }
            while (desired > 0 && m_Icur != m_queue.begin());

            if (desired < 0)
            {
                m_remainder = (*m_Icur).data.size() + desired;
                ++m_Icur;
            }
        }
    }
    else if (whence == SEEK_CUR)
    {
        if (offset < 0)
        {
            desired = -desired;
            while (m_Icur != m_queue.begin() && desired > 0)
            {
                --m_Icur;
                desired -= ((*m_Icur).data.size() - m_remainder);
                m_remainder = 0;
            }
            if (desired < 0)
            {
                m_remainder = (*m_Icur).data.size() + desired;
                ++m_Icur;
            }
        }
        else
        {
            desired -= m_remainder;
            m_remainder = 0;

            while (m_Icur != m_queue.end() && desired > 0)
            {
                desired -= (*m_Icur).data.size();
                ++m_Icur;
            }

            if (desired < 0)
                m_remainder = std::prev(m_Icur)->data.size() + desired;
        }
    }
    else if (whence == SEEK_SET)
    {
        m_Icur = m_queue.begin();
        m_remainder = 0;

        if (offset > 0)
        {
            desired = offset;

            while (m_Icur != m_queue.end() && desired > 0)
            {
                desired -= (*m_Icur).data.size();
                ++m_Icur;
            }

            if (desired < 0)
                m_remainder = std::prev(m_Icur)->data.size() + desired;
        }
    }
    else
    {
        return -1;
    }

    m_size = CalcSize();
    return 0;
}

void PktQueue::SetEOL(bool val)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!val)
    {
        m_EOL = false;
        m_EOLtriggered = false;
        m_EOLtimestamp = -1;
        if (m_verbose > 3)
            cerr << "EOL cleared\n";
    }
    else
    {
        if (m_queue.empty())
        {
            m_EOL = true;
            if (m_verbose > 3)
                cerr << "EOL designated for empty queue\n";
        }
        else
        {
            m_EOLtimestamp = std::prev(m_queue.end())->timestamp;
            if (m_verbose > 3)
                cerr << "EOL designated @ "
                     << std::prev(m_queue.end())->timestamp
                     << "\n";
        }
    }
}

void PktQueue::WaitEOL(void)
{
    if (m_verbose > 3)
        cerr << "Waiting for EOL.\n";
    std::unique_lock<std::mutex> lock(m_mutex);
    m_EOLtrigger.wait(lock, [&]{return m_EOLtriggered;});
    if (m_verbose > 3)
        cerr << "EOL found.\n";
}

size_t PktQueue::CalcSize(void)
{
    int idx;
    int sz = m_remainder;

    que_t::iterator Iq = m_Icur;

    for (Iq = m_Icur; Iq != m_queue.end(); ++Iq)
        sz += (*Iq).data.size();

    return sz;
}

void PktQueue::Clear(void)
{
    const std::unique_lock<std::mutex> lock(m_mutex);

    m_queue.clear();
    m_Icur = m_queue.begin();
    m_size = 0;
    m_remainder = 0;
}

static int read_packet(void* opaque, uint8_t* buf, int buf_size)
{
    PktQueue* q = reinterpret_cast<PktQueue* >(opaque);

    return q->Pop(buf, buf_size);
}

#if 0
static int write_packet(void* opaque, const uint8_t* buf, int buf_size)
{
    cerr << "Write packet not implemented but it needs to be!\n";
    exit(1);
}
#endif

static int64_t seek_packet(void* opaque, int64_t offset, int whence)
{
    PktQueue* q = reinterpret_cast<PktQueue* >(opaque);

    return q->Seek(offset, whence);
}


OutputTS::OutputTS(int verbose_level, const string & video_codec_name,
                   int look_ahead, bool no_audio,
                   const string & device)
    : m_packet_queue(verbose_level)
    , m_verbose(verbose_level)
    , m_video_codec_name(video_codec_name)
    , m_look_ahead(look_ahead)
    , m_no_audio(no_audio)
    , m_device("/dev/dri/" + device)
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
}

void OutputTS::open_streams(void)
{
    const char* filename;
    int ret;
    int idx;

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
        if (m_verbose > 0)
        {
            cerr << "Could not find video encoder for '"
                 << m_video_codec_name << "'\n";
        }
        m_error = true;
        return;
    }

    const AVCodec* audio_codec = nullptr;
    if (!m_no_audio)
    {
        audio_codec =
            avcodec_find_encoder_by_name(m_audio_codec_name.c_str());

        if (!audio_codec)
        {
            if (m_verbose > 0)
            {
                cerr << "Could not find audio encoder for AC3\n";
            }
            m_error = true;
            return;
        }
    }

    filename = "pipe:1";

    /* allocate the output media context */
    avformat_alloc_output_context2(&m_output_format_context,
                                   NULL, "mpegts", filename);
    if (!m_output_format_context)
    {
        if (m_verbose > 0)
        {
            cerr << "Could not create output format context.\n";
        }
        m_error = true;
        return;
    }

    fmt = m_output_format_context->oformat;

    /* Add the audio and video streams using the default format codecs
     * and initialize the codecs. */
    add_stream(&m_video_stream, m_output_format_context,
               &video_codec);
    if (!m_no_audio)
    {
        add_stream(&m_audio_stream, m_output_format_context,
                   &audio_codec);
    }

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

    if (!m_no_audio)
        open_audio(m_output_format_context, audio_codec, &m_audio_stream, opt);

    if (m_verbose > 0)
    {
        av_dump_format(m_output_format_context, 0, filename, 1);
    }

    /* open the output file, if needed */
    if (!(fmt->flags & AVFMT_NOFILE)) {
        ret = avio_open(&m_output_format_context->pb,
                        filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            cerr << "Could not open '" << filename << "': "
                 << AVerr2str(ret) << endl;
            return;
        }
    }

    /* Write the stream header, if any. */
    ret = avformat_write_header(m_output_format_context, &opt);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file: %s\n",
                AVerr2str(ret));
        return;
    }
}

void OutputTS::setAudioParams(int num_channels, int bytes_per_sample,
                              int samples_per_frame)
{
    std::unique_lock<std::mutex> lock(m_detect_mutex);

    if (m_audio_detect)
        return;

    if (m_verbose > 1)
    {
        cerr << "setAudioParams\n";
    }

    m_audio_channels = num_channels;
    m_audio_bytes_per_sample = bytes_per_sample;
    m_audio_samples_per_frame = samples_per_frame;

    /* 8 channels */
    m_audio_block_size = 8 * bytes_per_sample * m_audio_samples_per_frame;
    m_audio_detect = true;

    if (m_verbose > 1)
    {
        cerr << "Audio Params set;  channels: " << m_audio_channels
             << " bytes_per_sample: " << m_audio_bytes_per_sample
             << " samples_per_frame: " << m_audio_samples_per_frame << "\n";
    }

    if (m_verbose > 2)
    {
        cerr << "Start audio detection thread.\n";
    }

    // AudioReady might not get called. Make sure last thread is cleaned up.
    if (m_audio_detect_thread.joinable())
        m_audio_detect_thread.join();
    m_audio_detect_thread = std::thread(&OutputTS::detect_audio, this);
}

void OutputTS::setVideoParams(int width, int height, bool interlaced,
                              AVRational time_base, AVRational frame_rate)
{
    m_interlaced = interlaced;

    if (m_verbose > 1)
    {
        cerr << "Video: " << width << "x" << height
             << " fps: "
             << static_cast<double>(frame_rate.num) / frame_rate.den
             << (m_interlaced ? 'i' : 'p')
             << "\n";
    }

    m_input_width = width;
    m_input_height = height;
    m_input_frame_rate = frame_rate;
    m_input_time_base = time_base;

    if (m_verbose > 2)
        cerr << "Video Params set\n";

    open_streams();
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

    if (!(fmt && fmt->flags & AVFMT_NOFILE))
        /* Close the output file. */
        avio_closep(&m_output_format_context->pb);

    /* free the stream */
    avformat_free_context(m_output_format_context);
}

void OutputTS::addPacket(uint8_t* buf, int buf_size, int64_t timestamp)
{
    m_packet_queue.Push(buf, buf_size, timestamp);
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

        pkt->stream_index = ost->st->index;
        /* rescale output packet timestamp values from codec to stream
         * timebase */
        av_packet_rescale_ts(pkt, codec_ctx->time_base, ost->st->time_base);

#if 0
        log_packet("write_frame", fmt_ctx, pkt);
#endif
        /* Write the compressed frame to the media file. */
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

/* Add an output stream. */
void OutputTS::add_stream(OutputStream* ost, AVFormatContext* oc,
                          const AVCodec* *codec)
{
    AVCodecContext* codec_context;
    int idx;

    ost->tmp_pkt = av_packet_alloc();
    if (!ost->tmp_pkt) {
        fprintf(stderr, "Could not allocate AVPacket\n");
        exit(1);
    }

    ost->st = avformat_new_stream(oc, NULL);
    if (!ost->st) {
        fprintf(stderr, "Could not allocate stream\n");
        exit(1);
    }
    ost->st->id = oc->nb_streams-1;
    codec_context = avcodec_alloc_context3(*codec);
    if (!codec_context) {
        fprintf(stderr, "Could not alloc an encoding context\n");
        exit(1);
    }
    ost->enc = codec_context;

    ost->next_timestamp = 0;
    switch ((*codec)->type) {
        case AVMEDIA_TYPE_AUDIO:
          ost->enc->sample_fmt  = (*codec)->sample_fmts ?
                                  (*codec)->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
          ost->enc->bit_rate    = 192000;
          ost->enc->sample_rate = 48000;
          if ((*codec)->supported_samplerates)
          {
              ost->enc->sample_rate = (*codec)->supported_samplerates[0];
              for (idx = 0; (*codec)->supported_samplerates[idx]; ++idx)
              {
                  if ((*codec)->supported_samplerates[idx] == 48000)
                  {
                      ost->enc->sample_rate = 48000;
                      break;
                  }
              }
          }
          av_channel_layout_copy(&ost->enc->ch_layout, &m_channel_layout);
          ost->st->time_base = (AVRational){ 1, ost->enc->sample_rate };

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

void OutputTS::close_stream(AVFormatContext* oc, OutputStream* ost)
{
    avcodec_free_context(&ost->enc);
    av_frame_free(&ost->frame);
    av_frame_free(&ost->tmp_frame);
    av_packet_free(&ost->tmp_pkt);
    sws_freeContext(ost->sws_ctx);
    swr_free(&ost->swr_ctx);
}

/**************************************************************/
/* audio output */

AVFrame* OutputTS::alloc_audio_frame(enum AVSampleFormat sample_fmt,
                                     const AVChannelLayout* channel_layout,
                                     int sample_rate, int nb_samples)
{
    AVFrame* frame = av_frame_alloc();
    int ret;

    if (!frame) {
        fprintf(stderr, "Error allocating an audio frame\n");
        exit(1);
    }

    frame->format = sample_fmt;
    av_channel_layout_copy(&frame->ch_layout, channel_layout);
    frame->sample_rate = sample_rate;
    frame->nb_samples = nb_samples;

    if (nb_samples) {
        ret = av_frame_get_buffer(frame, 0);
        if (ret < 0) {
            fprintf(stderr, "Error allocating an audio buffer\n");
            exit(1);
        }
    }

    return frame;
}

void OutputTS::open_audio(AVFormatContext* oc, const AVCodec* codec,
                          OutputStream* ost, AVDictionary* opt_arg)
{
    AVCodecContext* c;
    int nb_samples;
    int ret;
    AVDictionary* opt = NULL;

    c = ost->enc;

    /* open it */
    av_dict_copy(&opt, opt_arg, 0);
    ret = avcodec_open2(c, codec, &opt);
    av_dict_free(&opt);
    if (ret < 0) {
        fprintf(stderr, "Could not open audio codec: %s\n", AVerr2str(ret));
        exit(1);
    }

    /* init signal generator */
    ost->t     = 0;
    ost->tincr = 2 * M_PI * 110.0 / c->sample_rate;
    /* increment frequency by 110 Hz per second */
    ost->tincr2 = 2 * M_PI * 110.0 / c->sample_rate / c->sample_rate;

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
    if (ret < 0) {
        fprintf(stderr, "Could not copy the stream parameters\n");
        exit(1);
    }

    /* create resampler context */
    ost->swr_ctx = swr_alloc();
    if (!ost->swr_ctx) {
        fprintf(stderr, "Could not allocate resampler context\n");
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
    if ((ret = swr_init(ost->swr_ctx)) < 0) {
        fprintf(stderr, "Failed to initialize the resampling context\n");
        exit(1);
    }
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
                                              reinterpret_cast<void* >(&m_packet_queue),
                                              read_packet,
                                              0,
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
    uint8_t buf[BURST_HEADER_SIZE];

    const size_t probesize = m_audio_block_size * 3;
    const size_t max_nulls = m_audio_block_size * 2000;
    const size_t nulls_msg = m_audio_block_size * 200;

    for (idx = 0; idx < 10; ++idx)
    {
        int data_cnt = 0, null_cnt = 0, sz;

        if (m_verbose > 2)
        {
            cerr << "Attempt " << idx << " to open SPDIF audio stream.\n";
        }

        {
            m_packet_queue.SetEOL(true);
            m_packet_queue.WaitEOL();

            std::unique_lock<std::mutex> inglock(m_detecting_mutex);
            m_packet_queue.SetEOL(false);

            while (data_cnt < probesize)
            {
                if (m_packet_queue.Size() < BURST_HEADER_SIZE)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }

                sz = m_packet_queue.Pop(reinterpret_cast<uint8_t* >(buf),
                                        BURST_HEADER_SIZE);

                uint16_t* p = reinterpret_cast<uint16_t* >(buf);

                // If nulls, ignore them.
                if (p[0] == 0 && p[1] == 0)
                    null_cnt += sz;
                else
                {
                    data_cnt += sz;

                    if (p[0] == SYNCWORD1 && p[1] == SYNCWORD2)
                    {
                        if (m_verbose > 0)
                            cerr << "Found SPDIF header @ "
                                 << null_cnt + data_cnt << " bytes"
                                 << " (nulls: " << null_cnt
                                 << " data: " << data_cnt << ")\n";
                        break;
                    }
                }

                if (data_cnt == 0)
                {
                    if (null_cnt > max_nulls)
                        break;
                    if (null_cnt % nulls_msg == 0)
                        cerr << "Still waiting for audio ("
                             << null_cnt << " null bytes).\n";
                }
            }
        }

        if (data_cnt >= probesize)
        {
            if (m_verbose > 0)
            {
                cerr << "Did not find SPDIF header in first "
                     << null_cnt + data_cnt << " bytes"
                     << " (nulls: " << null_cnt
                     << " data: " << data_cnt << ")\n";
            }
            return false;
        }

        if (null_cnt > max_nulls)
        {
            if (m_verbose > 0)
            {
                cerr << "WARNING: no audio data detected.\n";
            }
            return false;
        }

        m_packet_queue.Seek(-BURST_HEADER_SIZE, SEEK_CUR);
        open_spdif_context();

        if ((ret = av_probe_input_buffer(m_spdif_avio_context,
                                         &fmt, "", nullptr, 0,
                                         5 * m_audio_block_size)) != 0)
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
                 << "' '" << fmt->long_name
                ; //                 << "' codec: " << fmt->raw_codec_id << endl;
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

    if (idx == 10)
        return false;


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


    m_bitstream = true;
    return true;
}

void OutputTS::detect_audio(void)
{
    if (m_verbose > 2)
    {
        cerr << "Detecting audio\n";
    }

    m_bitstream = open_spdif();

    if (m_bitstream)
    {
        m_audio_codec_name = m_spdif_codec->name;
        m_channel_layout = AV_CHANNEL_LAYOUT_5POINT1;
        if (m_verbose > 0)
        {
            cerr << "--> Bitstream: (" << m_audio_codec_name << ")\n";
        }
    }
    else
    {
        m_audio_codec_name = "eac3";
        m_channel_layout = AV_CHANNEL_LAYOUT_STEREO;
        if (m_verbose > 0)
        {
            cerr << "--> PCM: (" << m_audio_codec_name << ")\n";
        }
    }

    const std::unique_lock<std::mutex> lock(m_detect_mutex);
    m_audio_detect = false;
    m_audio_detected.notify_all();
}

bool OutputTS::AudioReady(void)
{
    std::unique_lock<std::mutex> lock(m_detect_mutex);
    if (m_no_audio)
        return true;

    if (m_audio_detect)
        m_audio_detected.wait_for(lock, std::chrono::milliseconds(100));

    if (!m_audio_detect && m_audio_detect_thread.joinable())
        m_audio_detect_thread.join();

    return (!m_audio_detect && m_audio_channels >= 0);
}

AVFrame* OutputTS::get_pcm_audio_frame(OutputStream* ost)
{
    AVFrame* frame = ost->tmp_frame;

    int j, i;
    uint8_t* q = (uint8_t*)frame->data[0];
    size_t bytes;
    size_t got;

    bytes = ost->enc->ch_layout.nb_channels * frame->nb_samples * 2;
    if (m_packet_queue.Size() < bytes)
        return nullptr;
    if ((got = m_packet_queue.Pop(q, bytes)) == 0)
        return nullptr;

    frame->pts = m_packet_queue.TimeStamp();

    ost->frame->pts = av_rescale_q(frame->pts, m_input_time_base,
                                   ost->enc->time_base);

    ost->t     += (ost->tincr * frame->nb_samples);
    ost->tincr += (ost->tincr2 * frame->nb_samples);

    ost->next_timestamp = frame->pts + frame->nb_samples;

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
#if 0
    dst_nb_samples = av_rescale_rnd(swr_get_delay(ost->swr_ctx,
                                                  enc_ctx->sample_rate)
                                    + frame->nb_samples,
                                    enc_ctx->sample_rate,
                                    enc_ctx->sample_rate,
                                    AV_ROUND_UP);
#else
    dst_nb_samples = av_rescale(swr_get_delay(ost->swr_ctx,
                                              enc_ctx->sample_rate)
                                + frame->nb_samples,
                                enc_ctx->sample_rate,
                                enc_ctx->sample_rate);
#endif
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
#if 1
    frame->pts = av_rescale_q(m_packet_queue.TimeStamp(),
                              m_input_time_base,
                              enc_ctx->time_base);
#else
    frame->pts = av_rescale_q_rnd(m_packet_queue.TimeStamp(),
                                  m_input_time_base,
                                  enc_ctx->time_base,
                    static_cast<AVRounding>(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
#endif

    ost->samples_count += dst_nb_samples;

    return write_frame(oc, enc_ctx, frame, ost);
}

bool OutputTS::write_bitstream_frame(AVFormatContext* oc, OutputStream* ost)
{
    if (m_packet_queue.Size() < m_audio_block_size * 12)
        return false;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt)
    {
        if (m_verbose > 0)
        {
            cerr << "Could not allocate pkt for spdif input.\n";
        }
        return false;
    }

    int ret =  av_read_frame(m_spdif_format_context, pkt);
    if (0 > ret)
    {
        if (m_verbose > 0)
        {
            cerr << "Failed to read spdif frame: "
                 << AVerr2str(ret) << endl;
        }
        return false;
    }

    pkt->pts = av_rescale_q(m_packet_queue.TimeStamp(),
                            m_input_time_base,
                            ost->st->time_base);
    ost->next_timestamp = m_packet_queue.TimeStamp() +
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
    std::unique_lock<std::mutex> lock(m_detecting_mutex);

    if (m_bitstream)
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

    av_opt_set(ctx->priv_data, "preset", "p7", 0);
    av_opt_set(ctx->priv_data, "tune", "hq", 0);
    av_opt_set(ctx->priv_data, "rc", "constqp", 0);

    av_opt_set_int(ctx->priv_data, "cq", 16, 0);
    if (m_look_ahead >= 0)
        av_opt_set_int(ctx->priv_data, "rc-lookahead", m_look_ahead, 0);
    av_opt_set_int(ctx->priv_data, "b", 0, 0);
    av_opt_set_int(ctx->priv_data, "minrate", 4000000, 0);
    av_opt_set_int(ctx->priv_data, "maxrate", 25000000, 0);
    av_opt_set_int(ctx->priv_data, "bufsize", 400000000, 0);
    av_opt_set_int(ctx->priv_data, "surfaces", 50, 0);

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
    int ret;
    AVCodecContext* ctx = ost->enc;
    AVDictionary* opt = nullptr;
    AVBufferRef* hw_frames_ref;
    AVHWFramesContext* frames_ctx = nullptr;

    av_dict_copy(&opt, opt_arg, 0);

    av_opt_set_int(ctx->priv_data, "preset", 3, 0);
    av_opt_set(ctx->priv_data, "scenario", "livestreaming", 0);
//    av_opt_set_int(ctx->priv_data, "scenario", 7, 0);
    av_opt_set_int(ctx->priv_data, "extbrc", 1, 0);
    av_opt_set_int(ctx->priv_data, "look_ahead_depth", 30, 0);
//    av_opt_set_int(ctx->priv_data, "bf", 0, 0);
    ctx->global_quality = 20;

    vector<std::string> drivers{ "iHD", "i965" };
    vector<std::string>::iterator Idriver;
    for (Idriver = drivers.begin(); Idriver != drivers.end(); ++Idriver)
    {
        string envstr = "LIBVA_DRIVER_NAME=" + *Idriver;
        char* env = envstr.data();
        putenv(env);

        if ((ret = av_hwdevice_ctx_create(&ost->hw_device_ctx,
                                          AV_HWDEVICE_TYPE_QSV,
                                          m_device.c_str(), opt, 0)) < 0)
            cerr << "Failed to open QSV driver '" << *Idriver << "'\n";
        else
            break;
    }
    if (Idriver == drivers.end())
    {
        cerr << "Failed to create a QSV device. Error code: "
             << AVerr2str(ret) << endl;
        m_error = true;
        return false;
    }

    if (m_verbose > 0)
        cerr << "Using QSV driver '" << *Idriver << "'\n";

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
                         uint8_t* data,
                         uint32_t imageSize,
                         int64_t timestamp)
{
    AVCodecContext* ctx = ost->enc;

    int64_t pts = av_rescale_q(timestamp, m_input_time_base,
                               ctx->time_base);
    if (ost->next_pts > 0)
    {
        while (ost->next_pts < pts)
        {
            ost->frame->pts = ost->next_pts++;
            write_frame(oc, ost->enc, ost->frame, ost);
        }
        ost->frame->pts = ost->next_pts;
    }
    else
        ost->frame->pts = pts;

    // YUV 4:2:0
    size_t size = ctx->width * ctx->height;
    memcpy(ost->frame->data[0], data, size);
    memcpy(ost->frame->data[1],
           data + size, size / 4);
    memcpy(ost->frame->data[2], data + size * 5 / 4, size  / 4);

    ost->next_timestamp = timestamp + 1;
    ost->next_pts = ost->frame->pts + 1;

    return write_frame(oc, ost->enc, ost->frame, ost);
}

bool OutputTS::qsv_vaapi_encode(AVFormatContext* oc,
                                OutputStream* ost,
                                uint8_t* data,
                                uint32_t imageSize,
                                int64_t timestamp)
{
    AVCodecContext* enc_ctx = ost->enc;
    static AVFrame* hw_frame = nullptr;
    int    ret;

    /*
     * We may need to repeat the previous frame to keep everything in sync
     */
    int64_t pts;
    if (hw_frame != nullptr)
    {
        pts = av_rescale_q(timestamp, m_input_time_base,
                           enc_ctx->time_base);
        while (ost->next_pts < pts)
        {
            hw_frame->pts = ost->next_pts;
            ++ost->next_pts;
            write_frame(oc, enc_ctx, hw_frame, ost);
        }
        av_frame_free(&hw_frame);
    }

    size_t size = enc_ctx->width * enc_ctx->height;
    memcpy(ost->frame->data[0], data, size);
    memcpy(ost->frame->data[1], data + size, size / 2);

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

    if (ost->next_pts == 0)
        hw_frame->pts = av_rescale_q(timestamp, m_input_time_base,
                                     enc_ctx->time_base);
    else
        hw_frame->pts = ost->next_pts;

    ost->next_timestamp = timestamp + 1;
    ost->next_pts = hw_frame->pts + 1;

    return write_frame(oc, enc_ctx, hw_frame, ost);
}

/*
 * encode one video frame and send it to the muxer
 * return 1 when encoding is finished, 0 otherwise
 */
bool OutputTS::write_video_frame(AVFormatContext* oc, OutputStream* ost,
                                uint8_t* pImage, uint32_t imageSize,
                                int64_t timestamp)
{
    if (m_encoderType == EncoderType::NV)
        return nv_encode(oc, ost, pImage, imageSize, timestamp);
    else if (m_encoderType == EncoderType::QSV)
        return qsv_vaapi_encode(oc, ost, pImage, imageSize, timestamp);
    else if (m_encoderType == EncoderType::VAAPI)
        return qsv_vaapi_encode(oc, ost, pImage, imageSize, timestamp);

    return false;
}

bool OutputTS::Write(uint8_t* pImage, uint32_t imageSize,
                     int64_t timestamp)
{
    if (!m_no_audio)
    {
        while (m_video_stream.next_pts > m_audio_stream.next_pts)
            if (!write_audio_frame(m_output_format_context, &m_audio_stream))
                break;
    }

    write_video_frame(m_output_format_context,
                      &m_video_stream, pImage, imageSize,
                      timestamp);

    return false;
}
