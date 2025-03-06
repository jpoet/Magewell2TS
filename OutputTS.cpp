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
#include "lock_ios.h"

using namespace std;
using namespace s6_lock_ios;

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

    cerr << lock_ios()
         << where << "[" << pkt->stream_index << "] pts: " << pkt->pts
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
                   StopCallback stop,
                   MagCallback image_buffer_avail)
    : m_audioIO(verbose_level)
    , m_verbose(verbose_level)
    , m_has_audio(!no_audio)
    , m_video_codec_name(video_codec_name)
    , m_device("/dev/dri/" + device)
    , m_preset(preset)
    , m_quality(quality)
    , m_look_ahead(look_ahead)
    , f_stop(stop)
    , f_image_buffer_available(image_buffer_avail)
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
        cerr << lock_ios()
             << "ERROR: Codec '" << m_video_codec_name << "' not supported.\n";
        Shutdown();
    }

#if 0
    m_audio_ready = false;
#endif

    m_image_thread = std::thread(&OutputTS::mux, this);
    m_frame_thread = std::thread(&OutputTS::encode_video, this);

    m_display_primaries  = av_mastering_display_metadata_alloc();
    m_content_light  = av_content_light_metadata_alloc(NULL);
}

void OutputTS::Shutdown(void)
{
    m_audioIO.Shutdown();
    m_running.store(false);
    f_stop();
}

void OutputTS::setLight(AVMasteringDisplayMetadata * display_meta,
                        AVContentLightMetadata * light_meta)
{
    if (display_meta && light_meta)
    {
        *m_display_primaries = *display_meta;
        *m_content_light = *light_meta;
    }
}

AVFrame* OutputTS::alloc_audio_frame(enum AVSampleFormat sample_fmt,
                                     const AVChannelLayout* channel_layout,
                                     int sample_rate, int nb_samples)
{
    AVFrame* frame = av_frame_alloc();
    int ret;

    if (!frame)
    {
        cerr << lock_ios()
             << "ERROR: Failed to allocate an audio frame\n";
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
            cerr << lock_ios()
                 << "ERROR: failed to allocate an audio buffer\n";
            return nullptr;
        }
    }

    return frame;
}

