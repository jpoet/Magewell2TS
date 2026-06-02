/*
 * Copyright (c) 2022-2026 John Patrick Poet
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
 * @date 2022-2026
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
#include <algorithm>
#include <array>

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
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_qsv.h>  // AVQSVFramesContext
#include <vpl/mfxstructures.h>        // "MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET"
}

#if 0
#define BURST_HEADER_SIZE 0x4
#define SYNCWORD1 0xF872
#define SYNCWORD2 0x4E1F
#endif

#include "OutputTS.h"
#include "BitstreamAudioParser.h"

using namespace std;

#include <string>
#include <sstream>
#include <format>
#include <cstdint>

std::string AV_ts2str(int64_t ts)
{
    char astr[AV_ERROR_MAX_STRING_SIZE] = { 0 }; // Explicit stack buffer boundary
    av_ts_make_string(astr, ts);
    return std::string(astr);
}

std::string AV_ts2timestr(int64_t ts, AVRational* tb)
{
    if (!tb || tb->num == 0) return "0.00";
    std::ostringstream os;
    os << av_q2d(*tb) * ts;
    return os.str();
}

std::string AVerr2str(int code)
{
    char astr[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_make_error_string(astr, AV_ERROR_MAX_STRING_SIZE, code);
    return std::string(astr);
}

std::string av_dump_format_string(AVFormatContext* ctx)
{
    if (!ctx)
        return std::format("No format context provided\n");

    std::string result;
    for (unsigned int idx = 0; idx < ctx->nb_streams; ++idx)
    {
        AVStream* stream = ctx->streams[idx];
        AVCodecParameters* par = stream->codecpar;

        result += std::format("\tStream {}: ", idx);
        result += std::format("{}: {}, ",
                              av_get_media_type_string(par->codec_type),
                              avcodec_get_name(par->codec_id));

        if (par->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            std::string pix_name =
                par->format >= 0 ?
                    av_get_pix_fmt_name((AVPixelFormat)par->format) : "unknown";
            double tbn = 0.0;

            if (stream->time_base.num != 0)
            {
                tbn = static_cast<double>(stream->time_base.den) /
                      static_cast<double>(stream->time_base.num);
            }


            // Safe checking to handle fallback color layout strings safely
            const char* cs_name = av_color_space_name(par->color_space);
            std::string space_name = cs_name ? cs_name : "unknown";

            result += std::format("{}({}), {}x{}p {:.2f} tbn\n",
                                  pix_name,
                                  space_name,
                                  par->width, par->height,
                                  tbn);
        }
        else if (par->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            char layout_name[256] = { 0 };
            av_channel_layout_describe(&par->ch_layout, layout_name,
                                       sizeof(layout_name));

            // Calculate a true dynamic bitrate for uncompressed LPCM
            // targets safely
            int64_t true_bitrate = par->bit_rate;
            if (true_bitrate <= 0 && par->sample_rate > 0)
            {
                // Bitrate = sample_rate * channels * bits_per_sample
                int bps = av_get_bits_per_sample(par->codec_id);
                if (bps <= 0) bps = 16; // Safe standard baseline fallback guess
                true_bitrate = static_cast<int64_t>(par->sample_rate) *
                               par->ch_layout.nb_channels * bps;
            }

            // Added missing trailing newline to match video block
            // layout aesthetics
            result += std::format("{} Hz, {}, {} kb/s\n", par->sample_rate,
                                  layout_name, true_bitrate / 1000);
        }
    }

    return result;
}

OutputTS::OutputTS(int verbose_level, const string & video_codec_name,
                   const string & preset, int quality, int look_ahead,
                   bool p010, bool isEco, const string & device,
                   int extra_hw_frames, float gop_secs,
                   ShutdownCallback shutdown,
                   MagCallback image_buffer_avail)
    : m_verbose(verbose_level)
    , m_video_codec_name(video_codec_name)
    , m_device("/dev/dri/" + device)
    , m_preset(preset)
    , m_quality(quality)
    , m_look_ahead(look_ahead)
    , m_gop_secs(gop_secs)
    , m_p010(p010)
    , m_isEco(isEco)
    , m_extra_hw_frames(extra_hw_frames)
    , f_shutdown(shutdown)
    , f_image_buffer_available(image_buffer_avail)
{
    m_log = spdlog::get("app_logger");
    if (!m_log)
    {
        std::cerr << "OutputTS Error: Logger 'app_logger' not found!" << std::endl;
        return;
    }

    av_log_set_level(AV_LOG_QUIET);

    // Initialize atomic runtime state machine flags
    m_running.store(true);

    // Determine encoder type based on codec name strings
    if (m_video_codec_name.find("qsv") != string::npos) {
        m_encoderType = EncoderType::QSV;
    } else if (m_video_codec_name.find("vaapi") != string::npos) {
        m_encoderType = EncoderType::VAAPI;
    } else if (m_video_codec_name.find("nvenc") != string::npos || m_video_codec_name.find("nv") != string::npos) {
        m_encoderType = EncoderType::NV;
    } else {
        m_encoderType = EncoderType::UNKNOWN;
        m_log->critical("Codec '{}' not supported inside streaming pipeline.", m_video_codec_name);
        throw std::runtime_error("Unsupported video codec choice.");
    }

    // Allocate HDR metadata tracking blocks
    m_display_primaries = av_mastering_display_metadata_alloc();
    m_content_light     = av_content_light_metadata_alloc(nullptr);

    initialize_bitstream_parser();

    // Start up threads last
    m_mux_thread = std::thread(&OutputTS::mux, this);
    pthread_setname_np(m_mux_thread.native_handle(), "mux");

    m_video_thread = std::thread(&OutputTS::process_video, this);
    pthread_setname_np(m_video_thread.native_handle(), "video");

    m_audio_thread = std::thread(&OutputTS::process_audio, this);
    pthread_setname_np(m_audio_thread.native_handle(), "audio");
}

OutputTS::~OutputTS(void)
{
    if (m_verbose > 2)
        m_log->info("Cleaning Transport Stream");

    // Signal all execution loops to cease operations
    m_running.store(false);
    Shutdown();

    // Shut down thread-safe queues to break any active condition variable locks
    while (!m_videoPktQ.IsEmpty())
    {
        auto entry = m_videoPktQ.PopValue();
    }
    m_videoPktQ.Shutdown();

    while (!m_audioPktQ.IsEmpty())
    {
        auto entry = m_audioPktQ.PopValue();
    }
    m_audioPktQ.Shutdown();

    if (m_verbose > 2)
        m_log->info("Waiting for threads to exit.");
    // Wait for working thread scopes to exit cleanly
    if (m_video_thread.joinable())
        m_video_thread.join();

    if (m_mux_thread.joinable())
        m_mux_thread.join();

    m_log->info("Releasing core resource footprints...");

    // Clear out raw image frame queues and release their hardware
    // handles back to Magewell
    {
        std::lock_guard<std::mutex> lock(m_imageQ_mutex);
        while (!m_imageQ.empty()) {
            auto& img = m_imageQ.front();
            f_image_buffer_available(img.pImage, img.pEco);
            m_imageQ.pop_front();
        }
    }

    // =========================================================================
    // DEALLOCATE CORE STREAM ENCODERS & FILES
    // =========================================================================
    close_container();

    // Close the individual active video and audio encoder contexts safely
    close_encoder(&m_videoStream);
    close_encoder(&m_audioStream);

    // Free internal HDR metadata memory layout maps
    av_freep(&m_display_primaries);
    av_freep(&m_content_light);

    // Release GPU hardware
    if (m_videoStream.hw_device_ctx)
    {
        av_buffer_unref(&m_videoStream.hw_device_ctx);
        m_videoStream.hw_device_ctx = nullptr;
    }

    if (m_verbose > 2)
        m_log->info("Transport Stream shutdown");
}



string OutputTS::ColorSpaceDesc(void) const
{
    switch (m_color_space)
    {
        case AVCOL_SPC_BT470BG:
          return "YUV601";
        case AVCOL_SPC_BT2020_NCL:
          return "YUV2020";
        case AVCOL_SPC_BT709:
          return "YUV709";
        default:
          return "Unknown";
    }
}

/**
 * @brief Shutdown the output TS handler
 * @note Stops all threads and cleans up resources
 */
