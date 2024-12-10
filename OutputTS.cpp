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

#include <csignal>
#include <unistd.h>
#include <iostream>
#include <iomanip>
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

static std::string AVerr2str(int code)
{
    char astr[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_make_error_string(astr, AV_ERROR_MAX_STRING_SIZE, code);
    return string(astr);
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

OutputTS::OutputTS(int verbose_level, const string & video_codec_name,
                   const string & preset, int quality, int look_ahead,
                   bool no_audio, const string & device,
                   MagCallback image_buffer_avail)
    : m_audioIO(verbose_level)
    , m_verbose(verbose_level)
    , m_no_audio(no_audio)
    , m_video_codec_name(video_codec_name)
    , m_device("/dev/dri/" + device)
    , m_preset(preset)
    , m_quality(quality)
    , m_look_ahead(look_ahead)
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
        cerr << "ERROR: Codec '" << m_video_codec_name << "' not supported.\n";
        Shutdown();
    }

    m_audio_ready = m_no_audio;
    m_image_ready_thread = std::thread(&OutputTS::Write, this);
}

void OutputTS::Shutdown(void)
{
    m_audioIO.Shutdown();
    m_running.store(false);
}

/* Add an output stream. */
bool OutputTS::add_stream(OutputStream* ost, AVFormatContext* oc,
                          const AVCodec* *codec)
{
    AVChannelLayout channel_layout = m_audioIO.ChannelLayout();
    AVCodecContext* codec_context;
    int idx;

    ost->tmp_pkt = av_packet_alloc();
    if (!ost->tmp_pkt)
    {
        cerr << "ERROR: Could not allocate AVPacket\n";
        return false;
    }

    ost->st = avformat_new_stream(oc, NULL);
    if (!ost->st)
    {
        cerr << "ERROR: Could not allocate stream\n";
        return false;
    }
    ost->st->id = oc->nb_streams-1;
    codec_context = avcodec_alloc_context3(*codec);
    if (!codec_context)
    {
        cerr << "ERROR: Could not alloc an encoding context\n";
        return false;
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
                  if ((*codec)->supported_samplerates[idx] == m_audioIO.SampleRate())
                  {
                      ost->enc->sample_rate = m_audioIO.SampleRate();
                      break;
                  }
              }
          }
          else
              ost->enc->sample_rate = 48000;

          av_channel_layout_copy(&ost->enc->ch_layout, &channel_layout);
          ost->st->time_base = (AVRational){ 1, ost->enc->sample_rate };

          if (ost->enc->codec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
          {
              ost->enc->thread_type = FF_THREAD_SLICE;
              if (m_verbose > 1)
                  cerr << " Audio = THREAD SLICE\n";
          }
          else if (ost->enc->codec->capabilities & AV_CODEC_CAP_FRAME_THREADS)
          {
              ost->enc->thread_type = FF_THREAD_FRAME;
              if (m_verbose > 1)
                  cerr << " Audio = THREAD FRAME\n";
          }

          if (m_verbose > 1)
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

          /*
            For av1_qsv, pix_fmt options are:
                AV_PIX_FMT_NV12, AV_PIX_FMT_P010, AV_PIX_FMT_QSV
          */

          if (m_encoderType == EncoderType::QSV)
              ost->enc->pix_fmt = AV_PIX_FMT_QSV;
          else if (m_encoderType == EncoderType::VAAPI)
              ost->enc->pix_fmt = AV_PIX_FMT_VAAPI;
          else
              ost->enc->pix_fmt = AV_PIX_FMT_YUV420P;

          if (ost->enc->codec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
          {
              ost->enc->thread_type = FF_THREAD_SLICE;
              if (m_verbose > 1)
                  cerr << " Video = THREAD SLICE\n";
          }
          else if (ost->enc->codec->capabilities & AV_CODEC_CAP_FRAME_THREADS)
          {
              ost->enc->thread_type = FF_THREAD_FRAME;
              if (m_verbose > 1)
                  cerr << " Video = THREAD FRAME\n";
          }

          if (m_verbose > 1)
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

    return true;
}

AVFrame* OutputTS::alloc_audio_frame(enum AVSampleFormat sample_fmt,
                                     const AVChannelLayout* channel_layout,
                                     int sample_rate, int nb_samples)
{
    AVFrame* frame = av_frame_alloc();
    int ret;

    if (!frame)
    {
        cerr << "ERROR: Failed to allocate an audio frame\n";
        return nullptr;
    }

    frame->format = sample_fmt;
    av_channel_layout_copy(&frame->ch_layout, channel_layout);
    frame->sample_rate = sample_rate;
    /* Frame size passed from magewell includes all channels */
    frame->nb_samples = nb_samples;

    if (nb_samples)
    {
        ret = av_frame_get_buffer(frame, 0);
        if (ret < 0)
        {
            cerr << "ERROR: failed to allocate an audio buffer\n";
            return nullptr;
        }
    }

    return frame;
}

bool OutputTS::open_audio(void)
{
    if (m_no_audio)
        return true;

    close_stream(&m_audio_stream);

    const AVCodec* audio_codec = nullptr;

    audio_codec = avcodec_find_encoder_by_name(m_audioIO.CodecName().c_str());

    if (!audio_codec)
    {
        cerr << "ERROR: Could not find audio encoder for '"
             << m_audioIO.CodecName() << "'\n";
        Shutdown();
        return false;
    }

    if (!add_stream(&m_audio_stream, m_output_format_context, &audio_codec))
        return false;

    OutputStream* ost = &m_audio_stream;
//    AVFormatContext* oc = m_output_format_context;
    const AVCodec* codec = audio_codec;
    AVCodecContext* enc_ctx = m_audio_stream.enc;
    AVDictionary* opt = NULL;
    int nb_samples;
    int ret;

    if ((ret = avcodec_open2(enc_ctx, codec, &opt)) < 0)
    {
        cerr << "ERROR: Could not open audio codec: " << AVerr2str(ret) << endl;
        return false;
    }

    if (enc_ctx->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
        nb_samples = 10000;
    else
    {
        nb_samples = enc_ctx->frame_size;
    }

    ost->frame = alloc_audio_frame(enc_ctx->sample_fmt, &enc_ctx->ch_layout,
                                   enc_ctx->sample_rate, nb_samples);
    if (ost->frame == nullptr)
    {
        Shutdown();
        return false;
    }

    if (m_audioIO.BytesPerSample() == 4)
        ost->tmp_frame = alloc_audio_frame(AV_SAMPLE_FMT_S32,
                                           &enc_ctx->ch_layout,
                                           enc_ctx->sample_rate, nb_samples);
    else
        ost->tmp_frame = alloc_audio_frame(AV_SAMPLE_FMT_S16,
                                           &enc_ctx->ch_layout,
                                           enc_ctx->sample_rate, nb_samples);

    if (ost->tmp_frame == nullptr)
    {
        cerr << "ERROR: Unable to allocate a temporary audio frame.\n";
        Shutdown();
        return false;
    }

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, enc_ctx);
    if (ret < 0)
    {
        cerr << "ERROR: Could not copy the stream parameters\n";
        return false;
    }

    /* create resampler context */
    ost->swr_ctx = swr_alloc();
    if (!ost->swr_ctx)
    {
        cerr << "ERROR: Could not allocate resampler context\n";
        return false;
    }

    /* set options */
    av_opt_set_chlayout  (ost->swr_ctx, "in_chlayout",
                          &enc_ctx->ch_layout,     0);
    av_opt_set_int       (ost->swr_ctx, "in_sample_rate",
                          enc_ctx->sample_rate,    0);

    if (m_audioIO.BytesPerSample() == 4)
    {
        av_opt_set_sample_fmt(ost->swr_ctx, "in_sample_fmt",
                              AV_SAMPLE_FMT_S32, 0);
//        ctx->bits_per_raw_sample = 24;
    }
    else
        av_opt_set_sample_fmt(ost->swr_ctx, "in_sample_fmt",
                              AV_SAMPLE_FMT_S16, 0);

    av_opt_set_chlayout  (ost->swr_ctx, "out_chlayout",
                          &enc_ctx->ch_layout,     0);
    av_opt_set_int       (ost->swr_ctx, "out_sample_rate",
                          enc_ctx->sample_rate,    0);
    av_opt_set_sample_fmt(ost->swr_ctx, "out_sample_fmt",
                          enc_ctx->sample_fmt,     0);

    /* initialize the resampling context */
    if ((ret = swr_init(ost->swr_ctx)) < 0)
    {
        cerr << "ERROR: Failed to initialize the resampling context\n";
        return false;
    }

    return true;
}

bool OutputTS::open_video(void)
{
    close_stream(&m_video_stream);

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
        cerr << "ERROR: Could not find video encoder for '"
             << m_video_codec_name << "'\n";
        return false;
    }

    /* Add the audio and video streams using the default format codecs
     * and initialize the codecs. */
    if (!add_stream(&m_video_stream, m_output_format_context,
                    &video_codec))
        return false;

    /* Now that all the parameters are set, we can open the audio and
     * video codecs and allocate the necessary encode buffers. */
    switch (m_encoderType)
    {
        case EncoderType::QSV:
          if (!open_qsv(video_codec, &m_video_stream, opt))
              return false;
          break;
        case EncoderType::VAAPI:
          if (!open_vaapi(video_codec, &m_video_stream, opt))
              return false;
          break;
        case EncoderType::NV:
          if (!open_nvidia(video_codec, &m_video_stream, opt))
              return false;
          break;
        default:
          cerr << "ERROR: Could not determine video encoder type.\n";
          return false;
    }

    return true;
}