bool OutputTS::open_audio(void)
{
    close_encoder(&m_audio_stream);

    int idx;

    if (m_verbose > 1)
    {
        cerr << lock_ios()
             << "Adding audio stream." << endl;
    }

    const AVCodec* audio_codec = nullptr;

    audio_codec = avcodec_find_encoder_by_name(m_audioIO.CodecName().c_str());
    if (!audio_codec)
    {
        cerr << lock_ios()
             << "WARNING: Could not find audio encoder for '"
             << m_audioIO.CodecName() << "'\n";
        return true;
    }

    AVChannelLayout channel_layout = m_audioIO.ChannelLayout();

    m_audio_stream.tmp_pkt = av_packet_alloc();
    if (!m_audio_stream.tmp_pkt)
    {
        cerr << lock_ios()
             << "ERROR: Could not allocate AVPacket\n";
        return false;
    }

    m_audio_stream.enc = avcodec_alloc_context3(audio_codec);
    if (!m_audio_stream.enc)
    {
        cerr << lock_ios()
             << "ERROR: Could not alloc an encoding context\n";
        return false;
    }
    m_audio_stream.next_pts = 0;

    m_audio_stream.enc->sample_fmt  = audio_codec->sample_fmts ?
                            audio_codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
    m_audio_stream.enc->bit_rate    = 192000;
    if (audio_codec->supported_samplerates)
    {
        m_audio_stream.enc->sample_rate = audio_codec->supported_samplerates[0];
        for (idx = 0; audio_codec->supported_samplerates[idx]; ++idx)
        {
            if (audio_codec->supported_samplerates[idx] == m_audioIO.SampleRate())
            {
                m_audio_stream.enc->sample_rate = m_audioIO.SampleRate();
                break;
            }
        }
    }
    else
        m_audio_stream.enc->sample_rate = 48000;

    av_channel_layout_copy(&m_audio_stream.enc->ch_layout, &channel_layout);

    if (m_audio_stream.enc->codec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
    {
        m_audio_stream.enc->thread_type = FF_THREAD_SLICE;
        if (m_verbose > 1)
            cerr << lock_ios()
                 << " Audio = THREAD SLICE\n";
    }
    else if (m_audio_stream.enc->codec->capabilities &
             AV_CODEC_CAP_FRAME_THREADS)
    {
        m_audio_stream.enc->thread_type = FF_THREAD_FRAME;
        if (m_verbose > 1)
            cerr << lock_ios()
                 << " Audio = THREAD FRAME\n";
    }

//    AVFormatContext* oc = m_output_format_context;
    const AVCodec* codec = audio_codec;
//    AVCodecContext* enc_ctx = m_audio_stream.enc;
    AVDictionary* opt = NULL;
    int nb_samples;
    int ret;

    if ((ret = avcodec_open2(m_audio_stream.enc, codec, &opt)) < 0)
    {
        cerr << lock_ios()
             << "ERROR: Could not open audio codec: " << AVerr2str(ret) << endl;
        return false;
    }

    if (m_audio_stream.enc->codec->capabilities &
        AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
        nb_samples = 10000;
    else
    {
        nb_samples = m_audio_stream.enc->frame_size;
    }

    m_audio_stream.frame = alloc_audio_frame(m_audio_stream.enc->sample_fmt,
                                             &m_audio_stream.enc->ch_layout,
                                             m_audio_stream.enc->sample_rate,
                                             nb_samples);
    if (m_audio_stream.frame == nullptr)
    {
        Shutdown();
        return false;
    }

    if (m_audioIO.BytesPerSample() == 4)
        m_audio_stream.tmp_frame = alloc_audio_frame(AV_SAMPLE_FMT_S32,
                                           &m_audio_stream.enc->ch_layout,
                                           m_audio_stream.enc->sample_rate,
                                                     nb_samples);
    else
        m_audio_stream.tmp_frame = alloc_audio_frame(AV_SAMPLE_FMT_S16,
                                             &m_audio_stream.enc->ch_layout,
                                              m_audio_stream.enc->sample_rate,
                                                     nb_samples);

    if (m_audio_stream.tmp_frame == nullptr)
    {
        cerr << lock_ios()
             << "ERROR: Unable to allocate a temporary audio frame.\n";
        Shutdown();
        return false;
    }

    /* create resampler context */
    m_audio_stream.swr_ctx = swr_alloc();
    if (!m_audio_stream.swr_ctx)
    {
        cerr << lock_ios()
             << "ERROR: Could not allocate resampler context\n";
        return false;
    }

    /* set options */
    av_opt_set_chlayout  (m_audio_stream.swr_ctx, "in_chlayout",
                          &m_audio_stream.enc->ch_layout,     0);
    av_opt_set_int       (m_audio_stream.swr_ctx, "in_sample_rate",
                          m_audio_stream.enc->sample_rate,    0);

    if (m_audioIO.BytesPerSample() == 4)
    {
        av_opt_set_sample_fmt(m_audio_stream.swr_ctx, "in_sample_fmt",
                              AV_SAMPLE_FMT_S32, 0);
//        ctx->bits_per_raw_sample = 24;
    }
    else
        av_opt_set_sample_fmt(m_audio_stream.swr_ctx, "in_sample_fmt",
                              AV_SAMPLE_FMT_S16, 0);

    av_opt_set_chlayout  (m_audio_stream.swr_ctx, "out_chlayout",
                          &m_audio_stream.enc->ch_layout,     0);
    av_opt_set_int       (m_audio_stream.swr_ctx, "out_sample_rate",
                          m_audio_stream.enc->sample_rate,    0);
    av_opt_set_sample_fmt(m_audio_stream.swr_ctx, "out_sample_fmt",
                          m_audio_stream.enc->sample_fmt,     0);

    /* initialize the resampling context */
    if ((ret = swr_init(m_audio_stream.swr_ctx)) < 0)
    {
        cerr << lock_ios()
             << "ERROR: Failed to initialize the resampling context\n";
        return false;
    }

    return true;
}

bool OutputTS::open_video(void)
{
    close_encoder(&m_video_stream);

    AVDictionary* opt = NULL;
    const AVCodec* video_codec =
        avcodec_find_encoder_by_name(m_video_codec_name.c_str());
    if (video_codec)
    {
        if (m_verbose > 0)
        {
            cerr << lock_ios()
                 << "Video codec: " << video_codec->id << " : "
                 << video_codec->name << " '"
                 << video_codec->long_name << "' "
                 << endl;
        }
    }
    else
    {
        cerr << lock_ios()
             << "ERROR: Could not find video encoder for '"
             << m_video_codec_name << "'\n";
        return false;
    }

    m_video_stream.tmp_pkt = av_packet_alloc();
    if (!m_video_stream.tmp_pkt)
    {
        cerr << lock_ios()
        << "ERROR: Could not allocate AVPacket\n";
        return false;
    }

    m_video_stream.enc = avcodec_alloc_context3(video_codec);
    if (!m_video_stream.enc)
    {
        cerr << lock_ios()
             << "ERROR: Could not alloc an encoding context\n";
        return false;
    }
    m_video_stream.next_pts = 0;

    m_video_stream.enc->codec_id = (video_codec)->id;

//          m_video_stream.enc->bit_rate = m_video_bitrate;
    /* Resolution must be a multiple of two. */
    m_video_stream.enc->width    = m_input_width;
    m_video_stream.enc->height   = m_input_height;
    /* timebase: This is the fundamental unit of time (in
     * seconds) in terms of which frame timestamps are
     * represented. For fixed-fps content, timebase should be
     * 1/framerate and timestamp increments should be identical
     * to 1. */
    m_video_stream.enc->time_base = AVRational{m_input_frame_rate.den,
                                               m_input_frame_rate.num};
#if 0
    m_video_stream.enc->gop_size      = 12; /* emit one intra frame every twelve frames at most */
#endif

    /*
      For av1_qsv, pix_fmt options are:
      AV_PIX_FMT_NV12, AV_PIX_FMT_P010, AV_PIX_FMT_QSV
    */

    if (m_isHDR)
    {
        if (m_verbose > 0)
            cerr << lock_ios()
                 << "Open video stream with HDR.\n";
#if 1
        // Full color range
        m_video_stream.enc->color_range     = AVCOL_RANGE_JPEG;
#else
        // Limited color range
        m_video_stream.enc->color_range     = AVCOL_RANGE_MPEG;
#endif
    }
    else
        m_video_stream.enc->color_range     = AVCOL_RANGE_UNSPECIFIED;

    m_video_stream.enc->color_primaries = m_color_primaries;
    m_video_stream.enc->color_trc       = m_color_trc;
    m_video_stream.enc->colorspace      = m_color_space;

    if (m_encoderType == EncoderType::QSV)
        m_video_stream.enc->pix_fmt = AV_PIX_FMT_QSV;
    else if (m_encoderType == EncoderType::VAAPI)
        m_video_stream.enc->pix_fmt = AV_PIX_FMT_VAAPI;
    else
        m_video_stream.enc->pix_fmt = AV_PIX_FMT_YUV420P;

    if (m_video_stream.enc->codec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
    {
        m_video_stream.enc->thread_type = FF_THREAD_SLICE;
        if (m_verbose > 1)
            cerr << lock_ios()
                 << " Video = THREAD SLICE\n";
    }
    else if (m_video_stream.enc->codec->capabilities & AV_CODEC_CAP_FRAME_THREADS)
    {
        m_video_stream.enc->thread_type = FF_THREAD_FRAME;
        if (m_verbose > 1)
            cerr << lock_ios()
                 << " Video = THREAD FRAME\n";
    }

    if (m_verbose > 1)
    {
        cerr << lock_ios()
             << "Output stream Video: " << m_video_stream.enc->width
             << "x" << m_video_stream.enc->height
             << (m_interlaced ? 'i' : 'p')
#if 0
             << " time_base: " << m_video_stream.st->time_base.num
             << "/" << m_video_stream.st->time_base.den
#endif
             << "\n";
    }

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
          cerr << lock_ios()
               << "ERROR: Could not determine video encoder type.\n";
          return false;
    }

    return true;
}

bool OutputTS::open_container(void)
{
    int ret;
    AVDictionary* opt = NULL;

#if 0
    if (!m_audio_ready)
    {
        cerr << lock_ios() << "open_container: Audio not ready\n";
        return true;
    }
#endif

    close_container();

    if (m_running.load() == false)
        return false;

#if 1
    if (m_verbose > 1)
        cerr << lock_ios()
             << "\n================== open_container begin ==================\n";
#endif

    /* allocate the output media context */
    avformat_alloc_output_context2(&m_output_format_context,
                                   NULL, "mpegts", NULL);
    if (!m_output_format_context)
    {
        cerr << lock_ios()
             << "ERROR: Could not create output format context.\n";
        Shutdown();
        return false;
    }

    m_fmt = m_output_format_context->oformat;

    /* Video */
    m_video_stream.st = avformat_new_stream(m_output_format_context, NULL);
    if (!m_video_stream.st)
    {
        cerr << lock_ios()
             << "ERROR: Could not allocate video stream\n";
        return false;
    }
    m_video_stream.st->id = 0;
    m_video_stream.st->time_base = m_video_stream.enc->time_base;
    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(m_video_stream.st->codecpar,
                                          m_video_stream.enc);
    if (ret < 0)
    {
        cerr << lock_ios()
             << "ERROR: Could not copy the stream parameters." << endl;
        Shutdown();
        return false;
    }

    if (m_audio_stream.enc)
    {
        /* Audio */
        m_audio_stream.st = avformat_new_stream(m_output_format_context, NULL);
        if (!m_audio_stream.st)
        {
            cerr << lock_ios()
                 << "ERROR: Could not allocate stream\n";
            return false;
        }
        m_audio_stream.st->id = 1;
        m_audio_stream.st->time_base =
            (AVRational){ 1, m_audio_stream.enc->sample_rate };
        if (m_verbose > 1)
        {
            cerr << lock_ios()
                 << "Audio time base " << m_audio_stream.st->time_base.num << "/"
                 << m_audio_stream.st->time_base.den << "\n";
        }
        /* copy the stream parameters to the muxer */
        ret = avcodec_parameters_from_context(m_audio_stream.st->codecpar,
                                              m_audio_stream.enc);
        if (ret < 0)
        {
            cerr << lock_ios()
                 << "ERROR: Could not copy the stream parameters\n";
            return false;
        }
    }

    /* Transport Stream elements */
    if (m_verbose > 0)
        av_dump_format(m_output_format_context, 0, m_filename.c_str(), 1);

    /* open the output file, if needed */
    if (!(m_fmt->flags & AVFMT_NOFILE)) {
        ret = avio_open(&m_output_format_context->pb,
                        m_filename.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            cerr << lock_ios()
                 << "ERROR: Could not open '" << m_filename << "': "
                 << AVerr2str(ret) << endl;
            Shutdown();
            return false;
        }
    }

    /* Write the stream header, if any. */
    ret = avformat_write_header(m_output_format_context, &opt);
    if (ret < 0)
    {
        cerr << lock_ios()
             << "ERROR: Could not open output file: %s\n"
             << AVerr2str(ret) << endl;
        Shutdown();
        return false;
    }

#if 1
    if (m_verbose > 1)
        cerr << lock_ios()
             << "\n================== open_container end ==================\n";
#endif
    m_init_needed = false;
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

    if (m_verbose > 2)
        cerr << lock_ios()
             << "setAudioParams " << (is_lpcm ? "LPCM" : "Bitstream") << endl;

    return true;
}

bool OutputTS::setVideoParams(int width, int height, bool interlaced,
                              AVRational time_base, double frame_duration,
                              AVRational frame_rate, bool is_hdr)
{
    unique_lock<mutex> lock(m_image_mutex);

    m_frame_queue_empty.wait(lock,
                             [this]{return m_frame_queue.empty() ||
                                     !m_running.load(); });

    m_input_width = width;
    m_input_height = height;
    m_interlaced = interlaced;
    m_input_time_base = time_base;
    m_input_frame_duration = frame_duration;
    m_input_frame_wait_ms = frame_duration / 10000;
    m_input_frame_rate = frame_rate;
    m_isHDR = is_hdr;

    double fps = static_cast<double>(frame_rate.num) / frame_rate.den;

    if (m_verbose > 0)
    {
        cerr << lock_ios()
             << "Video: " << width << "x" << height
             << (m_interlaced ? 'i' : 'p') << fps
             << (m_isHDR ? " HDR" : "") << endl;
        if (m_verbose > 2)
            cerr << lock_ios()
                 << "Video Params set\n";
    }

    if (!open_video())
    {
        cerr << lock_ios() << "ERROR: Failed to create video stream\n";
        return false;
    }

    m_init_needed |= !m_audioIO.ChangePending();

    return true;
}

OutputTS::~OutputTS(void)
{
    Shutdown();

    if (m_image_thread.joinable())
        m_image_thread.join();
    if (m_frame_thread.joinable())
        m_frame_thread.join();

    if (m_video_stream.hw_device_ctx != nullptr)
        av_buffer_unref(&m_video_stream.hw_device_ctx);

    close_container();

    av_freep(&m_display_primaries);
    av_freep(&m_content_light);
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
            cerr << lock_ios()
                 << "WARNING: Failed sending a frame to the encoder: "
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
                cerr << lock_ios()
                     << "WARNING: Failed encoding a frame: AVerr2str(ret)\n";
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
        log_packet("wrt(audio)", fmt_ctx, pkt);
#endif

        ret = av_interleaved_write_frame(fmt_ctx, pkt);
        /* pkt is now blank (av_interleaved_write_frame() takes ownership of
         * its contents and resets pkt), so that no unreferencing is necessary.
         * This would be different if one used av_write_frame(). */
        if (ret < 0)
        {
            if (m_verbose > 0)
            {
                cerr << lock_ios()
                     << "WARNING: Failed to write packet: " << AVerr2str(ret)
                     << "\n";
            }

            if (m_verbose > 1)
            {
                cerr << lock_ios()
                     << "Codec time base " << codec_ctx->time_base.num
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
    {
        /* Close the output file. */
        avio_closep(&m_output_format_context->pb);

        m_fmt = nullptr;
    }

    /* free the output stream */
    if (m_output_format_context)
    {
        avformat_free_context(m_output_format_context);
        m_output_format_context = nullptr;
    }

    /* Documentation implies that avformat_free_context will
     * free all of the associated streams.
     */
    m_video_stream.st = nullptr;
    m_audio_stream.st = nullptr;
}

void OutputTS::close_encoder(OutputStream* ost)
{
    if (!ost->enc)
        return;

    av_buffer_unref(&ost->enc->hw_frames_ctx);
    ost->enc->hw_frames_ctx = nullptr;
    ost->hw_device = false;

#if 1
    if (ost->tmp_frame /*  && ost->tmp_frame->data[0] */)
    {
#if 0
        av_frame_free(&ost->tmp_frame);
#else
        av_free(&ost->tmp_frame->data[0]);
#endif
        ost->tmp_frame = nullptr;
    }
#endif

    if (ost->swr_ctx)
    {
        swr_free(&ost->swr_ctx);
        ost->swr_ctx = nullptr;
    }

    /*
      FFmpeg docs now say:
      Opening and closing a codec context multiple times is not
      supported anymore – use multiple codec contexts instead.

      So, does this mean to never free a codect context?
    */
#if 0
    avcodec_free_context(&ost->enc);
#endif
    ost->enc = nullptr;
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
            cerr << lock_ios()
                 << "Not enough audio data.\n";
        return nullptr;
    }
    if (m_audioIO.Read(q, bytes) == 0)
        return nullptr;

    ost->timestamp = frame->pts = m_audioIO.TimeStamp();

#if 0
            cerr << " get_pcm ts: " << m_audio_stream.timestamp << endl;
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
#if 0
    if (ost->swr_ctx->in_sample_rate < 1)
    {
        cerr << lock_ios() << "WARNING: write_pcm_frame, but sample rate is not set!\n";
        return false;
    }
#endif
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
        cerr << lock_ios()
             << "WARNING: write_pcm_frame: Failed to make frame writable\n";
        return false;
    }

    /* convert to destination format */
    ret = swr_convert(ost->swr_ctx,
                      ost->frame->data, dst_nb_samples,
                      const_cast<const uint8_t** >(frame->data),
                      frame->nb_samples);
    if (ret < 0)
    {
        cerr << lock_ios()
             << "WARNING: write_pcm_frame: Error while converting\n";
        return false;
    }

    frame = ost->frame;
    frame->pts = av_rescale_q(ost->timestamp,
                              m_input_time_base,
                              enc_ctx->time_base);

    ost->samples_count += dst_nb_samples;

    return write_frame(oc, enc_ctx, frame, ost);
}

bool OutputTS::write_bitstream_frame(AVFormatContext* oc, OutputStream* ost)
{
    AVPacket* pkt = m_audioIO.ReadSPDIF();

    if (pkt == nullptr)
    {
        cerr << lock_ios() << "No data from S/PDIF\n";
        return false;
    }

    if (abs(m_audioIO.TimeStamp() - ost->next_timestamp) > 5)
    {
        cerr << lock_ios() << "BITSTREAM audio: expected TS\n"
             << ost->next_timestamp << " but got\n"
             << m_audioIO.TimeStamp() << "\n";
        m_audioIO.PrintPointers("write", true);
    }

#if 1
    int64_t duration = av_rescale_q(pkt->duration,
                                    ost->st->time_base,
                                    m_input_time_base);
    ost->next_timestamp = m_audioIO.TimeStamp() + duration;
#endif

#if 0
    if (duration == 0)
    {
        cerr << lock_ios() << "Audio pkt duration ZERO: "
             << pkt->duration << " ST->TB: "
             << ost->st->time_base.num << "/" << ost->st->time_base.den
             << " IN->TB: " << m_input_time_base.num
             << "/" << m_input_time_base.den << "\n";
    }
    else if (m_audioIO.TimeStamp() > ost->timestamp + duration)
    {
        uint8_t* data = new uint8_t[pkt->size];
        memcpy(data, pkt->data, pkt->size);

        cerr << lock_ios() << "write_bitstream_frame expected duration:\n"
             << duration << " but actual duration:\n"
             << m_audioIO.TimeStamp() - ost->timestamp << "\n";

        while (m_audioIO.TimeStamp() > ost->timestamp + duration)
        {
            cerr << '.';
#if 0
            if (!m_audioIO.RescanSPDIF())
                Shutdown();
#endif
            AVPacket* missing_pkt = av_packet_alloc();
            ost->timestamp += duration;
            missing_pkt->pts = av_rescale_q(ost->timestamp,
                                            m_input_time_base,
                                            ost->st->time_base);
            missing_pkt->dts = missing_pkt->pts;
            missing_pkt->stream_index = ost->st->index;
            missing_pkt->size = pkt->size;
            missing_pkt->data = data;
            missing_pkt->flags = pkt->flags;
            missing_pkt->duration = duration;

            int ret = av_interleaved_write_frame(oc, missing_pkt);
            /* pkt is now blank (av_interleaved_write_frame() takes
             * ownership of its contents and resets pkt), so that no
             * unreferencing is necessary.  This would be different if
             * one used av_write_frame(). */
            if (ret < 0)
            {
                cerr << lock_ios()
                     << "WARNING: Failed to write missing audio packet: "
                     << AVerr2str(ret) << "\n";
                return false;
            }
        }
    }
#endif

#if 1 // Use av_rescale_q
    ost->timestamp = m_audioIO.TimeStamp();
    pkt->pts = av_rescale_q(ost->timestamp,
                            m_input_time_base,
                            ost->st->time_base);

    ost->prev_audio_pts = pkt->pts;

    /* Frame size passed from magewell includes all channels */
#if 0
    ost->next_pts = ost->timestamp + ost->frame->nb_samples;
#else
    ost->next_pts = pkt->pts + pkt->duration;
#endif

//    pkt->duration = pkt->pts;
    pkt->dts = pkt->pts;
#else // Use av_packet_rescale_ts
    pkt->dts = pkt->pts = m_audioIO.TimeStamp();
    av_packet_rescale_ts(pkt, m_input_time_base, ost->st->time_base);
#endif

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
        cerr << lock_ios()
             << "WARNING: Failed to write audio packet: " << AVerr2str(ret)
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
    if (m_audio_stream.st == nullptr /* || !m_audioIO.BlockReady() */)
    {
        cerr << lock_ios() << "Audio stream not open.\n";
        return false;
    }

    /* This only partially protects against a race condition! */
    if (m_audioIO.ChangePending())
    {
        cerr << lock_ios() << "Audio codec change pending.\n";
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
        cerr << lock_ios()
             << "ERROR: Could not allocate " << desc->name
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
            cerr << lock_ios()
                 << "Using preset " << m_preset << " for "
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
        cerr << lock_ios()
             << "ERROR: Could not open video codec: "
             << AVerr2str(ret) << "\n";
        Shutdown();
        return false;
    }

    /* allocate and init a re-usable frame */
    ost->frame = alloc_picture(ctx->pix_fmt, ctx->width, ctx->height);
    if (!ost->frame)
    {
        cerr << lock_ios()
             << "ERROR: Could not allocate NVIDIA video frame\n";
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
            cerr << lock_ios()
                 << "ERROR: Could not allocate temporary picture." << endl;
            Shutdown();
            return false;
        }
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
        vector<string> drivers{ "iHD", "i965" };
        vector<string>::iterator Idriver;
        for (Idriver = drivers.begin(); Idriver != drivers.end(); ++Idriver)
        {
            static string envstr = "LIBVA_DRIVER_NAME=" + *Idriver;
            char* env = envstr.data();
            putenv(env);

            if ((ret = av_hwdevice_ctx_create(&ost->hw_device_ctx,
                                              AV_HWDEVICE_TYPE_VAAPI,
                                              m_device.c_str(), opt, 0)) < 0)
                cerr << lock_ios()
                     << "ERROR: Failed to open VAPPI driver '"
                     << *Idriver << "'\n";
            else
                break;
        }
        if (Idriver == drivers.end())
        {
            cerr << lock_ios()
                 << "ERROR: Failed to create a VAAPI device. Error code: "
                 << AVerr2str(ret) << endl;
            Shutdown();
            return false;
        }

        if (m_verbose > 0)
            cerr << lock_ios()
                 << "Using VAAPI driver '" << *Idriver << "'\n";
    }

    /* set hw_frames_ctx for encoder's AVCodecContext */
    if (!(hw_frames_ref = av_hwframe_ctx_alloc(ost->hw_device_ctx)))
    {
        cerr << lock_ios()
             << "ERROR: Failed to create VAAPI frame context.\n";
        Shutdown();
        return false;
    }
    frames_ctx = reinterpret_cast<AVHWFramesContext* >(hw_frames_ref->data);
    frames_ctx->format    = AV_PIX_FMT_VAAPI;
    if (m_isHDR)
        frames_ctx->sw_format = AV_PIX_FMT_P010;
    else
        frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width     = m_input_width;
    frames_ctx->height    = m_input_height;
    frames_ctx->initial_pool_size = 20;
    if ((ret = av_hwframe_ctx_init(hw_frames_ref)) < 0)
    {
        cerr << lock_ios()
             << "ERROR: Failed to initialize VAAPI frame context."
             << "Error code: " << AVerr2str(ret) << endl;
        av_buffer_unref(&hw_frames_ref);
        Shutdown();
        return false;
    }
    ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    if (!ctx->hw_frames_ctx)
    {
        ret = AVERROR(ENOMEM);
        cerr << lock_ios()
             << "ERROR: Failed to allocate hw frame buffer. "
             << "Error code: " << AVerr2str(ret) << endl;
        av_buffer_unref(&hw_frames_ref);
        Shutdown();
        return false;
    }
    av_buffer_unref(&hw_frames_ref);
    ost->hw_device = true;

    if ((ret = avcodec_open2(ctx, codec, &opt)) < 0)
    {
        cerr << lock_ios()
             << "ERROR: Cannot open VAAPI video encoder codec. Error code: "
             << AVerr2str(ret) << endl;
        Shutdown();
        return false;
    }

    /* allocate and init a re-usable frame */
    ost->frame = alloc_picture(frames_ctx->sw_format,
                               frames_ctx->width, frames_ctx->height);
    if (!ost->frame)
    {
        cerr << lock_ios()
             << "ERROR: Could not allocate VAAPI video frame\n";
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
                cerr << lock_ios()
                     << "Using preset " << m_preset << " for "
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
            cerr << lock_ios()
                 << "ERROR: Failed to open QSV on " << m_device << "\n";
            return false;
        }

        if (m_verbose > 0)
            cerr << lock_ios()
                 << "Using QSV\n";
    }

    /* set hw_frames_ctx for encoder's AVCodecContext */
    if (!(hw_frames_ref = av_hwframe_ctx_alloc(ost->hw_device_ctx)))
    {
        cerr << lock_ios()
             << "ERROR: Failed to create QSV frame context.\n";
        Shutdown();
        return false;
    }
    frames_ctx = reinterpret_cast<AVHWFramesContext* >(hw_frames_ref->data);

    if (m_isHDR)
    {
        if (m_verbose > 1)
            cerr << lock_ios()
                 << "Open QSV stream with HDR.\n";
        frames_ctx->sw_format = AV_PIX_FMT_P010;
    }
    else
        frames_ctx->sw_format = AV_PIX_FMT_NV12;

    frames_ctx->format    = AV_PIX_FMT_QSV;
    frames_ctx->width     = m_input_width;
    frames_ctx->height    = m_input_height;
    frames_ctx->initial_pool_size = 20;
    if ((ret = av_hwframe_ctx_init(hw_frames_ref)) < 0)
    {
        cerr << lock_ios()
             << "ERROR: Failed to initialize QSV frame context."
             << "Error code: " << AVerr2str(ret) << endl;
        av_buffer_unref(&hw_frames_ref);
        Shutdown();
        return false;
    }
    ost->enc->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    if (!ost->enc->hw_frames_ctx)
    {
        ret = AVERROR(ENOMEM);
        cerr << lock_ios()
             << "ERROR: Failed to allocate hw frame buffer. "
             << "Error code: " << AVerr2str(ret) << endl;
        av_buffer_unref(&hw_frames_ref);
        Shutdown();
        return false;
    }
    av_buffer_unref(&hw_frames_ref);
    ost->hw_device = true;

    if ((ret = avcodec_open2(ost->enc, codec, &opt)) < 0)
    {
        cerr << lock_ios()
             << "ERROR: Cannot open QSV video encoder codec. Error code: "
             << AVerr2str(ret) << endl;
        Shutdown();
        return false;
    }

    /* allocate and init a re-usable frame */
    ost->frame = alloc_picture(frames_ctx->sw_format,
                               frames_ctx->width, frames_ctx->height);
    if (!ost->frame)
    {
        cerr << lock_ios()
             << "ERROR: Could not allocate QSV video frame\n";
        Shutdown();
        return false;
    }

    return true;
}