void OutputTS::Shutdown(void)
{
    if (m_running.exchange(false))
        f_shutdown();
}

void OutputTS::log_packet(string where, const AVFormatContext* fmt_ctx,
                       const AVPacket* pkt)
{
    AVRational* time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

    m_log->info("{}[{}] pts: {} pts_time: {} dts: {} dts_time: {} duration: {} "
                "duration_time: {}",
                where, pkt->stream_index, pkt->pts,
                AV_ts2timestr(pkt->pts, time_base),
                AV_ts2str(pkt->dts),
                AV_ts2timestr(pkt->dts, time_base),
                AV_ts2str(pkt->duration),
                AV_ts2timestr(pkt->duration, time_base));
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
 * @brief Open video encoder for output
 * @param pParams pointer to the updated hardware video specifications
 * @return true on success, false on failure
 */
bool OutputTS::open_video(const VideoParams& params)
{
    // Thread-safe cleanup of the previous encoder generation context
    close_encoder(&m_videoStream);

    // Perform basic hardware parameter safety validation
    if (params.width <= 0 || params.height <= 0)
    {
        m_log->error("Invalid dimensions received: {}x{}",
                     params.width, params.height);
        return false;
    }

    if (m_verbose > 1)
    {
        m_log->info("Opening {} encoder (Target Specs: {}x{}p{})",
                    m_video_codec_name, params.width, params.height,
                    static_cast<double>(params.frame_rate.num) /
                    static_cast<double>(params.frame_rate.den));
    }

    // =========================================================================
    // CODEC ALLOCATION & CONTEXT SETUP
    // =========================================================================
    const AVCodec* video_codec =
        avcodec_find_encoder_by_name(m_video_codec_name.c_str());
    if (!video_codec)
    {
        m_log->error("Could not locate requested video encoder: '{}'",
                     m_video_codec_name);
        return false;
    }

    m_videoStream.enc = avcodec_alloc_context3(video_codec);
    if (!m_videoStream.enc)
    {
        m_log->error("Failed to allocate unique video encoding context.");
        return false;
    }

    // Assign hardware metrics straight to the active encoder context
    m_videoStream.enc->codec_id  = video_codec->id;
    m_videoStream.enc->width     = params.width;
    m_videoStream.enc->height    = params.height;
    m_sw_pix_fmt                 = params.pix_fmt;

    // Set the encoder's internal processing time_base to match frame
    // intervals
    m_videoStream.enc->time_base =
        AVRational{params.frame_rate.den, params.frame_rate.num};

    // Calculate dynamic GOP size threshold boundaries based on target seconds
    if (m_gop_secs > 0)
    {
        m_videoStream.enc->gop_size =
            static_cast<int>((static_cast<double>(params.frame_rate.num) /
                              static_cast<double>(params.frame_rate.den)) *
                             static_cast<double>(m_gop_secs) + 0.5);
        if (m_verbose > 2)
            m_log->info("GOP size {} frames.",
                        m_videoStream.enc->gop_size);
    }

    // Apply color mastering tags dynamically from the payload flag
    if (params.is_HDR)
    {
        m_log->info("Configuring encoder context for HDR streaming.");
        m_videoStream.enc->color_range = AVCOL_RANGE_JPEG;
    }
    else
    {
        m_videoStream.enc->color_range = AVCOL_RANGE_UNSPECIFIED;
    }

    // Assign color spaces using existing baseline tracking variables
    m_videoStream.enc->color_primaries = m_color_primaries;
    m_videoStream.enc->color_trc       = m_color_trc;
    m_videoStream.enc->colorspace      = m_color_space;

    // Evaluate hardware thread slicing permissions
    if (m_videoStream.enc->codec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
    {
        m_videoStream.enc->thread_type = FF_THREAD_SLICE;
        if (m_verbose > 1)
            m_log->info(" Video Encoder Strategy = THREAD SLICE");
    }
    else if (m_videoStream.enc->codec->capabilities &
             AV_CODEC_CAP_FRAME_THREADS)
    {
        m_videoStream.enc->thread_type = FF_THREAD_FRAME;
        if (m_verbose > 1)
            m_log->info(" Video Encoder Strategy = THREAD FRAME");
    }

    // Cache the original un-scaled input timebase from the struct parameters.
    // This allows process_video to calculate frame PTS scaling accurately later:
    // frm->pts = av_rescale_q(timestamp, m_input_time_base, m_videoStream.enc->time_base);
    m_input_time_base = params.time_base;

    // Update the class dimensions so other subsystems remain synchronized
    m_input_width  = params.width;
    m_input_height = params.height;

    // Setup GPU
    AVDictionary* local_opt = nullptr;
    bool open_success = false;

    switch (m_encoderType)
    {
        case EncoderType::QSV:
          open_success = open_qsv(video_codec, &local_opt);
          break;
        case EncoderType::VAAPI:
          open_success = open_vaapi(video_codec, &local_opt);
          break;
        case EncoderType::NV:
          open_success = open_nvidia(video_codec, &local_opt);
          break;
        default:
          m_log->error("Unsupported hardware encoder architecture selected.");
          break;
    }

    // Always free the initialization dictionary context to avoid leaking RAM
    if (local_opt != nullptr) {
        av_dict_free(&local_opt);
    }

    if (!open_success) {
        return false;
    }

    return true;
}

bool OutputTS::open_audio_encoder(CodecParamsPtr& codecpar)
{
    close_encoder(&m_audioStream);

    // LPCM -> AC3 ENCODE
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AC3);

    if (!codec)
    {
        m_log->error("AC3 encoder not found");
        return false;
    }

    m_audioStream.enc = avcodec_alloc_context3(codec);

    if (!m_audioStream.enc)
    {
        m_log->error("Failed allocating AC3 context");

        return false;
    }

    AVCodecContext* enc = m_audioStream.enc;

    // AC3 standard sample rate
    enc->sample_rate = 48000;
    // Internal encoder format
    enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
    // AC3 timing domain
    enc->time_base = { 1, enc->sample_rate };

    av_channel_layout_default(&enc->ch_layout, codecpar->ch_layout.nb_channels);

    switch (codecpar->ch_layout.nb_channels)
    {
        case 2:
          enc->bit_rate = 192000;
          break;

        case 6:
          enc->bit_rate = 448000;
          break;

        case 8:
          enc->bit_rate = 640000;
          break;

        default:
          enc->bit_rate = 448000;
          break;
    }

    //
    // AC3 fixed frame size
    //
    enc->frame_size = 1536;

    int ret = avcodec_open2(enc, codec, nullptr);

    if (ret < 0)
    {
        m_log->error("avcodec_open2(audio) failed: {}",
                     AVerr2str(ret));
        close_encoder(&m_audioStream);
        return false;
    }

    ret = avcodec_parameters_from_context(codecpar.get(), enc);
    if (ret < 0)
    {
        m_log->error("Failed copying codec params");
        close_encoder(&m_audioStream);
        return false;
    }

    m_audioStream.samples_count = 0;

    m_log->info("Opened AC3 encoder: {}ch {}Hz {}bps",
                codecpar->ch_layout.nb_channels,
                enc->sample_rate,
                enc->bit_rate);

    return true;
}

void OutputTS::close_encoder(OutputStream* ost)
{
    if (!ost)
        return;

    if (m_verbose > 1)
    {
        if (ost->enc &&
            ost->enc->codec &&
            ost->enc->codec->long_name)
        {
            m_log->info(
                "Closing {} encoder.",
                ost->enc->codec->long_name);
        }
    }

    //
    // Encoder-owned HW references
    //
    if (ost->enc &&
        ost->enc->hw_frames_ctx)
    {
        av_buffer_unref(
            &ost->enc->hw_frames_ctx);

        ost->enc->hw_frames_ctx =
            nullptr;
    }

    //
    // Shared HW frame pools
    //
    if (ost->hw_frames_ctx)
    {
        av_buffer_unref(
            &ost->hw_frames_ctx);

        ost->hw_frames_ctx =
            nullptr;
    }

    //
    // Audio specific clean up
    //
    if (ost == &m_audioStream)
    {
        if (m_audio_fifo)
        {
            av_audio_fifo_free(m_audio_fifo);
            m_audio_fifo = nullptr;
        }
    }

    //
    // Destroy codec context
    //
    if (ost->enc)
    {
        avcodec_free_context(
            &ost->enc);

        ost->enc = nullptr;
    }

    //
    // Reset runtime state
    //
    ost->st             = nullptr;
    ost->hw_device      = false;
    ost->samples_count  = 0;
}

// Open Transport Stream container
bool OutputTS::open_container(AVCodecParameters* video_param,
                              AVRational video_time_base,
                              AVCodecParameters* audio_param,
                              AVRational audio_time_base)
{
    close_container();

    if (m_verbose > 1)
        m_log->info("================ open_container begin ================");

    // Allocate the fresh transport stream envelope targeting stdout
    // via the "pipe:" protocol
    int ret = avformat_alloc_output_context2(&m_formatContext,
                                             nullptr, "mpegts", "pipe:1");
    if (ret < 0 || m_formatContext == nullptr)
    {
        m_log->error("Failed to allocate stdout output context: {}",
                     AVerr2str(ret));
        return false;
    }

    // Bypass internal I/O buffering so frames hit stdout pipe
    // with zero latency
    m_formatContext->flags |= AVFMT_FLAG_FLUSH_PACKETS;

    // TRACK 0: Video track initialization
    AVStream* v_st = avformat_new_stream(m_formatContext, nullptr);
    if (v_st == nullptr)
        return false;

    avcodec_parameters_copy(v_st->codecpar, video_param);
    v_st->time_base = video_time_base;

    // TRACK 1: Audio track initialization (Conditional)
    if (!m_no_audio)
    {
        AVStream* a_st = avformat_new_stream(m_formatContext, nullptr);
        if (a_st == nullptr) return false;

        avcodec_parameters_copy(a_st->codecpar, audio_param);
        a_st->time_base = audio_time_base;
    }

    // Physical stream commit
    // Bind FFmpeg's I/O handle back to the active stdout stream descriptor
    ret = avio_open(&m_formatContext->pb, "pipe:1", AVIO_FLAG_WRITE);
    if (ret < 0)
    {
        m_log->error("Failed to bind physical stdout descriptor pipe: {}",
                     AVerr2str(ret));
        return false;
    }

    if (m_verbose > 0)
        m_log->info(av_dump_format_string(m_formatContext));

    AVDictionary* muxer_opts = nullptr;
    // Force the muxer to insert a PCR timestamp at minimum every 20ms to 40ms
    av_dict_set(&muxer_opts, "pcr_period", "20", 0);
    // Ensure strict transport stream compliance layout
    av_dict_set(&muxer_opts, "muxrate", "0", 0); // VBR mode, but forces clock packets

    // Commit headers to stream pipeline
    ret = avformat_write_header(m_formatContext, &muxer_opts);

    av_dict_free(&muxer_opts);
    if (ret < 0)
    {
        m_log->error("Error writing new sequential stream header: {}",
                     AVerr2str(ret));
        return false;
    }

    if (m_verbose > 1)
        m_log->info("================ open_container end ================");

    return true;
}

void OutputTS::close_container(void)
{
    if (m_formatContext != nullptr)
    {
        // If headers were written successfully, write the final
        // MPEG-TS trailer block.  This transmits crucial trailing PSI
        // data tables down the stdout pipe.
        av_write_trailer(m_formatContext);

        if (m_formatContext->pb != nullptr)
        {
            // Flush any remaining buffered bytes out to the Linux
            // kernel pipe.
            avio_flush(m_formatContext->pb);

            // CRITICAL FOR STDOUT: Explicitly free the private AVIO
            // context buffer without closing the underlying system
            // File Descriptor 1 (stdout).
            av_freep(&m_formatContext->pb->buffer);
            av_free(m_formatContext->pb);

            // Set the pointer to nullptr so FFmpeg's structural
            // clean-up code knows it doesn't need to try and close a
            // file handle.
            m_formatContext->pb = nullptr;
        }

        // Safely free the root context, structural streams, and
        // parameters.
        avformat_free_context(m_formatContext);
        m_formatContext = nullptr;
    }
}

// std::shared_ptr<AVPacket>
bool OutputTS::queue_packets(OutputStream* ost,
                             MediaQueue<Packet>& pktQ,
                             bool is_flushing)
{
    if (!ost || !ost->enc)
        return false;

    AVCodecContext* codec_ctx = ost->enc;

    int ret = 0;

    while (true)
    {
        PacketPtr pkt = make_packet();

        if (!pkt)
        {
            m_log->error("Failed allocating AVPacket.");
            return false;
        }

        ret = avcodec_receive_packet(codec_ctx, pkt.get());

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            break;
        }

        if (ret < 0)
        {
            m_log->warn("Failed encoding frame: {}",
                        AVerr2str(ret));
            return false;
        }

        // Some encoders omit DTS
        if (pkt->dts == AV_NOPTS_VALUE)
            pkt->dts = pkt->pts;

        // ??????????????????
        if (ost->st)
            pkt->stream_index = ost->st->index;

        Packet qp
            {
                .is_marker    = false,
                .dts          = pkt->dts,
                .time_base    = codec_ctx->time_base,
                .pkt          = std::move(pkt),
                .codec_par    = nullptr
            };

        pktQ.Push(std::move(qp));

        m_pktQ_ready.notify_one();
    }

    return is_flushing
        ? (ret == AVERROR_EOF)
        : true;
}