bool OutputTS::open_container(void)
{
    int ret;
    AVDictionary* opt = NULL;

    close_container();
    if (m_running.load() == false)
        return false;

#if 1
    if (m_verbose > 1)
        cerr << "\n================== open_container begin ==================\n";
#endif

    /* allocate the output media context */
    avformat_alloc_output_context2(&m_output_format_context,
                                   NULL, "mpegts", m_filename.c_str());
    if (!m_output_format_context)
    {
        cerr << "ERROR: Could not create output format context.\n";
        Shutdown();
        return false;
    }

    m_fmt = m_output_format_context->oformat;

    if (!open_audio())
        return false;
    if (!open_video())
        return false;

    if (m_verbose > 0)
        av_dump_format(m_output_format_context, 0, m_filename.c_str(), 1);

    /* open the output file, if needed */
    if (!(m_fmt->flags & AVFMT_NOFILE)) {
        ret = avio_open(&m_output_format_context->pb,
                        m_filename.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            cerr << "ERROR: Could not open '" << m_filename << "': "
                 << AVerr2str(ret) << endl;
            Shutdown();
            return false;
        }
    }

    /* Write the stream header, if any. */
    ret = avformat_write_header(m_output_format_context, &opt);
    if (ret < 0)
    {
        cerr << "ERROR: Could not open output file: %s\n"
             << AVerr2str(ret) << endl;
        Shutdown();
        return false;
    }

#if 1
    if (m_verbose > 1)
        cerr << "\n================== open_container end ==================\n";
#endif
    return true;
}

