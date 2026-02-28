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
 * @file OutputTS.cpp
 * @brief Implementation of Transport Stream output functionality for video/audio encoding
 * @author John Patrick Poet
 * @date 2022-2025
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

/**
 * @brief Convert AV timestamp to string representation
 * @param ts Timestamp value to convert
 * @return String representation of timestamp
 * @note This function is used for debugging and logging purposes
 */
std::string AV_ts2str(int64_t ts)
{
    char astr[AV_TS_MAX_STRING_SIZE] = { 0 };
    av_ts_make_string(astr, ts);
    return string(astr);
}

/**
 * @brief Convert AV timestamp to time string with timebase
 * @param ts Timestamp value to convert
 * @param tb Timebase for conversion
 * @return String representation of timestamp in seconds
 * @note This function is used for debugging and logging purposes
 */
std::string AV_ts2timestr(int64_t ts, AVRational* tb)
{
    ostringstream os;
    os << av_q2d(*tb) * ts;
    return os.str();
}

/**
 * @brief Convert AV error code to string representation
 * @param code Error code to convert
 * @return String representation of error code
 * @note This function is used for debugging and logging purposes
 */
static std::string AVerr2str(int code)
{
    char astr[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_make_error_string(astr, AV_ERROR_MAX_STRING_SIZE, code);
    return string(astr);
}

/**
 * @brief Log packet information for debugging
 * @param where Location where packet is being logged
 * @param fmt_ctx Format context for packet
 * @param pkt Packet to log
 * @note This function is used for debugging and logging purposes
 */
static void log_packet(string where, const AVFormatContext* fmt_ctx,
                       const AVPacket* pkt)
{
    AVRational* time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

    clog << lock_ios()
         << where << "[" << pkt->stream_index << "] pts: " << pkt->pts
         << " pts_time: " << AV_ts2timestr(pkt->pts, time_base)
         << " dts: " << AV_ts2str(pkt->dts)
         << " dts_time: " << AV_ts2timestr(pkt->dts, time_base)
         << " duration: " << AV_ts2str(pkt->duration)
         << " duration_time: " << AV_ts2timestr(pkt->duration, time_base)
         << endl;
}

/**
 * @brief Constructor for OutputTS class
 * @param verbose_level Verbose level for logging
 * @param video_codec_name Name of video codec to use
 * @param preset Encoding preset
 * @param quality Quality setting for encoding
 * @param look_ahead Look ahead setting for encoding
 * @param p010 Flag to use P010 format
 * @param device GPU device identifier
 * @param shutdown Callback for shutdown events
 * @param reset Callback for reset events
 * @param image_buffer_avail Callback for image buffer availability
 * @note Initializes the output TS handler with specified parameters
 */
OutputTS::OutputTS(int verbose_level, const string & video_codec_name,
                   const string & preset, int quality, int look_ahead,
                   bool p010, const string & device,
                   ShutdownCallback shutdown, ResetCallback reset,
                   MagCallback image_buffer_avail)
    : m_verbose(verbose_level)
    , m_video_codec_name(video_codec_name)
    , m_device("/dev/dri/" + device)
    , m_preset(preset)
    , m_quality(quality)
    , m_look_ahead(look_ahead)
    , m_p010(p010)
    , f_shutdown(shutdown)
    , f_reset(reset)
    , f_image_buffer_available(image_buffer_avail)
{
    // Determine encoder type based on codec name
    if (m_video_codec_name.find("qsv") != string::npos)
        m_encoderType = EncoderType::QSV;
    else if (m_video_codec_name.find("vaapi") != string::npos)
        m_encoderType = EncoderType::VAAPI;
    else if (m_video_codec_name.find("nvenc") != string::npos)
        m_encoderType = EncoderType::NV;
    else
    {
        m_encoderType = EncoderType::UNKNOWN;
        clog << lock_ios()
             << "ERROR: Codec '" << m_video_codec_name << "' not supported."
             << endl;
        Shutdown();
    }

    // Start muxing and copying threads
    m_mux_thread = std::thread(&OutputTS::mux, this);
    pthread_setname_np(m_mux_thread.native_handle(), "mux");

    m_copy_thread = std::thread(&OutputTS::copy_to_frame, this);
    pthread_setname_np(m_copy_thread.native_handle(), "copy");

    // Allocate HDR metadata structures
    m_display_primaries  = av_mastering_display_metadata_alloc();
    m_content_light  = av_content_light_metadata_alloc(NULL);
}

/**
 * @brief Destructor for OutputTS class
 * @note Cleans up all resources including threads, buffers, and FFmpeg contexts
 */
OutputTS::~OutputTS(void)
{
    Shutdown();

    // Wait for threads to finish
    if (m_copy_thread.joinable())
        m_copy_thread.join();

    if (m_mux_thread.joinable())
        m_mux_thread.join();

    // Free HDR metadata structures
    av_freep(&m_display_primaries);
    av_freep(&m_content_light);

    // Close streams and container
    close_stream(&m_video_stream);
    if (m_video_stream.hw_device_ctx != nullptr)
        av_buffer_unref(&m_video_stream.hw_device_ctx);

    close_stream(&m_audio_stream);
    close_container();
}

/**
 * @brief Shutdown the output TS handler
 * @note Stops all threads and cleans up resources
 */
void OutputTS::Shutdown(void)
{
    if (m_running.exchange(false))
    {
        f_shutdown();
        m_audioIO->Shutdown();
    }
}

/**
 * @brief Set HDR light metadata
 * @param display_meta Pointer to mastering display metadata
 * @param light_meta Pointer to content light metadata
 * @note Copies HDR metadata for use in video encoding
 */
void OutputTS::setLight(AVMasteringDisplayMetadata * display_meta,
                        AVContentLightMetadata * light_meta)
{
    if (display_meta && light_meta)
    {
        *m_display_primaries = *display_meta;
        *m_content_light = *light_meta;
    }
}

/**
 * @brief Allocate an audio frame with specified parameters
 * @param sample_fmt Audio sample format
 * @param channel_layout Channel layout for audio
 * @param sample_rate Sample rate for audio
 * @param nb_samples Number of samples in frame
 * @return Pointer to allocated AVFrame or nullptr on error
 * @note This function allocates memory for audio frames and sets up basic properties
 */
AVFrame* OutputTS::alloc_audio_frame(enum AVSampleFormat sample_fmt,
                                     const AVChannelLayout* channel_layout,
                                     int sample_rate, int nb_samples)
{
    AVFrame* frame = av_frame_alloc();
    int ret;

    // Check if frame allocation succeeded
    if (!frame)
    {
        clog << lock_ios()
             << "ERROR: Failed to allocate an audio frame." << endl;
        return nullptr;
    }

    // Set frame properties
    frame->format = sample_fmt;
    av_channel_layout_copy(&frame->ch_layout, channel_layout);
    frame->sample_rate = sample_rate;
    frame->nb_samples = nb_samples;

    // Allocate buffer for frame data if samples exist
    if (nb_samples)
    {
        ret = av_frame_get_buffer(frame, 0);
        if (ret < 0)
        {
            clog << lock_ios()
                 << "ERROR: failed to allocate an audio buffer" << endl;
            av_frame_free(&frame);
            return nullptr;
        }
    }

    return frame;
}

/**
 * @brief Open audio encoder for output
 * @return true on success, false on failure
 * @note Sets up audio encoder parameters and opens codec
 */
bool OutputTS::open_audio(void)
{
    close_encoder(&m_audio_stream);

    int idx;

    // Log audio stream addition if verbose level is high enough
    if (m_verbose > 1)
    {
        clog << lock_ios()
             << "Adding audio stream." << endl;
    }

    const AVCodec* audio_codec = nullptr;

    // Find audio codec by name
    audio_codec = avcodec_find_encoder_by_name(m_audioIO->CodecName().c_str());
    if (!audio_codec)
    {
        clog << lock_ios()
             << "WARNING: Could not find audio encoder for '"
             << m_audioIO->CodecName() << "'" << endl;
        return true;
    }

    // Allocate temporary packet
    m_audio_stream.tmp_pkt = av_packet_alloc();
    if (!m_audio_stream.tmp_pkt)
    {
        clog << lock_ios()
             << "ERROR: Could not allocate AVPacket" << endl;
        return false;
    }

    // Allocate codec context
    m_audio_stream.enc = avcodec_alloc_context3(audio_codec);
    if (!m_audio_stream.enc)
    {
        clog << lock_ios()
             << "ERROR: Could not alloc an encoding context" << endl;
        return false;
    }
    m_audio_stream.next_pts = 0;

    // Set bit rate based on channel count
    if (m_audioIO->NumChannels() == 2)
        m_audio_stream.enc->bit_rate = 256000;
    else
        m_audio_stream.enc->bit_rate = 640000;

    // Handle different FFmpeg versions for codec configuration
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(61, 13, 100)
    m_audio_stream.enc->sample_fmt = audio_codec->sample_fmts ?
                         audio_codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;

    if (audio_codec->supported_samplerates)
    {
        m_audio_stream.enc->sample_rate = audio_codec->supported_samplerates[0];
        for (idx = 0; audio_codec->supported_samplerates[idx]; ++idx)
        {
            if (audio_codec->supported_samplerates[idx] == m_audioIO->SampleRate())
            {
                m_audio_stream.enc->sample_rate = m_audioIO->SampleRate();
                break;
            }
        }
    }
    else
        m_audio_stream.enc->sample_rate = 48000;

    av_channel_layout_copy(&m_audio_stream.enc->ch_layout,
                           m_audioIO->ChannelLayout());
#else
    int count = 0;

    enum AVSampleFormat *sample_fmts = nullptr;
    avcodec_get_supported_config(m_audio_stream.enc,
                                 audio_codec,
                                 AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                 0,
                                 const_cast<const void**>(reinterpret_cast<void**>(&sample_fmts)),
                                 &count);
    if (!sample_fmts)
        m_audio_stream.enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
    else
        m_audio_stream.enc->sample_fmt = sample_fmts[0];

    const int *sample_rates;
    avcodec_get_supported_config(m_audio_stream.enc,
                                 NULL,
                                 AV_CODEC_CONFIG_SAMPLE_RATE,
                                 0,
                                 reinterpret_cast<const void**>(&sample_rates),
                                 &count);

    if (!sample_rates)
        m_audio_stream.enc->sample_rate = 48000;
    else
    {
        m_audio_stream.enc->sample_rate = sample_rates[0];
        for (idx = 0; idx < count; ++idx)
        {
            if (sample_rates[idx] == m_audioIO->SampleRate())
            {
                m_audio_stream.enc->sample_rate = m_audioIO->SampleRate();
                break;
            }
        }
    }

    const AVChannelLayout *ch_layouts;
    av_channel_layout_copy(&m_audio_stream.enc->ch_layout,
                           m_audioIO->ChannelLayout());
    avcodec_get_supported_config(m_audio_stream.enc,
                                 NULL,
                                 AV_CODEC_CONFIG_CHANNEL_LAYOUT,
                                 0,
                                 reinterpret_cast<const void**>(&ch_layouts),
                                 &count);
    if (ch_layouts)
    {
        for (idx = 0; idx < count; ++idx)
        {
            if (!av_channel_layout_compare(&m_audio_stream.enc->ch_layout,
                                           &ch_layouts[idx]))
                break;
        }
        if (idx == count)
        {
            char buf[512];
            av_channel_layout_describe(&m_audio_stream.enc->ch_layout, buf, sizeof(buf));
            clog << lock_ios();
            clog << "Channel layout " << buf << " is not supportr by the "
                 << m_audio_stream.enc->codec->name << " encoder.\n";
            clog << m_audio_stream.enc->codec->name << " encoder supports:\n";
            for (idx = 0; idx < count; ++idx)
            {
                av_channel_layout_describe(&ch_layouts[idx], buf, sizeof(buf));
                clog << "    " << buf << '\n';
            }
        }
    }
#endif

    // Set threading options
    if (m_audio_stream.enc->codec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
    {
        m_audio_stream.enc->thread_type = FF_THREAD_SLICE;
        if (m_verbose > 1)
            clog << lock_ios()
                 << " Audio = THREAD SLICE\n";
    }
    else if (m_audio_stream.enc->codec->capabilities &
             AV_CODEC_CAP_FRAME_THREADS)
    {
        m_audio_stream.enc->thread_type = FF_THREAD_FRAME;
        if (m_verbose > 1)
            clog << lock_ios()
                 << " Audio = THREAD FRAME\n";
    }

    const AVCodec* codec = audio_codec;
    AVDictionary* opt = NULL;
    int nb_samples;
    int ret;

    // Open audio codec
    if ((ret = avcodec_open2(m_audio_stream.enc, codec, &opt)) < 0)
    {
        clog << lock_ios()
             << "ERROR: Could not open audio codec: " << AVerr2str(ret) << endl;
        return false;
    }

    // Determine frame size
    if (m_audio_stream.enc->codec->capabilities &
        AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
        nb_samples = 10000;
    else
    {
        nb_samples = m_audio_stream.enc->frame_size;
    }

    // Allocate audio frame
    m_audio_stream.frame = alloc_audio_frame(m_audio_stream.enc->sample_fmt,
                                             &m_audio_stream.enc->ch_layout,
                                             m_audio_stream.enc->sample_rate,
                                             nb_samples);
    if (m_audio_stream.frame == nullptr)
    {
        Shutdown();
        return false;
    }

    // Allocate temporary frame for format conversion
    if (m_audioIO->BytesPerSample() == 4)
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
        clog << lock_ios()
             << "ERROR: Unable to allocate a temporary audio frame." << endl;
        Shutdown();
        return false;
    }

    // Create resampler context
    m_audio_stream.swr_ctx = swr_alloc();
    if (!m_audio_stream.swr_ctx)
    {
        clog << lock_ios()
             << "ERROR: Could not allocate resampler context" << endl;
        return false;
    }

    // Configure resampler context
    av_opt_set_chlayout  (m_audio_stream.swr_ctx, "in_chlayout",
                          &m_audio_stream.enc->ch_layout,     0);
    av_opt_set_int       (m_audio_stream.swr_ctx, "in_sample_rate",
                          m_audio_stream.enc->sample_rate,    0);

    if (m_audioIO->BytesPerSample() == 4)
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

    // Initialize resampler context
    if ((ret = swr_init(m_audio_stream.swr_ctx)) < 0)
    {
        clog << lock_ios()
             << "ERROR: Failed to initialize the resampling context" << endl;
        return false;
    }

    return true;
}

/**
 * @brief Open video encoder for output
 * @return true on success, false on failure
 * @note Sets up video encoder parameters and opens codec
 */
bool OutputTS::open_video(void)
{
    close_encoder(&m_video_stream);

    // Reset reusable frames
    if (m_video_stream.frames != nullptr)
    {
        for (int idx = 0; idx < m_video_stream.frames_total; ++idx)
        {
            av_frame_free(&m_video_stream.frames[idx].frame);
            m_video_stream.frames[idx].frame = nullptr;
        }
        delete[] m_video_stream.frames;
        m_video_stream.frames = nullptr;
    }
    m_video_stream.frame = nullptr;
    m_video_stream.frames_idx_in  = -1;
    m_video_stream.frames_idx_out = -1;
    m_video_stream.frames_used    = 0;
    m_video_stream.frames_total   = m_frame_buffers;

    AVDictionary* opt = NULL;
    const AVCodec* video_codec =
        avcodec_find_encoder_by_name(m_video_codec_name.c_str());

    // Find video codec
    if (video_codec)
    {
        if (m_verbose > 0)
        {
            clog << lock_ios()
                 << "Video codec: " << video_codec->id << " : "
                 << video_codec->name << " '"
                 << video_codec->long_name << "' "
                 << endl;
        }
    }
    else
    {
        clog << lock_ios()
             << "ERROR: Could not find video encoder for '"
             << m_video_codec_name << "'" << endl;
        return false;
    }

    // Allocate temporary packet
    m_video_stream.tmp_pkt = av_packet_alloc();
    if (!m_video_stream.tmp_pkt)
    {
        clog << lock_ios()
             << "ERROR: Could not allocate AVPacket" << endl;
        return false;
    }

    // Allocate codec context
    m_video_stream.enc = avcodec_alloc_context3(video_codec);
    if (!m_video_stream.enc)
    {
        clog << lock_ios()
             << "ERROR: Could not alloc an encoding context" << endl;

        av_packet_free(&m_video_stream.tmp_pkt);
        return false;
    }
    m_video_stream.next_pts = 0;

    // Set codec parameters
    m_video_stream.enc->codec_id = (video_codec)->id;
    m_video_stream.enc->width    = m_input_width;
    m_video_stream.enc->height   = m_input_height;
    m_video_stream.enc->time_base = AVRational{m_input_frame_rate.den,
                                               m_input_frame_rate.num};

    // Set HDR color range
    if (m_isHDR)
    {
        if (m_verbose > 0)
            clog << lock_ios()
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

    // Set threading options
    if (m_video_stream.enc->codec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
    {
        m_video_stream.enc->thread_type = FF_THREAD_SLICE;
        if (m_verbose > 1)
            clog << lock_ios()
                 << " Video = THREAD SLICE\n";
    }
    else if (m_video_stream.enc->codec->capabilities & AV_CODEC_CAP_FRAME_THREADS)
    {
        m_video_stream.enc->thread_type = FF_THREAD_FRAME;
        if (m_verbose > 1)
            clog << lock_ios()
                 << " Video = THREAD FRAME\n";
    }

    // Log video parameters
    if (m_verbose > 1)
    {
        clog << lock_ios()
             << "Output stream Video: " << m_video_stream.enc->width
             << "x" << m_video_stream.enc->height
             << (m_interlaced ? 'i' : 'p')
#if 0
             << " time_base: " << m_video_stream.st->time_base.num
             << "/" << m_video_stream.st->time_base.den
#endif
             << "\n";
    }

    // Open encoder based on type
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
          clog << lock_ios()
               << "ERROR: Could not determine video encoder type." << endl;
          return false;
    }

    // Set HDR metadata for frames
    if (m_isHDR)
    {
        for (int idx = 0; idx < m_video_stream.frames_total; ++idx)
        {
            AVFrame* frm = m_video_stream.frames[idx].frame;

            AVMasteringDisplayMetadata* primaries =
                av_mastering_display_metadata_create_side_data(frm);
            *primaries = *m_display_primaries;
            AVContentLightMetadata* light =
                av_content_light_metadata_create_side_data(frm);
            *light = *m_content_light;
        }
    }

    return true;
}

/**
 * @brief Open output container for muxing
 * @return true on success, false on failure
 * @note Sets up container format and opens output file
 */
bool OutputTS::open_container(void)
{
    int ret;
    AVDictionary* opt = NULL;

    close_container();

    if (m_running.load() == false)
        return false;

#if 1
    if (m_verbose > 1)
        clog << lock_ios()
             << "\n================== open_container begin ==================\n";
#endif

    // Allocate output format context
    avformat_alloc_output_context2(&m_output_format_context,
                                   NULL, "mpegts", NULL);
    if (!m_output_format_context)
    {
        clog << lock_ios()
             << "ERROR: Could not create output format context." << endl;
        Shutdown();
        return false;
    }

    m_fmt = m_output_format_context->oformat;

    // Create video stream
    m_video_stream.st = avformat_new_stream(m_output_format_context, NULL);
    if (!m_video_stream.st)
    {
        clog << lock_ios()
             << "ERROR: Could not allocate video stream" << endl;
        return false;
    }
    m_video_stream.st->id = 0;
    m_video_stream.st->time_base = m_video_stream.enc->time_base;

    // Copy stream parameters
    ret = avcodec_parameters_from_context(m_video_stream.st->codecpar,
                                          m_video_stream.enc);
    if (ret < 0)
    {
        clog << lock_ios()
             << "ERROR: Could not copy the stream parameters." << endl;
        Shutdown();
        return false;
    }

    // Create audio stream if enabled
    if (m_audio_stream.enc)
    {
        /* Audio */
        m_audio_stream.st = avformat_new_stream(m_output_format_context, NULL);
        if (!m_audio_stream.st)
        {
            clog << lock_ios()
                 << "ERROR: Could not allocate stream" << endl;
            return false;
        }
        m_audio_stream.st->id = 1;
        m_audio_stream.st->time_base =
            (AVRational){ 1, m_audio_stream.enc->sample_rate };
        if (m_verbose > 1)
        {
            clog << lock_ios()
                 << "Audio time base " << m_audio_stream.st->time_base.num << "/"
                 << m_audio_stream.st->time_base.den << "\n";
        }
        // Copy stream parameters
        ret = avcodec_parameters_from_context(m_audio_stream.st->codecpar,
                                              m_audio_stream.enc);
        if (ret < 0)
        {
            clog << lock_ios()
                 << "ERROR: Could not copy the stream parameters" << endl;
            return false;
        }
    }

    // Dump format information
    if (m_verbose > 0)
        av_dump_format(m_output_format_context, 0, m_filename.c_str(), 1);

    // Open output file
    if (!(m_fmt->flags & AVFMT_NOFILE)) {
        ret = avio_open(&m_output_format_context->pb,
                        m_filename.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            clog << lock_ios()
                 << "ERROR: Could not open '" << m_filename << "': "
                 << AVerr2str(ret) << endl;
            Shutdown();
            return false;
        }
    }

    // Write header
    ret = avformat_write_header(m_output_format_context, &opt);
    if (ret < 0)
    {
        clog << lock_ios()
             << "ERROR: Could not open output file: %s\n"
             << AVerr2str(ret) << endl;
        Shutdown();
        return false;
    }

#if 1
    if (m_verbose > 1)
        clog << lock_ios()
             << "\n================== open_container end ==================\n";
#endif
    m_init_needed = false;
    return true;
}

/**
 * @brief Set audio parameters for encoding
 * @param num_channels Number of audio channels
 * @param is_lpcm Flag for LPCM format
 * @param bytes_per_sample Bytes per audio sample
 * @param sample_rate Audio sample rate
 * @param samples_per_frame Samples per audio frame
 * @param frame_size Size of audio frame
 * @return true on success, false on failure
 * @note Configures audio parameters for the audio IO handler
 */
bool OutputTS::setAudioParams(int num_channels, bool is_lpcm,
                              int bytes_per_sample, int sample_rate,
                              int samples_per_frame, int frame_size)
{
    if (m_audioIO == nullptr)
    {
        m_audioIO = new AudioIO([=](bool val) { this->DiscardImages(val); },
                                m_verbose);
        if (m_audioIO == nullptr)
        {
            clog << "Failed to create Audio handler" << endl;
            return false;
        }
    }
    m_no_audio = false;

    if (!m_audioIO->AddBuffer(num_channels, is_lpcm,
                              bytes_per_sample, sample_rate,
                              samples_per_frame, frame_size))
        return false;

    if (m_verbose > 2)
        clog << lock_ios()
             << "setAudioParams " << (is_lpcm ? "LPCM" : "Bitstream") << endl;

    return true;
}

/**
 * @brief Set video parameters for encoding
 * @param width Video width
 * @param height Video height
 * @param interlaced Flag for interlaced video
 * @param time_base Timebase for video
 * @param frame_duration Frame duration in microseconds
 * @param frame_rate Frame rate
 * @param is_hdr Flag for HDR video
 * @return true on success, false on failure
 * @note Configures video parameters and prepares for encoding
 */
bool OutputTS::setVideoParams(int width, int height, bool interlaced,
                              AVRational time_base, double frame_duration,
                              AVRational frame_rate, bool is_hdr)
{
    // Calculate frame wait time
    m_input_frame_wait_ms = frame_duration / 10000 * 2;

    // Clear queues before setting new parameters
    {
        unique_lock<mutex> lock(m_imagequeue_mutex);
        while (m_running.load() && !m_imagequeue.empty())
        {
            m_imagequeue_empty.wait_for(lock,
                         std::chrono::milliseconds(m_input_frame_wait_ms));
        }
    }

    {
        std::unique_lock<std::mutex> lock(m_videopool_mutex);
        while (m_running.load() && m_video_stream.frames_used != 0)
        {
            m_videopool_empty.wait_for(lock,
                         std::chrono::milliseconds(m_input_frame_wait_ms));
        }
    }

    // Update video parameters
    m_input_width = width;
    m_input_height = height;
    m_interlaced = interlaced;
    m_input_time_base = time_base;
    m_input_frame_duration = frame_duration;
    m_input_frame_rate = frame_rate;
    m_isHDR = is_hdr;
    m_frame_buffers = 15 +
                      (m_p010 || m_isHDR ? 20 : 0) +
                      std::exp(max(25 - m_quality, 1));

    double fps = static_cast<double>(frame_rate.num) / frame_rate.den;

    if (m_verbose > 0)
    {
        clog << lock_ios()
             << "Video: " << width << "x" << height
             << (m_interlaced ? 'i' : 'p') << fps
             << (m_isHDR ? " HDR" : "") << endl;
        if (m_verbose > 2)
            clog << lock_ios()
                 << "Video Params set\n";
    }

    open_video();
    m_init_needed = true;

    return true;
}

/**
 * @brief Add audio data to the output
 * @param buf Pointer to audio buffer
 * @param timestamp Timestamp for audio data
 * @return true on success, false on failure
 * @note Adds audio data to the audio IO handler for processing
 */
bool OutputTS::addAudio(AudioBuffer::AudioFrame *& buf, int64_t timestamp)
{
    return m_audioIO->Add(buf, timestamp);
}

/**
 * @brief Close the output container
 * @note Cleans up container resources
 */
void OutputTS::close_container(void)
{
    if (m_fmt && !(m_fmt->flags & AVFMT_NOFILE))
        avio_closep(&m_output_format_context->pb);

    avformat_free_context(m_output_format_context);
}

/**
 * @brief Close encoder for a stream
 * @param ost Output stream to close
 * @note Frees encoder resources
 */
void OutputTS::close_encoder(OutputStream* ost)
{
    if (!ost->enc)
        return;

    // Free hardware frames context
    av_buffer_unref(&ost->enc->hw_frames_ctx);
    ost->enc->hw_frames_ctx = nullptr;
    ost->hw_device = false;

    // Free temporary frame
    if (ost->tmp_frame /*  && ost->tmp_frame->data[0] */)
    {
        av_frame_free(&ost->tmp_frame);
        ost->tmp_frame = nullptr;
    }

    // Free resampler context
    if (ost->swr_ctx)
    {
        swr_free(&ost->swr_ctx);
        ost->swr_ctx = nullptr;
    }

    // Note: FFmpeg docs suggest not to free codec context multiple times
    // This is commented out to avoid double-free issues
#if 0
    avcodec_free_context(&ost->enc);
#endif
    ost->enc = nullptr;
}

/**
 * @brief Close output stream
 * @param ost Output stream to close
 * @note Frees stream resources
 */
void OutputTS::close_stream(OutputStream* ost)
{
    if (ost == nullptr)
        return;

    // Free hardware device context
    if (ost->hw_device)
    {
        av_buffer_unref(&ost->enc->hw_frames_ctx);
        ost->hw_device = false;
    }

    // Free resampler context
    if (ost->swr_ctx)
    {
        swr_free(&ost->swr_ctx);
        ost->swr_ctx = nullptr;
    }

    // Free codec context (commented to avoid double-free)
    if (ost->enc)
    {
#if 0
        avcodec_free_context(&ost->enc);
        ost->enc = nullptr;
#endif
    }

    // Note: FFmpeg docs suggest not to free stream context here
#if 0
    avformat_free_context(&ost->st);
    ost->st = nullptr;
#endif
}

/**
 * @brief Write a frame to output container
 * @param fmt_ctx Format context
 * @param codec_ctx Codec context
 * @param frame Frame to write
 * @param ost Output stream
 * @return true on success, false on failure
 * @note Encodes and writes frame to output container
 */
bool OutputTS::write_frame(AVFormatContext* fmt_ctx,
                           AVCodecContext* codec_ctx,
                           AVFrame* frame,
                           OutputStream* ost)
{
    int ret;
    AVPacket* pkt = ost->tmp_pkt;

    // Send frame to encoder
    ret = avcodec_send_frame(codec_ctx, frame);
    if (ret < 0)
    {
        if (m_verbose > 0)
        {
            clog << lock_ios()
                 << "WARNING: Failed sending a frame to the encoder: "
                 << AVerr2str(ret) << endl;
        }
        return false;
    }

    // Receive encoded packets
    while (ret >= 0)
    {
        ret = avcodec_receive_packet(codec_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        else if (ret < 0)
        {
            if (m_verbose > 0)
            {
                clog << lock_ios()
                     << "WARNING: Failed encoding a frame: AVerr2str(ret)"
                     << endl;
            }
            return false;
        }

        // Rescale timestamp values
        av_packet_rescale_ts(pkt, codec_ctx->time_base, ost->st->time_base);

        pkt->stream_index = ost->st->index;

        // Handle timestamp adjustments
        if (ost->prev_dts >= pkt->dts)
            pkt->dts = ost->prev_dts + 1;
        ost->prev_dts = pkt->dts;

        if (pkt->pts < pkt->dts)
            pkt->pts = pkt->dts;

        // Write packet to container
        ret = av_interleaved_write_frame(fmt_ctx, pkt);
        if (ret < 0)
        {
            if (m_verbose > 0)
            {
                clog << lock_ios()
                     << "WARNING: Failed to write packet: " << AVerr2str(ret)
                     << endl;
            }

            if (m_verbose > 1)
            {
                clog << lock_ios()
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
        ++ost->frames_written;
    }

    return ret == AVERROR_EOF ? false : true;
}

/**
 * @brief Get PCM audio frame for encoding
 * @param ost Output stream
 * @return Pointer to audio frame or nullptr on error
 * @note Retrieves PCM audio data from buffer and prepares it for encoding
 */
AVFrame* OutputTS::get_pcm_audio_frame(OutputStream* ost)
{
    AVFrame* frame = ost->tmp_frame;

    uint8_t* q = (uint8_t*)frame->data[0];

    int bytes = ost->enc->ch_layout.nb_channels *
                frame->nb_samples * m_audioIO->BytesPerSample();

    // Check if we have enough data
    if (m_audioIO->Size() < bytes)
    {
        if (m_verbose > 4)
            clog << lock_ios()
                 << "Not enough audio data.\n";
        this_thread::sleep_for(chrono::milliseconds(1));
        return nullptr;
    }

    // Read audio data
    if (m_audioIO->Read(q, bytes) <= 0)
        return nullptr;

    // Set frame properties
    ost->timestamp = frame->pts = m_audioIO->TimeStamp();
    ost->next_timestamp = ost->timestamp;

    ost->frame->pts = av_rescale_q(frame->pts, m_input_time_base,
                                   ost->enc->time_base);

    ost->next_pts = frame->pts + frame->nb_samples;

    return frame;
}

/**
 * @brief Write PCM audio frame to output
 * @param oc Output context
 * @param ost Output stream
 * @return true on success, false on failure
 * @note Encodes and writes PCM audio frame
 */
bool OutputTS::write_pcm_frame(AVFormatContext* oc, OutputStream* ost)
{
    AVCodecContext* enc_ctx = ost->enc;
    AVFrame* frame = get_pcm_audio_frame(ost);
    int dst_nb_samples = 0;
    int ret = 0;

    if (!frame)
        return false;

    // Calculate destination samples
    dst_nb_samples = av_rescale(swr_get_delay(ost->swr_ctx,
                                              enc_ctx->sample_rate)
                                + frame->nb_samples,
                                enc_ctx->sample_rate,
                                enc_ctx->sample_rate);
    av_assert0(dst_nb_samples == frame->nb_samples);

    // Make frame writable
    if (0 > av_frame_make_writable(ost->frame))
    {
        clog << "WARNING: write_pcm_frame: Failed to make frame writable"
             << endl;
        return false;
    }

    // Convert audio samples
    ret = swr_convert(ost->swr_ctx,
                      ost->frame->data, dst_nb_samples,
                      const_cast<const uint8_t** >(frame->data),
                      frame->nb_samples);
    if (ret < 0)
    {
        clog << "WARNING: write_pcm_frame: Error while converting" << endl;
        return false;
    }

    frame = ost->frame;
    frame->pts = av_rescale_q(m_audioIO->TimeStamp(),
                              m_input_time_base,
                              enc_ctx->time_base);

    ost->samples_count += dst_nb_samples;

    return write_frame(oc, enc_ctx, frame, ost);
}

/**
 * @brief Write bitstream audio frame to output
 * @param oc Output context
 * @param ost Output stream
 * @return true on success, false on failure
 * @note Writes bitstream audio frame directly to output
 */
bool OutputTS::write_bitstream_frame(AVFormatContext* oc, OutputStream* ost)
{
    AVPacket* pkt = m_audioIO->ReadSPDIF();

    if (pkt == nullptr)
    {
        if (m_verbose > 2)
            clog << "Failed to read pkt from S/PDIF" << endl;
        return false;
    }

    // Set timestamp and duration
    ost->timestamp = m_audioIO->TimeStamp();
    int64_t duration = av_rescale_q(pkt->duration,
                                    ost->st->time_base,
                                    m_input_time_base);

    ost->next_timestamp = ost->timestamp + duration;
    pkt->pts = av_rescale_q(ost->timestamp,
                            m_input_time_base,
                            ost->st->time_base);

    // Set packet properties
    pkt->dts = pkt->pts;
    pkt->stream_index = ost->st->index;

    // Write packet
    int ret = av_interleaved_write_frame(oc, pkt);
    /* pkt is now blank (av_interleaved_write_frame() takes ownership of
     * its contents and resets pkt), so that no unreferencing is necessary.
     * This would be different if one used av_write_frame(). */

    if (ret < 0)
    {
        clog << lock_ios()
             << "WARNING: Failed to write audio packet: " << AVerr2str(ret)
             << endl;
        return false;
    }

    return true;
}

/**
 * @brief Write audio frame to output
 * @param oc Output context
 * @param ost Output stream
 * @return true on success, false on failure
 * @note Dispatches audio frame writing based on format (PCM or bitstream)
 */
bool OutputTS::write_audio_frame(AVFormatContext* oc, OutputStream* ost)
{
    if (m_audioIO->Bitstream())
        return write_bitstream_frame(oc, ost);
    else
        return write_pcm_frame(oc, ost);
}

/**
 * @brief Allocate picture frame
 * @param pix_fmt Pixel format for frame
 * @param width Width of frame
 * @param height Height of frame
 * @return Pointer to allocated AVFrame or nullptr on error
 * @note Allocates memory for video frames
 */
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

    // Allocate frame buffer
    ret = av_frame_get_buffer(picture, 0);
    if (ret < 0)
    {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pix_fmt);
        clog << lock_ios()
             << "ERROR: Could not allocate " << desc->name
             << " video frame of " << width << "x" << height
             << " : " << AV_ts2str(ret) << endl;
        return nullptr;
    }

    return picture;
}

/**
 * @brief Open NVIDIA encoder
 * @param codec Video codec to use
 * @param ost Output stream
 * @param opt_arg Encoder options
 * @return true on success, false on failure
 * @note Configures NVIDIA NVENC encoder
 */
bool OutputTS::open_nvidia(const AVCodec* codec,
                           OutputStream* ost, AVDictionary* opt_arg)
{
    int ret;
    AVCodecContext* ctx = ost->enc;
    AVDictionary* opt = NULL;

    av_dict_copy(&opt, opt_arg, 0);

    // Set encoder options
    if (!m_preset.empty())
    {
        av_opt_set(ctx->priv_data, "preset", m_preset.c_str(), 0);
        if (m_verbose > 0)
            clog << lock_ios()
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

    ctx->gop_size = 180;
    if (av_opt_set(ctx->priv_data, "no-open-gop", "1",
                   AV_OPT_SEARCH_CHILDREN) < 0)
        if (m_verbose > 2)
            clog << "nvenc: Could not set no-open-gop option.\n";

    // Set pixel format
    if (m_isHDR || m_p010)
        ctx->pix_fmt = AV_PIX_FMT_P010LE;
    else
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    // Open codec
    ret = avcodec_open2(ctx, codec, &opt);
    av_dict_free(&opt);
    if (ret < 0)
    {
        clog << lock_ios()
             << "ERROR: Could not open video codec: "
             << AVerr2str(ret) << endl;
        Shutdown();
        return false;
    }

    // Allocate reusable frames
    ost->frames = new OutputStream::FramePool[ost->frames_total];
    for (int idx = 0; idx < ost->frames_total; ++idx)
    {
        ost->frames[idx].frame = alloc_picture(ctx->pix_fmt,
                                               ctx->width,
                                               ctx->height);
        if (!ost->frames[idx].frame)
        {
            clog << lock_ios()
                 << "ERROR: Could not allocate video frame" << endl;

            Shutdown();
            return false;
        }
    }

    ost->tmp_frame = NULL;

    return true;
}

/**
 * @brief Open VAAPI encoder
 * @param codec Video codec to use
 * @param ost Output stream
 * @param opt_arg Encoder options
 * @return true on success, false on failure
 * @note Configures VAAPI encoder
 */
bool OutputTS::open_vaapi(const AVCodec* codec,
                          OutputStream* ost, AVDictionary* opt_arg)
{
    int ret;
    AVCodecContext* ctx = ost->enc;
    AVDictionary* opt = nullptr;
    AVBufferRef* hw_frames_ref;
    AVHWFramesContext* frames_ctx = nullptr;

    av_dict_copy(&opt, opt_arg, 0);

    // Set encoder options
    av_opt_set(ctx->priv_data, "rc_mode", "ICQ", 0);
    av_opt_set_int(ctx->priv_data, "maxrate", 25000000, 0);
    av_opt_set_int(ctx->priv_data, "bufsize", 400000000, 0);
    av_opt_set_int(ctx->priv_data, "bf", 0, 0);
    av_opt_set_int(ctx->priv_data, "qp", 25, 0);

    // Create hardware device context
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
                clog << lock_ios()
                     << "ERROR: Failed to open VAPPI driver '"
                     << *Idriver << "'" << endl;
            else
                break;
        }
        if (Idriver == drivers.end())
        {
            clog << lock_ios()
                 << "ERROR: Failed to create a VAAPI device. Error code: "
                 << AVerr2str(ret) << endl;
            Shutdown();
            return false;
        }

        if (m_verbose > 0)
            clog << lock_ios()
                 << "Using VAAPI driver '" << *Idriver << "'\n";
    }

    // Create hardware frames context
    if (!(hw_frames_ref = av_hwframe_ctx_alloc(ost->hw_device_ctx)))
    {
        clog << lock_ios()
             << "ERROR: Failed to create VAAPI frame context." << endl;
        Shutdown();
        return false;
    }
    frames_ctx = reinterpret_cast<AVHWFramesContext* >(hw_frames_ref->data);

    // Set frame format
    if (m_isHDR || m_p010)
        frames_ctx->sw_format = AV_PIX_FMT_P010;
    else
        frames_ctx->sw_format = AV_PIX_FMT_NV12;

    frames_ctx->format    = AV_PIX_FMT_VAAPI;
    ost->enc->pix_fmt     = AV_PIX_FMT_VAAPI;

    frames_ctx->width     = m_input_width;
    frames_ctx->height    = m_input_height;
    frames_ctx->initial_pool_size = 20;

    // Initialize frames context
    if ((ret = av_hwframe_ctx_init(hw_frames_ref)) < 0)
    {
        clog << lock_ios()
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
        clog << lock_ios()
             << "ERROR: Failed to allocate hw frame buffer. "
             << "Error code: " << AVerr2str(ret) << endl;
        av_buffer_unref(&hw_frames_ref);
        Shutdown();
        return false;
    }
    av_buffer_unref(&hw_frames_ref);
    ost->hw_device = true;

    // Open codec
    if ((ret = avcodec_open2(ctx, codec, &opt)) < 0)
    {
        clog << lock_ios()
             << "ERROR: Cannot open VAAPI video encoder codec. Error code: "
             << AVerr2str(ret) << endl;
        Shutdown();
        return false;
    }

    // Allocate reusable frames
    ost->frames = new OutputStream::FramePool[ost->frames_total];
    for (int idx = 0; idx < ost->frames_total; ++idx)
    {
        ost->frames[idx].frame = alloc_picture(frames_ctx->sw_format,
                                               frames_ctx->width,
                                               frames_ctx->height);
        if (!ost->frames[idx].frame)
        {
            clog << lock_ios()
                 << "ERROR: Could not allocate QSV video frame" << endl;
            Shutdown();
            return false;
        }
    }

    return true;
}

/**
 * @brief Open QSV encoder
 * @param codec Video codec to use
 * @param ost Output stream
 * @param opt_arg Encoder options
 * @return true on success, false on failure
 * @note Configures Intel Quick Sync Video encoder
 */
bool OutputTS::open_qsv(const AVCodec* codec,
                        OutputStream* ost, AVDictionary* opt_arg)
{
    int    ret;

    AVDictionary* opt = nullptr;
    AVBufferRef* hw_frames_ref;
    AVHWFramesContext* frames_ctx = nullptr;

    av_dict_copy(&opt, opt_arg, 0);

    // Set encoder options
    av_opt_set(ost->enc->priv_data, "rc_mode", "ICQ", 0);
    ost->enc->global_quality = m_quality;

    if (m_video_codec_name != "av1_qsv")
    {
        if (!m_preset.empty())
        {
            av_opt_set(ost->enc->priv_data, "preset", m_preset.c_str(), 0);
            if (m_verbose > 0)
                clog << lock_ios()
                     << "Using preset " << m_preset << " for "
                     << m_video_codec_name << endl;
        }

        av_opt_set(ost->enc->priv_data, "scenario", "livestreaming", 0);

        if (m_look_ahead >= 0)
        {
            if (m_video_codec_name == "hevc_qsv")
                av_opt_set_int(ost->enc->priv_data, "look_ahead", 1, 0);
            av_opt_set_int(ost->enc->priv_data, "look_ahead_depth", m_look_ahead, 0);
        }
        av_opt_set_int(ost->enc->priv_data, "extra_hw_frames", m_look_ahead, 0);
        av_opt_set(ost->enc->priv_data, "skip_frame", "insert_dummy", 0);
    }

    av_opt_set_int(ost->enc->priv_data, "idr_interval", 0, 0);

    // Create hardware device context
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
            clog << lock_ios()
                 << "ERROR: Failed to open QSV on " << m_device << endl;
            return false;
        }

        if (m_verbose > 0)
            clog << lock_ios()
                 << "Using QSV\n";
    }

    // Create hardware frames context
    if (!(hw_frames_ref = av_hwframe_ctx_alloc(ost->hw_device_ctx)))
    {
        clog << lock_ios()
             << "ERROR: Failed to create QSV frame context." << endl;
        Shutdown();
        return false;
    }
    frames_ctx = reinterpret_cast<AVHWFramesContext* >(hw_frames_ref->data);

    // Set frame format
    if (m_isHDR || m_p010)
        frames_ctx->sw_format = AV_PIX_FMT_P010;
    else
        frames_ctx->sw_format = AV_PIX_FMT_NV12;

    frames_ctx->format    = AV_PIX_FMT_QSV;
    ost->enc->pix_fmt     = AV_PIX_FMT_QSV;

    frames_ctx->width     = m_input_width;
    frames_ctx->height    = m_input_height;
    frames_ctx->initial_pool_size = 20;

    // Initialize frames context
    if ((ret = av_hwframe_ctx_init(hw_frames_ref)) < 0)
    {
        clog << lock_ios()
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
        clog << lock_ios()
             << "ERROR: Failed to allocate hw frame buffer. "
             << "Error code: " << AVerr2str(ret) << endl;
        av_buffer_unref(&hw_frames_ref);
        Shutdown();
        return false;
    }
    av_buffer_unref(&hw_frames_ref);
    ost->hw_device = true;

    // Open codec
    if ((ret = avcodec_open2(ost->enc, codec, &opt)) < 0)
    {
        clog << lock_ios()
             << "ERROR: Cannot open QSV video encoder codec. Error code: "
             << AVerr2str(ret) << endl;
        Shutdown();
        return false;
    }

    // Allocate reusable frames
    ost->frames = new OutputStream::FramePool[ost->frames_total];
    for (int idx = 0; idx < ost->frames_total; ++idx)
    {
        ost->frames[idx].frame = alloc_picture(frames_ctx->sw_format,
                                               frames_ctx->width,
                                               frames_ctx->height);
        if (!ost->frames[idx].frame)
        {
            clog << lock_ios()
                 << "ERROR: Could not allocate QSV video frame" << endl;
            Shutdown();
            return false;
        }
    }

    return true;
}

/**
 * @brief Encode video frame with NVIDIA encoder
 * @return true on success, false on failure
 * @note Encodes video frame using NVENC
 */
bool OutputTS::nv_encode(void)
{
    OutputStream* ost   = &m_video_stream;

    ost->next_pts = m_video_stream.timestamp + 1;

    return write_frame(m_output_format_context,
                       ost->enc, ost->frame, ost);
}

/**
 * @brief Encode video frame with QSV/VAAPI encoder
 * @return true on success, false on failure
 * @note Encodes video frame using QSV or VAAPI
 */
bool OutputTS::qsv_vaapi_encode(void)
{
    static AVFrame* hw_frame = nullptr;
    int    ret;

    OutputStream* ost   = &m_video_stream;

    if (!(hw_frame = av_frame_alloc()))
    {
        clog << lock_ios()
             << "ERROR: Failed to allocate hw frame.";
        Shutdown();
        return false;
    }

    // Get hardware frame buffer
    if ((ret = av_hwframe_get_buffer(ost->enc->hw_frames_ctx,
                                     hw_frame, 0)) < 0)
    {
        clog << lock_ios()
             << "ERROR: Failed to get hw buffer: "
             << AV_ts2str(ret) << endl;
        Shutdown();
        return false;
    }

    // Transfer frame data to hardware
    if (!hw_frame->hw_frames_ctx)
    {
        clog << lock_ios()
             << "ERROR: Failed to allocate hw frame CTX." << endl;
        Shutdown();
        return false;
    }

    if ((ret = av_hwframe_transfer_data(hw_frame,
                                        ost->frame, 0)) < 0)
    {
        clog << lock_ios()
             << "ERROR: failed transferring frame data to surface: "
             << AV_ts2str(ret) << endl;
        Shutdown();
        return false;
    }

    hw_frame->pts = ost->frame->pts;
    ost->next_pts = m_video_stream.timestamp + 1;

    ret = write_frame(m_output_format_context,
                      ost->enc, hw_frame, ost);
    av_frame_free(&hw_frame);

    return ret;
}

/**
 * @brief Main muxing thread function
 * @note Handles audio and video frame processing and muxing
 */
void OutputTS::mux(void)
{
    int glitch_cnt = 0;

    while (m_running.load() == true)
    {
        // Handle audio codec changes
        if (m_audioIO && m_audioIO->CodecChanged())
        {
            clog << lock_ios() << " Audio changing: closing audio encoder\n";
            if (!open_audio())
            {
                clog << lock_ios()
                     << "ERROR: Failed to create audio stream" << endl;
                Shutdown();
                break;
            }
            m_init_needed = true;
        }

        // Initialize container if needed
        if (m_init_needed)
        {
            if (m_video_stream.enc &&
                (!m_audioIO || m_audio_stream.enc != nullptr))
            {
                if (!open_container())
                {
                    Shutdown();
                    break;
                }
                m_video_stream.timestamp = -1;
                m_video_stream.next_timestamp = -1;
                if (m_audioIO)
                    m_audio_stream.next_timestamp = -1;
                else
                    m_audio_stream.next_timestamp = -2;
            }
            else
            {
                string why;
                if (m_video_stream.enc == nullptr)
                    why = " video";
                if (m_audioIO && m_audio_stream.enc == nullptr)
                {
                    if (!why.empty())
                        why += " &";
                    why += " audio";
                }
                if (m_verbose > 4)
                {
                    clog << lock_ios() << "WARNING: New TS needed but"
                         << why << " encoder is not ready." << endl;
                }
            }
        }

        // Write audio frames
        if (m_audio_stream.enc)
        {
#if 0
            clog << lock_ios()
                 << "Write: video " << m_video_stream.next_pts << "\n"
                 << "  audio [" << setw(2) << m_audioIO->BufId()
                 << "] " << m_audio_stream.next_pts << "\n";
#endif
            if (!write_audio_frame(m_output_format_context,
                                   &m_audio_stream))
            {
                if (++glitch_cnt % 100 == 0)
                {
                    if (m_video_stream.frames_written > 900)
                        clog << "Damaged: Audio glitch. Resetting.\n";
                    else if (m_verbose > 0)
                        clog << "Warning: Audio glitch. Resetting.\n";
                    m_audioIO->Reset("OutputTS::mux");
                    f_reset();
                    ClearVideoPool();
                    ClearImageQueue();
                }
                this_thread::sleep_for(chrono::milliseconds(5));
                continue;
            }
            glitch_cnt = 0;
        }

        // Clear queues when needed
        if (m_audio_stream.next_timestamp == -1)
        {
            ClearVideoPool();
            ClearImageQueue();
        }

        // Process video frames
        while (!m_audio_stream.enc ||
               m_video_stream.timestamp <= m_audio_stream.next_timestamp)
        {
            {
                std::unique_lock<std::mutex> lock(m_videopool_mutex);

                if (!m_video_stream.enc || m_video_stream.frames_used == 0)
                {
                    m_videopool_empty.notify_one();
                    m_videopool_ready.wait_for(lock,
                        std::chrono::milliseconds(m_input_frame_wait_ms));
                    break;
                }
            }

            if (++m_video_stream.frames_idx_out == m_video_stream.frames_total)
                m_video_stream.frames_idx_out = 0;

            m_video_stream.frame = m_video_stream
                       .frames[m_video_stream.frames_idx_out].frame;
            m_video_stream.timestamp = m_video_stream
                       .frames[m_video_stream.frames_idx_out].timestamp;

            // Encode with appropriate encoder
            if (m_encoderType == EncoderType::NV)
                nv_encode();
            else if (m_encoderType == EncoderType::QSV ||
                     m_encoderType == EncoderType::VAAPI)
                qsv_vaapi_encode();
            else
            {
                clog << lock_ios() << "ERROR: Unknown encoderType." << endl;
                Shutdown();
                return;
            }

            {
                std::unique_lock<std::mutex> lock(m_videopool_mutex);
                --m_video_stream.frames_used;
            }
            m_videopool_avail.notify_one();
        }
    }
}

/**
 * @brief Clear video frame pool
 * @note Resets video frame pool state
 */
void OutputTS::ClearVideoPool(void)
{
    const unique_lock<mutex> lock(m_videopool_mutex);

    m_video_stream.frames_idx_in  = -1;
    m_video_stream.frames_idx_out = -1;
    m_video_stream.frames_used = 0;
    m_videopool_cnt = 0;
}

/**
 * @brief Clear image queue and return buffers
 * @note Returns all image buffers to their original source
 */
void OutputTS::ClearImageQueue(void)
{
    const unique_lock<mutex> lock(m_imagequeue_mutex);
    imageque_t::iterator Iq;
    for (Iq = m_imagequeue.begin(); Iq != m_imagequeue.end(); ++Iq)
        f_image_buffer_available((*Iq).image, (*Iq).pEco);
    m_imagequeue.clear();
}

/**
 * @brief Set discard images flag
 * @param val Flag to set discard images
 * @note Clears queues when discarding images
 */
void OutputTS::DiscardImages(bool val)
{
    m_discard_images = val;
    if (val)
    {
        ClearVideoPool();
        ClearImageQueue();
    }
}

/**
 * @brief Copy frames from image queue to frame pool
 * @note Copies frames from image queue to frame pool for encoding
 */
void OutputTS::copy_to_frame(void)
{
    uint8_t* pImage;
    void*    pEco;
    int      image_size;
    int64_t  timestamp;
    int64_t  prev_ts = -1;
    int64_t  prev_pts = -1;
    int      prev_idx = -1;

    while (m_running.load() == true)
    {
        {
            unique_lock<mutex> lock(m_videopool_mutex);
            if (m_video_stream.frames_used >= m_video_stream.frames_total)
            {
                if (m_verbose > 3)
                    clog << lock_ios() << "Frame pool is full "
                         << m_video_stream.frames_used << "/"
                         << m_video_stream.frames_total
                         << " (" << m_videopool_cnt << " processed)."
                         << " Waiting for available slot.\n";
                m_videopool_avail.wait_for(lock,
                           std::chrono::milliseconds(m_input_frame_wait_ms));
                continue;
            }
        }

        {
            std::unique_lock<std::mutex> lock_i(m_imagequeue_mutex);

            if (m_imagequeue.empty())
            {
                m_imagequeue_empty.notify_one();
                m_imagequeue_ready.wait_for(lock_i,
                       std::chrono::milliseconds(m_input_frame_wait_ms));
                continue;
            }

            pImage     = m_imagequeue.front().image;
            pEco       = m_imagequeue.front().pEco;
            image_size = m_imagequeue.front().image_size;
            timestamp  = m_imagequeue.front().timestamp;

            m_imagequeue.pop_front();
        }

        ++m_videopool_cnt;
        if (++m_video_stream.frames_idx_in == m_video_stream.frames_total)
            m_video_stream.frames_idx_in = 0;

        m_video_stream.frames[m_video_stream.frames_idx_in]
                      .timestamp = timestamp;
        AVFrame* frm = m_video_stream
                       .frames[m_video_stream.frames_idx_in].frame;

        frm->pts = av_rescale_q(timestamp,
                                m_input_time_base,
                                m_video_stream.enc->time_base);

        if (frm->pts <= prev_pts)
        {
            if (m_verbose > 0)
            {
                clog << lock_ios()
                     << "WARNING: copy_frame: scaled pts did not increase: "
                     << "[" << prev_idx << "] -> ["
                     << m_video_stream.frames_idx_in << "] / "
                     << m_video_stream.frames_used << "/"
                     << m_video_stream.frames_total << "; "
                     << prev_pts << " -> " << frm->pts << ". TS "
                     << prev_ts << " -> " << timestamp
                     << " diff:" << timestamp - prev_ts
                     << " expected: " << m_input_frame_duration
                     << endl;
            }
//            ++frm->pts;
        }
        prev_pts = frm->pts;
        prev_ts = timestamp;
        prev_idx = m_video_stream.frames_idx_in;

        // Copy frame data based on pixel format
        if (m_video_stream.enc->pix_fmt == AV_PIX_FMT_YUV420P)
        {
            // YUV 4:2:0
            memcpy(frm->data[0], pImage, image_size);
            memcpy(frm->data[1], pImage + image_size, image_size / 4);
            memcpy(frm->data[2], pImage + image_size * 5 / 4,
                   image_size / 4);
        }
        else
        {
            memcpy(frm->data[0], pImage, image_size);
            memcpy(frm->data[1], pImage + image_size,
                   image_size / 2);
        }
        f_image_buffer_available(pImage, pEco);

        {
            std::unique_lock<std::mutex> lock(m_videopool_mutex);
            ++m_video_stream.frames_used;
        }
        m_videopool_ready.notify_one();
    }
}

/**
 * @brief Add video frame to processing queue
 * @param pImage Pointer to image buffer
 * @param pEco Pointer to ECO context
 * @param imageSize Size of image data
 * @param timestamp Timestamp for frame
 * @return true always
 * @note Adds video frame to queue for processing
 */
bool OutputTS::AddVideoFrame(uint8_t* pImage, void* pEco,
                             int imageSize, int64_t timestamp)
{
    const std::unique_lock<std::mutex> lock(m_imagequeue_mutex);

    if (m_discard_images)
        f_image_buffer_available(pImage, pEco);
    else
    {
        m_imagequeue.push_back(imagepkt_t{timestamp, pImage, pEco, imageSize});
        m_imagequeue_ready.notify_one();
    }

    return true;
}