bool OutputTS::encode_frame(OutputStream* ost, AVFrame* frame,
                            MediaQueue<Packet>& pktQ)
{
    if (!ost || !ost->enc) return false;

    // Send frame to encoder
    int ret = avcodec_send_frame(ost->enc, frame);
    av_frame_unref(frame);
    if (ret < 0)
    {
        m_log->warn("Failed sending a frame to the encoder: {}", AVerr2str(ret));
        return false;
    }

    // Only unref if the encoder accepted ownership.
    // If ret == EAGAIN, the frame was rejected and must be preserved.
    if (frame && ret != AVERROR(EAGAIN))
        av_frame_unref(frame);

    return queue_packets(ost, pktQ, false);
}

bool OutputTS::flush_packets(OutputStream* ost, MediaQueue<Packet>& pktQ)
{
    if (!ost || !ost->enc) return true;
    if (!avcodec_is_open(ost->enc)) return true;

    // Enter draining mode by passing nullptr
    int ret = avcodec_send_frame(ost->enc, nullptr);
    if (ret < 0 && ret != AVERROR_EOF)
    {
        if (m_verbose > 0) {
            m_log->error("Error entering encoder drain mode: {}", AVerr2str(ret));
        }
        return false;
    }

    return queue_packets(ost, pktQ, true);
}