bool OutputTS::setAudioParams(uint8_t* capture_buf, size_t capture_buf_size,
                              int num_channels, bool is_lpcm,
                              int bytes_per_sample, int sample_rate,
                              int samples_per_frame, int frame_size,
                              int64_t* timestamps)
{
    if (!m_audioIO.AddBuffer(capture_buf, capture_buf + capture_buf_size,
                             num_channels, is_lpcm,
                             bytes_per_sample, sample_rate,
                             samples_per_frame, frame_size,
                             timestamps))
        return false;

    if (m_verbose > 0)
        cerr << "setAudioParams " << (is_lpcm ? "LPCM" : "Bitstream") << endl;

    return true;
}

bool OutputTS::setVideoParams(int width, int height, bool interlaced,
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

    if (m_verbose > 0)
    {
        cerr << "Video: " << width << "x" << height
             << " fps: " << fps
             << (m_interlaced ? 'i' : 'p')
             << "\n";

        if (m_verbose > 2)
            cerr << "Video Params set\n";
    }

    m_init_needed = true;

    return true;
}

OutputTS::~OutputTS(void)
{
    Shutdown();

    if (m_image_ready_thread.joinable())
        m_image_ready_thread.join();

    close_stream(&m_video_stream);
    if (m_video_stream.hw_device_ctx != nullptr)
        av_buffer_unref(&m_video_stream.hw_device_ctx);

    close_stream(&m_audio_stream);
    close_container();
}