AVFrame* OutputTS::nv_encode(AVFormatContext* oc,
                         OutputStream* ost,
                         uint8_t* pImage, void* pEco,
                         int image_size,
                         int64_t timestamp)
{
#if 0
    /* when we pass a frame to the encoder, it may keep a reference to it
     * internally; make sure we do not overwrite it here */
    if (av_frame_make_writable(ost->frame) < 0)
    {
        cerr << lock_ios()
             << "ERROR: get_video_frame: Make frame writable failed.\n";
        return nullptr;
    }
#endif

    // YUV 4:2:0
    memcpy(ost->frame->data[0], pImage, image_size);
    memcpy(ost->frame->data[1], pImage + image_size, image_size / 4);
    memcpy(ost->frame->data[2], pImage + image_size * 5 / 4, image_size / 4);
    f_image_buffer_available(pImage, pEco);

    if (m_isHDR)
    {
        AVMasteringDisplayMetadata* primaries =
            av_mastering_display_metadata_create_side_data(ost->frame);
        *primaries = *m_display_primaries;
        AVContentLightMetadata* light =
            av_content_light_metadata_create_side_data(ost->frame);
        *light = *m_content_light;
    }

    ost->frame->pts = av_rescale_q_rnd(timestamp, m_input_time_base,
                                       ost->enc->time_base,
               static_cast<AVRounding>(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));

    ost->timestamp = timestamp;
    ost->next_pts = timestamp + 1;

    return ost->frame;
}