bool OutputTS::open_nvidia(const AVCodec* codec, AVDictionary** opt_arg)
{
    int ret;
    AVDictionary* opt = nullptr;

    if (opt_arg && *opt_arg) {
        av_dict_copy(&opt, *opt_arg, 0);
    }

    // Configure nVidia nvenc private encoder compression options
    av_dict_set(&opt, "rc", "constqp", 0);
    m_videoStream.enc->global_quality = m_quality;

    if (!m_preset.empty())
    {
        // nvenc uses presets like 'p1' to 'p7' (fastest to highest quality)
        av_dict_set(&opt, "preset", m_preset.c_str(), 0);
        if (m_verbose > 0) {
            m_log->info("Nvidia Engine: Applied preset '{}' for {}",
                        m_preset, m_video_codec_name);
        }
    }

    // Configure real-time, low-latency streaming pipeline behavior
    av_dict_set(&opt, "delay", "0", 0);
    av_dict_set(&opt, "forced-idr", "1", 0);
    av_dict_set(&opt, "zerolatency", "1", 0);

    // Apply lookahead parameter blocks if supported by the hardware model
    if (m_look_ahead > 0)
    {
        av_dict_set_int(&opt, "rc-lookahead", m_look_ahead, 0);
        // "no-scenecut" optimizes multi-threaded pipeline performance during fast cuts
        av_dict_set_int(&opt, "no-scenecut", 1, 0);
    }

    // =========================================================================
    // SAFE MULTI-THREADED DEVICE ACCELERATION ACQUISITION
    // =========================================================================
    if (m_videoStream.hw_device_ctx == nullptr)
    {
        // Bind the NVENC execution environment to the physical GPU card
        // m_device contains the path or index identifier (e.g., "cuda" or "0")
        ret = av_hwdevice_ctx_create(&m_videoStream.hw_device_ctx,
                                     AV_HWDEVICE_TYPE_CUDA,
                                     m_device.c_str(), nullptr, 0);
        if (ret < 0 || m_videoStream.hw_device_ctx == nullptr)
        {
            m_log->error("Failed to acquire persistent Nvidia CUDA Device "
                         "Context on '{}': {}", m_device, AVerr2str(ret));
            if (opt) av_dict_free(&opt);
            return false;
        }

        if (m_verbose > 0) {
            m_log->info("nVidia CUDA hardware runtime engine "
                        "successfully bound.");
        }
    }

    // Assign input surface format codes
    // Because nvenc ingests raw host memory arrays or CUDA textures directly,
    // we assign its input pixel format explicitly. NVENC natively converts
    // the layout into its optimized hardware space behind the scenes.
    m_videoStream.enc->pix_fmt = AV_PIX_FMT_NV12;

    // Pass the core hardware accelerator handle reference into the
    // encoder block context
    m_videoStream.enc->hw_device_ctx =
        av_buffer_ref(m_videoStream.hw_device_ctx);

    // Codec initialization & encoder activation
    // Commit the local options dictionary parameters during encoder activation
    ret = avcodec_open2(m_videoStream.enc, codec, &opt);

    // Clean local tracking structures safely to avoid leaking RAM
    if (opt) {
        av_dict_free(&opt);
    }

    if (ret < 0)
    {
        m_log->critical("Fatal Error: Nvidia NVENC codec activation "
                        "rejected: {}", AVerr2str(ret));
        return false;
    }

    m_log->info("Nvidia NVENC pipeline fully active "
                "at resolution {}x{}",
                m_videoStream.enc->width, m_videoStream.enc->height);
    return true;
}

bool OutputTS::open_vaapi(const AVCodec* codec, AVDictionary** opt_arg)
{
    int ret;
    AVDictionary* opt = nullptr;

    if (opt_arg && *opt_arg) {
        av_dict_copy(&opt, *opt_arg, 0);
    }

    // Configure VAAPI private encoder compression options
    // Most VAAPI encoders utilize standard global_quality directly or
    // expose private option strings.
    m_videoStream.enc->global_quality = m_quality;

    if (!m_preset.empty())
    {
        // VAAPI encoders accept string presets depending on driver capabilities
        // Common examples: "fast", "medium", "slow"
        av_opt_set(m_videoStream.enc->priv_data, "preset", m_preset.c_str(), 0);
        if (m_verbose > 0)
        {
            m_log->info("VAAPI Engine: Applied preset '{}' for {}",
                        m_preset, m_video_codec_name);
        }
    }

    // Low-overhead real-time live streaming behavior controls
    av_opt_set(m_videoStream.enc->priv_data, "async_depth", "4", 0);

    // VAAPI drivers natively compute internal frame pipelines
    // differently than QSV.  Most entry levels do not support deep
    // multi-frame lookahead mechanics inline, but we pad the surface
    // allocation window anyway to stay safe against spikes.
    int surface_count_padding = m_extra_hw_frames + 4;
    if (m_look_ahead > 0)
        surface_count_padding += m_look_ahead;

    // =========================================================================
    // SAFE MULTI-THREADED DEVICE ACCELERATION ACQUISITION
    // =========================================================================
    if (m_videoStream.hw_device_ctx == nullptr)
    {
        // Thread-safe configuration adjustments using compliant
        // system APIs.  For standard Linux setups, the default driver
        // can be iHD (Intel) or radeonsi (AMD).
        setenv("LIBVA_MESSAGING_LEVEL", "0", 1);

        // For VAAPI, m_device typically specifies a card render node
        // path (e.g., "/dev/dri/renderD128")
        ret = av_hwdevice_ctx_create(&m_videoStream.hw_device_ctx,
                                     AV_HWDEVICE_TYPE_VAAPI,
                                     m_device.c_str(), nullptr, 0);
        if (ret < 0 || m_videoStream.hw_device_ctx == nullptr)
        {
            m_log->error("Failed to acquire persistent VAAPI Device "
                         "Context on path '{}': {}", m_device, AVerr2str(ret));
            if (opt)
                av_dict_free(&opt);
            return false;
        }

        if (m_verbose > 0)
        {
            m_log->info("VAAPI hardware runtime engine successfully bound.");
        }
    }

    if (opt)
        av_dict_free(&opt);

    // Bind encoder parameters format to target execution surface expectations
    m_videoStream.enc->pix_fmt = AV_PIX_FMT_VAAPI;

    // Context aware surface pool allocation rebuild
    // Explicitly destroy the old generation frame context layout map
    // before spawning a new one
    if (m_videoStream.hw_frames_ctx != nullptr)
    {
        av_buffer_unref(&m_videoStream.hw_frames_ctx);
        m_videoStream.hw_frames_ctx = nullptr;
    }

    // Allocate a fresh frames context matching the updated resolution
    // parameters
    m_videoStream.hw_frames_ctx =
        av_hwframe_ctx_alloc(m_videoStream.hw_device_ctx);
    if (m_videoStream.hw_frames_ctx == nullptr)
    {
        m_log->error("VAAPI Pool Builder: Failed to allocate hardware "
                     "frames memory block.");
        return false;
    }

    AVHWFramesContext* frames_ctx =
        reinterpret_cast<AVHWFramesContext*>(m_videoStream.hw_frames_ctx->data);

    // VAAPI drivers are strict regarding surface dimensions matching
    // the context exactly
    frames_ctx->width             = m_videoStream.enc->width;
    frames_ctx->height            = m_videoStream.enc->height;
    frames_ctx->format            = AV_PIX_FMT_VAAPI;
    frames_ctx->sw_format         = m_sw_pix_fmt;

    // Set an explicit static initial pool size broad enough to absorb
    // execution spikes comfortably
    frames_ctx->initial_pool_size = surface_count_padding + 16;

    ret = av_hwframe_ctx_init(m_videoStream.hw_frames_ctx);
    if (ret < 0)
    {
        m_log->error("VAAPI Kernel Driver rejected fresh format "
                     "surface pool request: {}", AVerr2str(ret));
        av_buffer_unref(&m_videoStream.hw_frames_ctx);
        m_videoStream.hw_frames_ctx = nullptr;
        return false;
    }

    // Copy the newly validated hardware surface reference directly to
    // the encoder object context
    m_videoStream.enc->hw_frames_ctx =
        av_buffer_ref(m_videoStream.hw_frames_ctx);

    // Kernel initialization codec activation
    ret = avcodec_open2(m_videoStream.enc, codec, nullptr);
    if (ret < 0)
    {
        m_log->critical("Fatal Error: VAAPI codec activation rejected "
                        "by system kernel: {}", AVerr2str(ret));
        return false;
    }

    m_log->info("VAAPI pipeline fully active at "
                "hardware resolution {}x{}",
                frames_ctx->width, frames_ctx->height);
    return true;
}