bool OutputTS::addAudio(uint8_t* buf, size_t len, int64_t timestamp)
{
    m_audioIO.Add(buf, len, timestamp);
    return true;
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
            cerr << "WARNING: Failed sending a frame to the encoder: "
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
                cerr << "WARNING: Failed encoding a frame: AVerr2str(ret)\n";
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
                cerr << "WARNING: Failed to write packet: " << AVerr2str(ret)
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


void OutputTS::close_container(void)
{

#if 0
    if (m_output_format_context)
        av_write_trailer(m_output_format_context);
#endif

    if (m_fmt && !(m_fmt->flags & AVFMT_NOFILE))
        /* Close the output file. */
        avio_closep(&m_output_format_context->pb);

    /* free the output stream */
    avformat_free_context(m_output_format_context);
}

void OutputTS::close_stream(OutputStream* ost)
{
    if (ost == nullptr)
        return;

    if (ost->hw_device)
    {
        av_buffer_unref(&ost->enc->hw_frames_ctx);
        ost->hw_device = false;
    }

#if 1
    if (ost->tmp_frame /*  && ost->tmp_frame->data[0] */)
    {
#if 0
        av_frame_free(&ost->tmp_frame);
#else
        av_free(&ost->frame->data[0]);
#endif
        ost->tmp_frame = nullptr;
    }
#endif

    if (ost->swr_ctx)
    {
        swr_free(&ost->swr_ctx);
        ost->swr_ctx = nullptr;
    }

    if (ost->enc)
    {
        /*
          FFmpeg docs now say:
          Opening and closing a codec context multiple times is not
          supported anymore – use multiple codec contexts instead.

          So, does this mean to never free a codect context?
         */
#if 0
        avcodec_free_context(&ost->enc);
        ost->enc = nullptr;
#endif
    }

    /* Documentation says to call avformat_free_context, but
       that does not compile. The example code does not free
       the stream either, so... */
#if 0
    avformat_free_context(&ost->st);
    ost->st = nullptr;
#endif
}

/**************************************************************/
/* audio output */

AVFrame* OutputTS::get_pcm_audio_frame(OutputStream* ost)
{
    AVFrame* frame = ost->tmp_frame;

    uint8_t* q = (uint8_t*)frame->data[0];

    int bytes = ost->enc->ch_layout.nb_channels *
                frame->nb_samples * m_audioIO.BytesPerSample();
    if (m_audioIO.Size() < bytes)
    {
        if (m_verbose > 4)
            cerr << "Not enough audio data.\n";
        return nullptr;
    }
    if (m_audioIO.Read(q, bytes) == 0)
        return nullptr;

    frame->pts = m_audioIO.TimeStamp();
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
        cerr << "WARNING: write_pcm_frame: Failed to make frame writable\n";
        return false;
    }

    /* convert to destination format */
    ret = swr_convert(ost->swr_ctx,
                      ost->frame->data, dst_nb_samples,
                      const_cast<const uint8_t** >(frame->data),
                      frame->nb_samples);
    if (ret < 0)
    {
        cerr << "WARNING: write_pcm_frame: Error while converting\n";
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
    AVPacket* pkt = m_audioIO.ReadSPDIF();

    if (pkt == nullptr)
        return false;

    pkt->pts = av_rescale_q(m_audioIO.TimeStamp(),
                            m_input_time_base,
                            ost->st->time_base);

    if (pkt->pts - ost->prev_audio_pts > ost->frame->nb_samples * 3)
    {
        /* This a bit of hack, but seems to solve the problem.
           Jumping around in some applications can cause the S/PDIF to
           get out of sync. For example, This can happen if the
           audio_capture::audio_buf_sz is too small.
        */
        if (++m_slow_audio_cnt > 10)
        {
            if (m_verbose > 0)
                cerr << "WARNING: S/PDIF audio out of sync, resetting." << endl;
            m_slow_audio_cnt = 0;

            if (!m_audioIO.RescanSPDIF())
                Shutdown();

            return false;
        }
    }
    else
        m_slow_audio_cnt = 0;

    ost->prev_audio_pts = pkt->pts;

    /* Frame size passed from magewell includes all channels */
    ost->next_pts = m_audioIO.TimeStamp() + ost->frame->nb_samples;

    pkt->duration = pkt->pts;
    pkt->dts = pkt->pts;

    pkt->stream_index = ost->st->index;

    /* Write the frame to the media file. */
#if 0
    log_packet("write_bitstream_frame", oc, pkt);
#endif
    int ret = av_interleaved_write_frame(oc, pkt);
    /* pkt is now blank (av_interleaved_write_frame() takes ownership of
     * its contents and resets pkt), so that no unreferencing is necessary.
     * This would be different if one used av_write_frame(). */
    if (ret < 0)
    {
        cerr << "WARNING: Failed to write audio packet: " << AVerr2str(ret)
             << "\n";
        return false;
    }

    return true;
}

/*
 * encode one audio frame and send it to the muxer
 */
bool OutputTS::write_audio_frame(AVFormatContext* oc, OutputStream* ost)
{
    if (m_audioIO.CodecChanged())
    {
        m_init_needed = true;
        return false;
    }

    if (!m_audioIO.BlockReady())
    {
        return false;
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
        cerr << "ERROR: Could not allocate " << desc->name
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
        cerr << "ERROR: Could not open video codec: "
             << AVerr2str(ret) << "\n";
        Shutdown();
        return false;
    }

    /* allocate and init a re-usable frame */
    ost->frame = alloc_picture(ctx->pix_fmt, ctx->width, ctx->height);
    if (!ost->frame)
    {
        cerr << "ERROR: Could not allocate NVIDIA video frame\n";
        Shutdown();
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
            cerr << "ERROR: Could not allocate temporary picture." << endl;
            Shutdown();
            return false;
        }
    }

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, ctx);
    if (ret < 0)
    {
        cerr << "ERROR: Could not copy the stream parameters." << endl;
        Shutdown();
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

    if (ost->hw_device_ctx == nullptr)
    {
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
                cerr << "ERROR: Failed to open VAPPI driver '" << *Idriver << "'\n";
            else
                break;
        }
        if (Idriver == drivers.end())
        {
            cerr << "ERROR: Failed to create a VAAPI device. Error code: "
                 << AVerr2str(ret) << endl;
            Shutdown();
            return false;
        }

        if (m_verbose > 0)
            cerr << "Using VAAPI driver '" << *Idriver << "'\n";
    }

    /* set hw_frames_ctx for encoder's AVCodecContext */
    if (!(hw_frames_ref = av_hwframe_ctx_alloc(ost->hw_device_ctx)))
    {
        cerr << "ERROR: Failed to create VAAPI frame context.\n";
        Shutdown();
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
        cerr << "ERROR: Failed to initialize VAAPI frame context."
             << "Error code: " << AVerr2str(ret) << endl;
        av_buffer_unref(&hw_frames_ref);
        Shutdown();
        return false;
    }
    ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    if (!ctx->hw_frames_ctx)
    {
        ret = AVERROR(ENOMEM);
        cerr << "ERROR: Failed to allocate hw frame buffer. "
             << "Error code: " << AVerr2str(ret) << endl;
        av_buffer_unref(&hw_frames_ref);
        Shutdown();
        return false;
    }
    av_buffer_unref(&hw_frames_ref);
    ost->hw_device = true;

    if ((ret = avcodec_open2(ctx, codec, &opt)) < 0)
    {
        cerr << "ERROR: Cannot open VAAPI video encoder codec. Error code: "
             << AVerr2str(ret) << endl;
        Shutdown();
        return false;
    }

    /* allocate and init a re-usable frame */
    ost->frame = alloc_picture(frames_ctx->sw_format,
                               frames_ctx->width, frames_ctx->height);
    if (!ost->frame)
    {
        cerr << "ERROR: Could not allocate VAAPI video frame\n";
        Shutdown();
        return false;
    }

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, ctx);
    if (ret < 0)
    {
        cerr << "ERROR: Could not copy the VAAPI stream parameters." << endl;
        Shutdown();
        return false;
    }

    return true;
}