AVFrame* OutputTS::qsv_vaapi_encode(AVFormatContext* oc,
                                    OutputStream* ost,
                                    uint8_t* pImage, void* pEco,
                                    int image_size,
                                    int64_t timestamp)
{
    AVFrame* hw_frame = nullptr;
    int    ret;

    int64_t pts = av_rescale_q(timestamp, m_input_time_base,
                               ost->enc->time_base);

    memcpy(ost->frame->data[0], pImage, image_size);
    memcpy(ost->frame->data[1], pImage + image_size, image_size / 2);
    f_image_buffer_available(pImage, pEco);

    if (m_isHDR)
    {
        AVMasteringDisplayMetadata* primaries =
            av_mastering_display_metadata_create_side_data(ost->frame);
        *primaries = *m_display_primaries;
        AVContentLightMetadata* light =
            av_content_light_metadata_create_side_data(ost->frame);
        *light = *m_content_light;
    }

    if (!(hw_frame = av_frame_alloc()))
    {
        cerr << lock_ios()
             << "ERROR: Failed to allocate hw frame.";
        Shutdown();
        return nullptr;
    }

    if ((ret = av_hwframe_get_buffer(ost->enc->hw_frames_ctx,
                                     hw_frame, 0)) < 0)
    {
        cerr << lock_ios()
             << "ERROR: Failed to get hw buffer: "
             << AV_ts2str(ret) << endl;
        Shutdown();
        return nullptr;
    }

    if (!hw_frame->hw_frames_ctx)
    {
        cerr << lock_ios()
             << "ERROR: Failed to allocate hw frame CTX.\n";
        Shutdown();
        return nullptr;
    }

    if ((ret = av_hwframe_transfer_data(hw_frame,
                                        ost->frame, 0)) < 0)
    {
        cerr << lock_ios()
             << "ERROR: failed transferring frame data to surface: "
             << AV_ts2str(ret) << endl;
        Shutdown();
        return nullptr;
    }

    hw_frame->pts = pts;
    ost->timestamp = timestamp;
    ost->next_pts = timestamp + 1;

    return hw_frame;
}