bool OutputTS::open_qsv(const AVCodec* codec, AVDictionary** opt_arg)
{
    int ret;
    AVDictionary* opt = nullptr;

    if (opt_arg && *opt_arg)
        av_dict_copy(&opt, *opt_arg, 0);

    // Configure Intel QSV private encoder compression options
    av_opt_set(m_videoStream.enc->priv_data, "rc_mode", "ICQ", 0);
    m_videoStream.enc->global_quality = m_quality;

    if (m_video_codec_name != "av1_qsv")
    {
        if (!m_preset.empty())
        {
            av_opt_set(m_videoStream.enc->priv_data, "preset",
                       m_preset.c_str(), 0);
            if (m_verbose > 0)
                m_log->info("QSV Engine: Applied preset '{}' for {}",
                            m_preset, m_video_codec_name);
        }

        av_opt_set(m_videoStream.enc->priv_data, "scenario",
                   "livestreaming", 0);

        if (m_look_ahead > 0 && av_opt_find(m_videoStream.enc->priv_data,
                                            "lookahead", nullptr, 0,
                                            AV_OPT_SEARCH_CHILDREN))
        {
            // Standard dynamic naming convention across modern FFmpeg branches
            av_opt_set_int(m_videoStream.enc->priv_data, "lookahead", 1, 0);
            av_opt_set_int(m_videoStream.enc->priv_data, "lookahead_depth",
                           m_look_ahead, 0);
        }

        av_opt_set(m_videoStream.enc->priv_data, "skip_frame",
                   "insert_dummy", 0);
        av_opt_set(m_videoStream.enc->priv_data, "async_depth", "4", 0);
    }

    // Match surface allocation requirements to the active QSV
    // lookahead lookback parameters
    av_opt_set_int(m_videoStream.enc->priv_data, "extra_hw_frames",
                   m_extra_hw_frames + m_look_ahead + 4, 0);

    if (m_gop_secs > 0)
    {
        if (av_opt_set_int(m_videoStream.enc->priv_data,
                           "forced_idr", 1, 0) < 0)
            m_log->warn("qsv: failed to set forced_idr");
        if (av_opt_set_int(m_videoStream.enc->priv_data,
                           "idr_interval", 1, 0) < 0)
            m_log->warn("qsv: failed to set idr_interval");
    }

    if (m_videoStream.hw_device_ctx == nullptr)
    {
        // Use modern Intel media driver, overwrite=0 protects user env
        setenv("LIBVA_DRIVER_NAME", "iHD", 0);
        setenv("LIBVA_MESSAGING_LEVEL", "0", 1);

        av_dict_set(&opt, "child_device", m_device.c_str(), 0);

        // Establish the single, long-lived hardware backend pipeline
        // instance [1]
        ret = av_hwdevice_ctx_create(&m_videoStream.hw_device_ctx,
                                     AV_HWDEVICE_TYPE_QSV,
                                     m_device.c_str(), opt, 0);
        if (ret < 0 || m_videoStream.hw_device_ctx == nullptr)
        {
            m_log->error("Failed to acquire persistent Intel QSV "
                         "Device Context on device '{}': {}",
                         m_device, AVerr2str(ret));
            if (opt)
                av_dict_free(&opt);
            return false;
        }

        if (m_verbose > 0)
            m_log->info("Intel QSV hardware runtime engine successfully bound");
    }

    // Clean local working option structures
    if (opt)
        av_dict_free(&opt);

    // Bind encoder parameters format to target execution plane
    // mapping requirements
    m_videoStream.enc->pix_fmt = AV_PIX_FMT_QSV;

    // Explicitly destroy the old generation frame context layout map
    // before spawning a new one [1]
    if (m_videoStream.hw_frames_ctx != nullptr)
    {
        av_buffer_unref(&m_videoStream.hw_frames_ctx);
        m_videoStream.hw_frames_ctx = nullptr;
    }

    // Create a fresh frames context matching the updated resolution parameters
    m_videoStream.hw_frames_ctx =
        av_hwframe_ctx_alloc(m_videoStream.hw_device_ctx);
    if (m_videoStream.hw_frames_ctx == nullptr)
    {
        m_log->error("QSV Pool Builder: Failed to allocate hardware "
                     "frames memory block.");
        return false;
    }

    AVHWFramesContext* frames_ctx =
        reinterpret_cast<AVHWFramesContext*>(m_videoStream.hw_frames_ctx->data);

    // Intel Media Driver require 16 or 32-byte surface boundary alignments [1]
    #define INTEL_ALIGN(x) (((x) + 31) & ~31)
    frames_ctx->width             = INTEL_ALIGN(m_videoStream.enc->width);
    frames_ctx->height            = INTEL_ALIGN(m_videoStream.enc->height);
    frames_ctx->format            = AV_PIX_FMT_QSV;
    frames_ctx->sw_format         = m_sw_pix_fmt;

    // Set a explicit static initial pool size broad enough to hold
    // the QSV lookahead depth window [1]
    frames_ctx->initial_pool_size = m_extra_hw_frames + m_look_ahead + 16;

    // Apply QSV-specific flags if driver layer exposes them
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 0, 0)
    // Use frames_ctx directly as AVHWFramesContext*
    if (frames_ctx->hwctx)
    {
        AVQSVFramesContext* qsv_hwctx =
            reinterpret_cast<AVQSVFramesContext*>(frames_ctx->hwctx);
        // Safely assign the target GPU memory optimization
        qsv_hwctx->frame_type = MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET;
    }
#endif

    ret = av_hwframe_ctx_init(m_videoStream.hw_frames_ctx);
    if (ret < 0)
    {
        m_log->error("Intel Media Driver rejected fresh format surface "
                     "pool request: {}", AVerr2str(ret));
        av_buffer_unref(&m_videoStream.hw_frames_ctx);
        m_videoStream.hw_frames_ctx = nullptr;
        return false;
    }

    // Copy the newly validated hardware surface reference directly to
    // the encoder object context [1]
    m_videoStream.enc->hw_frames_ctx =
        av_buffer_ref(m_videoStream.hw_frames_ctx);


    // Intel kernel initialization codec activation
    ret = avcodec_open2(m_videoStream.enc, codec, nullptr);
    if (ret < 0)
    {
        m_log->critical("Fatal Error: Intel QSV codec activation rejected "
                        "by system kernel: {}", AVerr2str(ret));
        return false;
    }

    m_log->info("Intel QSV pipeline fully active at "
                "hardware resolution {}x{}",
                frames_ctx->width, frames_ctx->height);
    return true;
}