bool OutputTS::open_qsv(const AVCodec* codec,
                        OutputStream* ost, AVDictionary* opt_arg)
{
    int    ret;

    AVDictionary* opt = nullptr;
    AVBufferRef* hw_frames_ref;
    AVHWFramesContext* frames_ctx = nullptr;

    av_dict_copy(&opt, opt_arg, 0);

    ost->enc->global_quality = m_quality;

    if (m_video_codec_name != "av1_qsv")
    {
        if (!m_preset.empty())
        {
            av_opt_set(ost->enc->priv_data, "preset", m_preset.c_str(), 0);
            if (m_verbose > 0)
                cerr << "Using preset " << m_preset << " for "
                     << m_video_codec_name << endl;
        }

        av_opt_set(ost->enc->priv_data, "scenario", "livestreaming", 0);
#if 0
        av_opt_set_int(ost->enc->priv_data, "extbrc", 1, 0);
        av_opt_set_int(ost->enc->priv_data, "adaptive_i", 1, 0);

//    av_opt_set_int(ost->enc->priv_data, "bf", 0, 0);
#endif

        if (m_look_ahead >= 0)
        {
            if (m_video_codec_name == "hevc_qsv")
                av_opt_set_int(ost->enc->priv_data, "look_ahead", 1, 0);
            av_opt_set_int(ost->enc->priv_data, "look_ahead_depth", m_look_ahead, 0);
        }
        av_opt_set_int(ost->enc->priv_data, "extra_hw_frames", m_look_ahead, 0);

        av_opt_set(ost->enc->priv_data, "skip_frame", "insert_dummy", 0);
    }
    if (ost->hw_device_ctx == nullptr)
    {
        // Make sure env doesn't prevent QSV init.
        static string envstr = "LIBVA_DRIVER_NAME";
        char* env = envstr.data();
        unsetenv(env);

        av_dict_set(&opt, "child_device", m_device.c_str(), 0);
        if ((ret = av_hwdevice_ctx_create(&ost->hw_device_ctx,
                                          AV_HWDEVICE_TYPE_QSV,
                                          m_device.c_str(), opt, 0)) != 0)
        {
            cerr << "ERROR: Failed to open QSV on " << m_device << "\n";
            return false;
        }

        if (m_verbose > 0)
            cerr << "Using QSV\n";
    }

    /* set hw_frames_ctx for encoder's AVCodecContext */
    if (!(hw_frames_ref = av_hwframe_ctx_alloc(ost->hw_device_ctx)))
    {
        cerr << "ERROR: Failed to create QSV frame context.\n";
        Shutdown();
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
        cerr << "ERROR: Failed to initialize QSV frame context."
             << "Error code: " << AVerr2str(ret) << endl;
        av_buffer_unref(&hw_frames_ref);
        Shutdown();
        return false;
    }
    ost->enc->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    if (!ost->enc->hw_frames_ctx)
    {
        ret = AVERROR(ENOMEM);
        cerr << "ERROR: Failed to allocate hw frame buffer. "
             << "Error code: " << AVerr2str(ret) << endl;
        av_buffer_unref(&hw_frames_ref);
        Shutdown();
        return false;
    }
    av_buffer_unref(&hw_frames_ref);
    ost->hw_device = true;

    if ((ret = avcodec_open2(ost->enc, codec, &opt)) < 0)
    {
        cerr << "ERROR: Cannot open QSV video encoder codec. Error code: "
             << AVerr2str(ret) << endl;
        Shutdown();
        return false;
    }

    /* allocate and init a re-usable frame */
    ost->frame = alloc_picture(frames_ctx->sw_format,
                               frames_ctx->width, frames_ctx->height);
    if (!ost->frame)
    {
        cerr << "ERROR: Could not allocate QSV video frame\n";
        Shutdown();
        return false;
    }

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, ost->enc);
    if (ret < 0)
    {
        cerr << "ERROR: Could not copy the stream parameters." << endl;
        Shutdown();
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
        cerr << "ERROR: get_video_frame: Make frame writable failed.\n";
        return false;
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
        cerr << "ERROR: Failed to allocate hw frame.";
        Shutdown();
        return false;
    }

    if ((ret = av_hwframe_get_buffer(enc_ctx->hw_frames_ctx,
                                     hw_frame, 0)) < 0)
    {
        cerr << "ERROR: Failed to get hw buffer: "
             << AV_ts2str(ret) << endl;
        Shutdown();
        return false;
    }

    if (!hw_frame->hw_frames_ctx)
    {
        cerr << "ERROR: Failed to allocate hw frame CTX.\n";
        Shutdown();
        return false;
    }

    if ((ret = av_hwframe_transfer_data(hw_frame,
                                        ost->frame, 0)) < 0)
    {
        cerr << "ERROR: failed transferring frame data to surface: "
             << AV_ts2str(ret) << endl;
        Shutdown();
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

    while (m_running.load() == true)
    {
        m_image_ready.wait_for(lock,
                               std::chrono::milliseconds(m_input_frame_wait_ms));

        if (!m_audio_ready)
        {
            if (m_audioIO.CodecChanged())
                continue;
            m_audio_ready = true;
        }

        if (m_init_needed)
        {
            if (!open_container())
            {
                Shutdown();
                break;
            }
            m_init_needed = false;
        }

        while (m_running.load() == true)
        {
            if (!m_no_audio)
            {
#if 0
                cerr << "Write: video " << m_video_stream.next_pts << "\n"
                     << "  audio [" << setw(2) << m_audioIO.BufId()
                     << "] " << m_audio_stream.next_pts << endl;
#endif
                while (m_video_stream.next_pts > m_audio_stream.next_pts)
                    if (!write_audio_frame(m_output_format_context,
                                           &m_audio_stream))
                        break;
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
                cerr << "ERROR: Unknown encoderType.\n";
                Shutdown();
                return;
            }
        }
    }
}

void OutputTS::ClearImageQueue(void)
{
    const std::unique_lock<std::mutex> lock(m_imagequeue_mutex);
    m_imagequeue.clear();
}

bool OutputTS::AddVideoFrame(uint8_t* pImage, uint32_t imageSize,
                          int64_t timestamp)
{
    const std::unique_lock<std::mutex> lock(m_imagequeue_mutex);

    m_imagequeue.push_back(imagepkt_t{timestamp, pImage});
    m_image_ready.notify_one();

    return true;
}