void OutputTS::mux(void)
{
    pkts_t::iterator Ipkt;
    pkts_t  pkts;

    int64_t  video_timestamp = -1;
    bool     ready;
    int      ret;

    while (m_running.load() == true)
    {
        if (m_audioIO.CodecChanged(ready))
        {
            close_container();
#if 1
            cerr << lock_ios() << " Audio changing: closing audio encoder\n";
#endif
            close_encoder(&m_audio_stream);
            if (ready)
            {
#if 1
            cerr << lock_ios() << " Audio changing: opening audio encoder\n";
#endif
                if (!open_audio())
                {
                    cerr << lock_ios()
                         << "ERROR: Failed to create audio stream\n";
                    Shutdown();
                    break;
                }
                m_audio_stream.timestamp = m_audioIO.TimeStamp();
                m_init_needed = true;
            }
        }

        if (m_init_needed)
        {
            if (m_video_stream.enc &&
                (!m_has_audio || m_audio_stream.enc != nullptr))
            {
                if (!open_container())
                {
                    Shutdown();
                    break;
                }
            }
            else
            {
                string why;
                if (m_video_stream.enc == nullptr)
                    why = " video";
                if (m_has_audio && m_audio_stream.enc == nullptr)
                    why += " audio";
                if (m_verbose > 1)
                    cerr << lock_ios() << "WARNING: New TS needed but"
                         << why << " encoder is not ready.\n";
            }
        }

        {
            unique_lock<mutex> lock(m_frame_mutex);

            if (m_frame_queue.empty())
            {
                m_frame_queue_empty.notify_one();
                m_frame_ready.wait_for(lock,
                               chrono::milliseconds(m_input_frame_wait_ms));
                continue;
            }

            video_timestamp = m_frame_queue.front().timestamp;

            if (m_output_format_context == nullptr)
            {
                if (m_verbose > 2)
                    cerr << lock_ios() << "####### mux: TS not open. "
                         << m_frame_queue.size() << " queued.\n";
                this_thread::sleep_for
                    (chrono::milliseconds(m_input_frame_wait_ms));
                continue;
            }

            if (!m_running.load())
                break;

            pkts = m_frame_queue.front().pkts;
            m_frame_queue.pop_front();
        }

        for (Ipkt = pkts.begin(); Ipkt != pkts.end(); ++Ipkt)
        {
            /* rescale output packet timestamp values from codec to stream
             * timebase
             */
            av_packet_rescale_ts((*Ipkt), m_video_stream.enc->time_base,
                                 m_video_stream.st->time_base);
            (*Ipkt)->stream_index = m_video_stream.st->index;
#if 0
            log_packet("mux(video)", m_output_format_context, *Ipkt);
#endif
            if ((ret =
                 av_interleaved_write_frame(m_output_format_context,
                                            (*Ipkt))) < 0)
            {
                cerr << lock_ios()
                     << "WARNING: Failed to write packet: " << AVerr2str(ret)
                     << "\n";
            }
            av_packet_free(&(*Ipkt));
        }

#if 0
        while (m_audioIO.TimeStamp() < video_timestamp)
#else
        while (m_audio_stream.next_timestamp < video_timestamp)
#endif
        {
#if 0
//        if (m_video_stream.next_pts <= m_audio_stream.next_pts)
            cerr << lock_ios()
                 << "Write: video " << m_video_stream.timestamp << "\n"
                 << "  audio [" << setw(2) << m_audioIO.BufId()
                 << "] " << m_audioIO.TimeStamp()
                 << " First " << m_audioIO.FirstTimeStamp() << endl;
#endif

            if (!write_audio_frame(m_output_format_context,
                                   &m_audio_stream))
                break;
        }

#if 0
        cerr << lock_ios() << " Wrote video pkts for " << video_timestamp
             << endl;
#endif
    }
}