bool OutputTS::stream_changed(const AVCodecParameters* current_params,
                              const AVCodecParameters* new_params) const
{
    // If either pointer is null, we can't compare safely; force a
    // fresh reopen/init.
    if (!current_params || !new_params)
        return true;

    // Check for Codec Profile or Type modifications
    if (current_params->codec_type != new_params->codec_type ||
        current_params->codec_id   != new_params->codec_id)
        return true;

    // Audio-Specific Hard Boundaries
    if (current_params->codec_type == AVMEDIA_TYPE_AUDIO)
    {
        if (current_params->sample_rate != new_params->sample_rate)
            return true;

        // Channel configuration
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100)
        if (av_channel_layout_compare(&current_params->ch_layout,
                                      &new_params->ch_layout) != 0)
            return true;
#else
        if (current_params->channels != new_params->channels)
            return true;
#endif
    }

    // Video changes (Resolution, Aspect Ratio, Color Space) return false.
    // They are handled by the QSV encoder context natively without recycling.
    return false;
}

void OutputTS::mux(void)
{
    CodecParamsPtr audio_params;
    CodecParamsPtr video_params;

    AVRational audio_time_base { 0, 1 };
    AVRational video_time_base { 0, 1 };

    int64_t prev_video_dts { AV_NOPTS_VALUE };
    int64_t prev_audio_dts { AV_NOPTS_VALUE };

    // MPEG clock domain for comparison
    bool update_container = false;

    while (m_running.load())
    {
        std::optional<Packet> outPkt;
        PacketPtr pkt;

        if (m_videoPktQ.IsEmpty() || (!m_no_audio && m_audioPktQ.IsEmpty()))
        {
            std::unique_lock<std::mutex> lock(m_pktQ_mutex);
            m_pktQ_ready.wait_for(lock,
                            std::chrono::milliseconds(m_input_frame_wait_ms));

            continue;
        }

        // VIDEO ONLY MODE
        if (m_no_audio)
        {
            outPkt = m_videoPktQ.PopValue();

            if (outPkt->is_marker)
            {
                video_params = std::move(outPkt->codec_par);
                video_time_base  = outPkt->time_base;
                update_container = true;
            }
            else
            {
                pkt = std::move(outPkt->pkt);
                pkt->stream_index = VIDEO_STREAM_ID;

                av_packet_rescale_ts(pkt.get(),
                                     outPkt->time_base,
                                     m_formatContext
                                     ->streams[VIDEO_STREAM_ID]
                                     ->time_base);

                //
                // Monotonic DTS
                //
                if (prev_video_dts != AV_NOPTS_VALUE &&
                    pkt->dts <= prev_video_dts)
                {
                    int64_t fixed = prev_video_dts + 1;

                    m_log->warn("Fixing video DTS regression {} -> {}",
                                pkt->dts, fixed);
                    pkt->dts = fixed;
                }

                prev_video_dts = pkt->dts;
            }
        }
        else
        {
            //
            // Compare streams in common tb
            //
            int64_t audio_dts =
                av_rescale_q(m_audioPktQ.PeekDts(),
                             m_audioPktQ.PeekTimebase(),
                             TimeBase::MPEG_TS);

            int64_t video_dts =
                av_rescale_q(m_videoPktQ.PeekDts(),
                             m_videoPktQ.PeekTimebase(),
                             TimeBase::MPEG_TS);

#if 0
            m_log->info("audio_dts {} {} {} video_dts",
                        audio_dts, audio_dts < video_dts ? "<" : ">",
                        video_dts);
#endif
            //
            // AUDIO NEXT
            //
            if (audio_dts < video_dts)
            {
                outPkt = m_audioPktQ.PopValue();

                if (outPkt->is_marker)
                {
                    audio_params = std::move(outPkt->codec_par);
                    audio_time_base = outPkt->time_base;

                    if (m_videoPktQ.PeekMarker())
                    {
                        outPkt          = m_videoPktQ.PopValue();
                        video_params = std::move(outPkt->codec_par);
                        video_time_base = outPkt->time_base;
                    }

                    update_container = true;
                }
                else
                {
                    pkt = std::move(outPkt->pkt);

                    pkt->stream_index = AUDIO_STREAM_ID;

                    av_packet_rescale_ts(pkt.get(),
                                         outPkt->time_base,
                                         m_formatContext
                                         ->streams[AUDIO_STREAM_ID]
                                         ->time_base);

                    //
                    // Audio monotonicity
                    //
                    if (prev_audio_dts != AV_NOPTS_VALUE &&
                        pkt->dts <= prev_audio_dts)
                    {
                        int64_t fixed = prev_audio_dts + 1;

                        m_log->warn("Fixing audio DTS regression {} -> {}",
                                    pkt->dts, fixed);
                        pkt->dts = fixed;
                    }

                    prev_audio_dts = pkt->dts;
                }
            }
            //
            // VIDEO NEXT
            //
            else
            {
                outPkt = m_videoPktQ.PopValue();

                if (outPkt->is_marker)
                {
                    video_params = std::move(outPkt->codec_par);
                    video_time_base = outPkt->time_base;

                    if (m_audioPktQ.PeekMarker())
                    {
                        outPkt          = m_audioPktQ.PopValue();
                        audio_params = std::move(outPkt->codec_par);
                        audio_time_base = outPkt->time_base;
                    }

                    update_container = true;
                }
                else
                {
                    pkt = std::move(outPkt->pkt);

                    pkt->stream_index = VIDEO_STREAM_ID;

                    av_packet_rescale_ts(pkt.get(),
                                         outPkt->time_base,
                                         m_formatContext
                                         ->streams[VIDEO_STREAM_ID]
                                         ->time_base);

                    if (pkt->duration <= 0)
                    {
                        pkt->duration = av_rescale_q(1,
                                                     outPkt->time_base,
                                                     m_formatContext
                                                     ->streams[VIDEO_STREAM_ID]
                                                     ->time_base);
                    }

                    if (prev_video_dts != AV_NOPTS_VALUE &&
                        pkt->dts <= prev_video_dts)
                    {
                        int64_t fixed = prev_video_dts + 1;

                        m_log->warn("Fixing video DTS regression {} -> {}",
                                    pkt->dts, fixed);
                        pkt->dts = fixed;
                    }

                    prev_video_dts = pkt->dts;
                }
            }
        }

        //
        // Dynamic container reopen
        //
        if (update_container)
        {
            if (!m_formatContext ||
                stream_changed(m_formatContext
                               ->streams[VIDEO_STREAM_ID]
                               ->codecpar, video_params.get()) ||
                (!m_no_audio && stream_changed(m_formatContext
                                               ->streams[AUDIO_STREAM_ID]
                                               ->codecpar,
                                               audio_params.get())))
            {
                open_container(video_params.get(),
                               video_time_base,
                               audio_params.get(),
                               audio_time_base);
            }

            update_container = false;

            continue;
        }

        if (!outPkt || !pkt)
            continue;

#if 0
        //
        // Ensure pts >= dts
        //
        if (pkt->pts < pkt->dts)
            pkt->pts = pkt->dts;
#endif

#if 0
        m_log->info("MUX: [{}] pts:{} dts:{} duration:{} size:{} flags:{}",
                    pkt->stream_index, pkt->pts, pkt->dts,
                    pkt->duration, pkt->size, pkt->flags);
#endif
        int ret = av_write_frame(m_formatContext, pkt.get());

        if (ret == AVERROR(EINVAL))
        {
            m_log->critical("Mux rejected packet! PTS={} DTS={} DUR={}",
                            pkt->pts, pkt->dts, pkt->duration);
        }
        else if (ret < 0)
        {
            m_log->error("av_write_frame failed: {}", AVerr2str(ret));
        }
    }
}


void OutputTS::process_video(void)
{
    while (m_running.load())
    {
        VideoImage image;

        //
        // Wait for next image
        //
        {
            std::unique_lock<std::mutex> lock(m_imageQ_mutex);

            m_imageQ_ready.wait_for(lock,
                                    std::chrono::milliseconds(100), [this]()
            {
                return !m_running.load() || !m_imageQ.empty();
            });

            if (!m_running.load())
                break;

            if (m_imageQ.empty())
                continue;

            image = std::move(m_imageQ.front());
            m_imageQ.pop_front();
        }

        //
        // Encoder reconfiguration request
        //
        if (image.pParams.has_value())
        {
            const auto& params = *image.pParams;

            m_log->info("Video parameter change detected: {}x{}p{:.2f}",
                        params.width,
                        params.height,
                        static_cast<double>(params.frame_rate.num) /
                        static_cast<double>(params.frame_rate.den));
#if 1
            m_log->info("process_video(): input={} hw={} size={}x{}",
                        av_get_pix_fmt_name(m_sw_pix_fmt),
                        av_get_pix_fmt_name
                        (static_cast<AVPixelFormat>(params.pix_fmt)),
                        params.width,
                        params.height);
#endif

            //
            // Flush encoder completely
            //
            flush_packets(&m_videoStream, m_videoPktQ);


#if 0 // Is this really necessary?
            // Allow QSV async surfaces time to retire.
            //
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
#endif

            //
            // Open replacement encoder
            //
            if (!open_video(params))
            {
                m_log->critical("Failed to reopen video encoder.");

                Shutdown();
                break;
            }

            //
            // Signal mux thread that codec/container
            // re-open is required.
            //
            Packet marker;
            marker.is_marker = true;
            marker.codec_par = make_codec_params();

            if (!marker.codec_par)
            {
                m_log->critical("Failed to allocate codec parameters.");
                Shutdown();
                break;
            }

            avcodec_parameters_from_context(marker.codec_par.get(),
                                            m_videoStream.enc);

            marker.time_base = m_videoStream.enc->time_base;

            m_videoPktQ.Push(std::move(marker));
        }

        //
        // No encoder active yet
        //
        if (!m_videoStream.enc ||
            !m_videoStream.hw_frames_ctx)
        {
            m_log->warn("Dropping frame because encoder "
                        "is not initialized.");

            if (image.pImage)
            {
                f_image_buffer_available(image.pImage,
                                         image.pEco);
            }

            continue;
        }


        if (m_sw_pix_fmt != AV_PIX_FMT_NV12 &&
            m_sw_pix_fmt != AV_PIX_FMT_P010LE)
        {
            m_log->error("Unsupported input pixel format: {}",
                         av_get_pix_fmt_name(m_sw_pix_fmt));

            f_image_buffer_available(image.pImage,
                                     image.pEco);

            continue;
        }

        //
        // Allocate fresh HW frame
        //
        AVFrame* hw_frame = av_frame_alloc();

        if (!hw_frame)
        {
            m_log->critical("Failed to allocate hw_frame.");

            Shutdown();
            break;
        }

        int ret =
            av_hwframe_get_buffer(m_videoStream.hw_frames_ctx,
                                  hw_frame,
                                  0);

        if (ret < 0)
        {
            m_log->error("av_hwframe_get_buffer failed: {}",
                         AVerr2str(ret));

            av_frame_free(&hw_frame);

            f_image_buffer_available(image.pImage,
                                     image.pEco);

            continue;
        }

        //
        // Map HW frame to CPU-visible memory
        //
        AVFrame* mapped = av_frame_alloc();

        if (!mapped)
        {
            av_frame_free(&hw_frame);

            f_image_buffer_available(image.pImage,
                                     image.pEco);

            Shutdown();
            break;
        }

        ret = av_hwframe_map(mapped,
                             hw_frame,
                             AV_HWFRAME_MAP_WRITE);

        if (ret < 0)
        {
            m_log->error("av_hwframe_map failed: {}",
                         AVerr2str(ret));

            av_frame_free(&mapped);
            av_frame_free(&hw_frame);

            f_image_buffer_available(image.pImage,
                                     image.pEco);

            continue;
        }

        av_frame_make_writable(mapped);

        //
        // Build source plane layout
        //
        uint8_t* src_data[4] = { nullptr };
        int src_linesize[4] = { 0 };

        ret = av_image_fill_arrays(src_data,
                                   src_linesize,
                                   image.pImage,
                                   m_sw_pix_fmt,
                                   m_input_width,
                                   m_input_height,
                                   1);

        if (ret < 0)
        {
            m_log->error("av_image_fill_arrays failed: {}",
                         AVerr2str(ret));

            av_frame_free(&mapped);
            av_frame_free(&hw_frame);

            f_image_buffer_available(image.pImage,
                                     image.pEco);

            continue;
        }

        //
        // Copy CPU image into mapped QSV surface
        //
        av_image_copy(mapped->data,
                      mapped->linesize,
                      const_cast<const uint8_t**>(src_data),
                      src_linesize,
                      m_sw_pix_fmt,
                      m_input_width,
                      m_input_height);

        //
        // Release Magewell buffer IMMEDIATELY
        //
        f_image_buffer_available(image.pImage,
                                 image.pEco);

        //
        // Assign timing
        //
        hw_frame->pts =
            av_rescale_q(image.timestamp,
                         m_input_time_base,
                         m_videoStream.enc->time_base);

        //
        // Submit frame to encoder
        //
        if (!encode_frame(&m_videoStream,
                          hw_frame,
                          m_videoPktQ))
        {
            m_log->error("Video encode_frame failed.");

            av_frame_free(&mapped);
            av_frame_free(&hw_frame);

            continue;
        }

        //
        // IMPORTANT:
        // Release local references immediately.
        //
        av_frame_free(&mapped);
        av_frame_free(&hw_frame);
    }

    //
    // Final encoder flush
    //
    if (m_videoStream.enc)
    {
        flush_packets(&m_videoStream,
                      m_videoPktQ);
    }

    m_log->info("process_video thread exited.");
}

void OutputTS::AddVideoFrame(VideoImage&& image)
{
    const std::unique_lock<std::mutex> lock(m_imageQ_mutex);

    m_imageQ.push_back(std::move(image));
    m_imageQ_ready.notify_one();
}


/*
 * THREAD ENTRY
 */
void OutputTS::process_audio(void)
{
    while (m_running.load())
    {
        AudioSamples audio;

        {
            std::unique_lock<std::mutex> lock(m_audioQ_mutex);

            m_audioQ_ready.wait_for(lock,
                                    std::chrono::milliseconds
                                    (m_input_frame_wait_ms), [this]()
                    {
                        return !m_running.load() || !m_audioQ.empty();
                    });

            if (!m_running.load())
                continue;

            if (m_audioQ.empty())
                continue;

            audio = std::move(m_audioQ.front());

            m_audioQ.pop_front();

        }

        if (audio.pParams.has_value())
        {
            if (audio.pParams->is_lpcm)
            {
                m_bitstream = false;
                if (!configure_lpcm(audio))
                {
                    Shutdown();
                    break;
                }
            }
            else
            {
                m_bitstream = true;
                m_bitstream_parser->flush();
            }
        }

        if (m_bitstream)
            process_bitstream(std::move(audio));
        else
            process_lpcm(std::move(audio));
    }

    flush_audio_pipeline();

    m_log->info(
        "process_audio thread exited.");
}

/*
 *  LPCM -> AC3
 */