void OutputTS::encode_video(void)
{
    AVFrame* frame;
    uint8_t* pImage;
    void*    pEco;
    int      image_size;
    int64_t  timestamp = -1;
    int      ret;

    while (m_running.load() == true)
    {
        {
            unique_lock<mutex> lock(m_image_mutex);
            if (m_image_queue.empty())
            {
                m_image_ready.wait_for(lock,
                               chrono::milliseconds(m_input_frame_wait_ms));
                continue;
            }

            pImage    = m_image_queue.front().image;
            pEco      = m_image_queue.front().pEco;
            image_size = m_image_queue.front().image_size;
            timestamp = m_image_queue.front().timestamp;

            m_image_queue.pop_front();
        }

        if (m_audioIO.FirstTimeStamp() == -1 ||
            m_audioIO.FirstTimeStamp() > timestamp)
        {
            cerr << lock_ios() << " Skipping video TS "
                 << timestamp << " first audio TS " << m_audioIO.FirstTimeStamp()
                 << endl;
            f_image_buffer_available(pImage, pEco);
            continue;
        }

        if (m_video_stream.enc == nullptr)
        {
#if 1
            cerr << lock_ios() << " encode_video: codec is not open\n";
#endif
            continue;
        }

        if (m_encoderType == EncoderType::NV)
            frame = nv_encode(m_output_format_context, &m_video_stream,
                              pImage, pEco, image_size, timestamp);
        else if (m_encoderType == EncoderType::QSV ||
                 m_encoderType == EncoderType::VAAPI)
            frame = qsv_vaapi_encode(m_output_format_context, &m_video_stream,
                                     pImage, pEco, image_size, timestamp);
        else
        {
            cerr << lock_ios()
                 << "ERROR: Unknown encoderType.\n";
            Shutdown();
            return;
        }

        if (frame == nullptr)
        {
            cerr << lock_ios() << "WARNING: Failed to encode the video frame.\n";
            continue;
        }

#if 0
        cerr << lock_ios() << "Frame pts " << frame->pts << endl;
#endif
        if ((ret = avcodec_send_frame(m_video_stream.enc, frame)) < 0)
        {
            cerr << lock_ios()
                 << "WARNING: Failed sending a frame to the encoder: "
                 << AVerr2str(ret) << "\n";
            Shutdown();
            break;
        }

        pkts_t   pkts;
        for (;;)
        {
            AVPacket* pkt = av_packet_alloc();
            ret = avcodec_receive_packet(m_video_stream.enc, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
            {
                cerr << lock_ios()
                     << "WARNING: Failed encoding a frame: AVerr2str(ret)\n";
                Shutdown();
                break;
            }

            pkt->stream_index = 1;

            if (m_video_stream.prev_dts >= pkt->dts)
                pkt->dts = m_video_stream.prev_dts + 1;
            m_video_stream.prev_dts = pkt->dts;

            if (pkt->pts < pkt->dts)
                pkt->pts = pkt->dts;

            pkts.push_back(pkt);
        }

        m_frame_queue.push_back({timestamp, pkts});
        av_frame_free(&frame);
        m_frame_ready.notify_one();
    }
}

void OutputTS::ClearImageQueue(void)
{
    const unique_lock<mutex> lock(m_image_mutex);
    imageque_t::iterator Iq;
    for (Iq = m_image_queue.begin(); Iq != m_image_queue.end(); ++Iq)
        f_image_buffer_available((*Iq).image, (*Iq).pEco);
    m_image_queue.clear();
}

bool OutputTS::AddVideoFrame(uint8_t* pImage, void* pEco,
                             int imageSize, int64_t timestamp)
{
    const unique_lock<mutex> lock(m_image_mutex);

    m_image_queue.push_back(imagepkt_t{timestamp, pImage, pEco, imageSize});

    m_image_ready.notify_one();
    return m_running.load();
}