bool OutputTS::configure_lpcm(const AudioSamples& audio)
{
    m_bitstream = false;

    // Flush previous encoder
    if (m_audioStream.enc)
    {
        flush_packets(&m_audioStream,
                      m_audioPktQ);
    }

    m_audio_params = *audio.pParams;

    Packet marker;
    marker.is_marker = true;
    marker.codec_par = make_codec_params();
    if (!marker.codec_par)
    {
        m_log->critical("Failed to allocate codec parameters.");
        Shutdown();
        return false;
    }
    marker.codec_par->ch_layout.nb_channels = m_audio_params.num_channels;

    // Open new stream
    if (!open_audio_encoder(marker.codec_par))
    {
        m_log->critical("Failed opening audio stream.");
        return false;
    }

    m_audio_total_samples = av_rescale_q(audio.timestamp,
                                         TimeBase::Magewell,
                                         TimeBase::AUDIO48);

    // Create FIFO for LPCM encode path
    constexpr int fifo_size = 1536 * 16;
    m_audio_fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP,
                                       m_audio_params.num_channels,
                                       fifo_size);
    if (!m_audio_fifo)
    {
        m_log->critical("Failed allocating AVAudioFifo.");
        return false;
    }

    m_audioPktQ.Push(std::move(marker));
    m_pktQ_ready.notify_one();

    return true;
}

void OutputTS::process_lpcm(const AudioSamples& audio)
{
    constexpr int AC3_FRAME_SAMPLES = 1536;

    const int  channels      = m_audio_params.num_channels;
    const bool is_24bit      = m_audio_params.bits_per_sample > 16;
    const int  input_samples = m_samples_per_frame;

    //
    // Temporary planar float buffer
    //
    std::vector<float> planar(input_samples * channels);

    float* planes[8] {};

    for (int ch = 0; ch < channels; ++ch)
    {
        planes[ch] = planar.data() + (input_samples * ch);
    }

    //
    // ========================================================
    // FAST 16-BIT PATH
    // ========================================================
    //
    if (!is_24bit)
    {
        const int16_t* src =
            reinterpret_cast<const int16_t*>(audio.data.data());

        constexpr float scale = 1.0f / 32768.0f;

        for (int s = 0; s < input_samples; ++s)
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                planes[ch][s] = static_cast<float>
                           (src[s * channels + ch]) * scale;
            }
        }
    }
    //
    // ========================================================
    // 24-BIT PATH
    // ========================================================
    //
    else
    {
        const int32_t* src =
            reinterpret_cast<const int32_t*>(audio.data.data());

        constexpr float scale = 1.0f / 8388608.0f;

        for (int s = 0; s < input_samples; ++s)
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                int32_t v = src[s * channels + ch];

                //
                // Sign extend packed 24-bit
                //
                v <<= 8;
                v >>= 8;

                planes[ch][s] = static_cast<float>(v) * scale;
            }
        }
    }

    //
    // ========================================================
    // PUSH INTO FIFO
    // ========================================================
    //
    if (av_audio_fifo_write(m_audio_fifo,
                            reinterpret_cast<void**>(planes),
                            input_samples) < input_samples)
    {
        m_log->error("Failed writing audio FIFO.");
        return;
    }

    //
    // ========================================================
    // ENCODE WHILE ENOUGH SAMPLES EXIST
    // ========================================================
    //
    while (av_audio_fifo_size(m_audio_fifo) >= AC3_FRAME_SAMPLES)
    {
        encode_ac3_frame();
    }
}

//
// ============================================================
// ENCODE ONE AC3 FRAME
// ============================================================
//

void OutputTS::encode_ac3_frame(void)
{
    constexpr int AC3_FRAME_SAMPLES = 1536;
    constexpr int AC3_SAMPLE_RATE   = 48000;

    AVFrame* frame = av_frame_alloc();

    if (!frame)
        return;

    frame->format      = AV_SAMPLE_FMT_FLTP;
    frame->sample_rate = AC3_SAMPLE_RATE;
    frame->nb_samples  = AC3_FRAME_SAMPLES;
    frame->pts         = m_audio_total_samples;

    av_channel_layout_copy(&frame->ch_layout,
                           &m_audioStream.enc->ch_layout);

    int ret = av_frame_get_buffer(frame, 0);
    if (ret < 0)
    {
        av_frame_free(&frame);
        return;
    }

    // Pull exact AC3 frame
    ret = av_audio_fifo_read(m_audio_fifo,
                             reinterpret_cast<void**>(frame->data),
                             AC3_FRAME_SAMPLES);

    if (ret < AC3_FRAME_SAMPLES)
    {
        av_frame_free(&frame);
        return;
    }

    //
    // Advance perfect sample clock
    //
    m_audio_total_samples += AC3_FRAME_SAMPLES;

    //
    // Encode
    //
    if (!encode_frame(&m_audioStream, frame, m_audioPktQ))
        m_log->error("encode_frame(audio) failed.");

    av_frame_free(&frame);
}

void OutputTS::initialize_bitstream_parser(void)
{
    BitstreamAudioParser::AccessUnitCallback au_handler =
        [this](PacketPtr&& pkt)
    {
        pkt->stream_index = AUDIO_STREAM_ID;

        Packet qp {
            .is_marker    = false,
            .dts          = pkt->dts,
            .time_base    = TimeBase::Magewell,
            .pkt          = std::move(pkt),
            .codec_par    = nullptr
        };

        m_audioPktQ.Push(std::move(qp));
        m_pktQ_ready.notify_one();
    };

    BitstreamAudioParser::ConfigChangeCallback config_handler =
        [this](const DynamicStreamParams& new_params)
    {
        m_log->info("%%%%%% Audio:\n{}", new_params);

        CodecParamsPtr codecpar = make_codec_params();
        if (!codecpar) {
            m_log->error("Failed to allocate codec parameters memory block.");
            return;
        }

        codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        codecpar->sample_rate = new_params.sample_rate;
        codecpar->ch_layout.nb_channels = new_params.channels;

        if (new_params.codec == DetectedCodec::AC3) {
            codecpar->codec_id = AV_CODEC_ID_AC3;
        }
        else if (new_params.codec == DetectedCodec::EAC3) {
            codecpar->codec_id = AV_CODEC_ID_EAC3;
        }
        else if (new_params.codec == DetectedCodec::AC4) {
            codecpar->codec_id = AV_CODEC_ID_AC4;
        }

        this->configure_bitstream(std::move(codecpar));
    };

    m_bitstream_parser =
        std::make_unique<BitstreamAudioParser>(std::move(au_handler),
                                               std::move(config_handler));
}

void OutputTS::process_bitstream(AudioSamples&& samples)
{
    if (m_bitstream_parser)
    {
        m_bitstream_parser->consumeBuffer(std::move(samples.data),
                                          samples.timestamp);
    }
}

bool OutputTS::configure_bitstream(CodecParamsPtr codecpar)
{
    // Flush previous encoder
    if (m_audioStream.enc)
    {
        flush_packets(&m_audioStream,
                      m_audioPktQ);
    }

    Packet marker
        {
            .is_marker = true,
            .time_base = TimeBase::Magewell,
            .codec_par = std::move(codecpar)
        };

    m_log->info("Opened passthrough {} stream",
                avcodec_get_name(marker.codec_par->codec_id));

    m_audioPktQ.Push(std::move(marker));
    m_pktQ_ready.notify_one();

    return true;
}

//
// ============================================================
// FINAL FLUSH
// ============================================================
//
void OutputTS::flush_audio_pipeline(void)
{
    if (m_audioStream.enc)
        flush_packets(&m_audioStream, m_audioPktQ);

    if (m_audio_fifo)
    {
        av_audio_fifo_free(m_audio_fifo);
        m_audio_fifo = nullptr;
    }
}


void OutputTS::AddAudioSamples(AudioSamples&& samples)
{
    const std::unique_lock<std::mutex> lock(m_audioQ_mutex);

    m_audioQ.push_back(std::move(samples));
    m_audioQ_ready.notify_one();

    return;
}
