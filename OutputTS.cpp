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
#include <semaphore>

extern "C" {
#include <libavutil/opt.h>
}

#include "OutputTS.h"
#include "VideoStream.h"
#include "PCMStream.h"
#include "BitStream.h"

using namespace std;

inline std::string DumpAVFormat(const AVFormatContext* fmtctx,
                                string color_desc)
{
    fmt::memory_buffer out;

    fmt::format_to(std::back_inserter(out),
                   "Format: {} ({})",
                   fmtctx->oformat
                   ? fmtctx->oformat->name
                   : fmtctx->iformat->name,
                   fmtctx->oformat
                   ? fmtctx->oformat->long_name
                   : fmtctx->iformat->long_name);

    for (unsigned idx = 0; idx < fmtctx->nb_streams; ++idx)
    {
        const AVStream* st = fmtctx->streams[idx];
        const AVCodecParameters* cp = st->codecpar;

        const AVCodecDescriptor* codec =
            avcodec_descriptor_get(cp->codec_id);

        const char* codec_name = codec
                                 ? codec->name
                                 : "unknown";

#if 0
        const char* codec_long = codec && codec->long_name
                                 ? codec->long_name
                                 : "";
#endif

        fmt::format_to(std::back_inserter(out), "\n    #{} {} {}",
                       idx, av_get_media_type_string(cp->codec_type),
                       codec_name);

        if (cp->bit_rate > 0)
        {
            fmt::format_to(std::back_inserter(out), " {}kb/s",
                           cp->bit_rate / 1000);
        }

        switch (cp->codec_type)
        {
            case AVMEDIA_TYPE_VIDEO:
            {
                fmt::format_to(std::back_inserter(out), " {}x{}p{:.2f}",
                               cp->width, cp->height,
                               av_q2d(st->avg_frame_rate));

                const AVPixFmtDescriptor* pix =
                    av_pix_fmt_desc_get(static_cast<AVPixelFormat>(cp->format));

                if (pix)
                {
                    fmt::format_to(std::back_inserter(out), " {}({})",
                                   pix->name, color_desc);
                }

                break;
            }

            case AVMEDIA_TYPE_AUDIO:
            {
                fmt::format_to(std::back_inserter(out), " {}hz",
                               cp->sample_rate);

                char layout[128] {};

                if (cp->ch_layout.nb_channels > 0)
                {
                    av_channel_layout_describe(&cp->ch_layout,
                                               layout,
                                               sizeof(layout));

                    fmt::format_to(std::back_inserter(out),
                                   " Channels: {} ({})",
                                   cp->ch_layout.nb_channels, layout);
                }

#if 0
                if (cp->format)
                {
                    fmt::format_to(std::back_inserter(out), " {}",
                                   av_get_sample_fmt_name
                                   (static_cast<AVSampleFormat>
                                    (cp->format)));
                }
#endif
            }

            default:
                break;
        }
    }

    return fmt::to_string(out);
}

OutputTS::OutputTS(int verbose_level, bool isEco,
                   VideoStream::Args&& video_args,
                   ShutdownCallback shutdown,
                   VideoStream::MagCallback image_buffer_avail)
    : m_verbose(verbose_level)
    , m_video_args(std::move(video_args))
    , f_shutdown(shutdown)
    , f_image_avail(image_buffer_avail)
{
    m_log = spdlog::get("app_logger");
    if (!m_log)
    {
        std::cerr << "OutputTS Error: Logger 'app_logger' not found!"
                  << std::endl;
        return;
    }

    if (m_verbose > 4)
        av_log_set_level(AV_LOG_DEBUG);
    else if (m_verbose > 3)
        av_log_set_level(AV_LOG_INFO);
    else
        av_log_set_level(AV_LOG_QUIET);

    // Initialize atomic runtime state machine flags
    m_running.store(true);

    // Start up threads last
    m_audio_thread = std::thread(&OutputTS::process_audio, this);
    pthread_setname_np(m_audio_thread.native_handle(), "audenc");

    m_video_thread = std::thread(&OutputTS::process_video, this);
    pthread_setname_np(m_video_thread.native_handle(), "vidcpy");

    m_mux_thread = std::thread(&OutputTS::mux, this);
    pthread_setname_np(m_mux_thread.native_handle(), "mux");
}

OutputTS::~OutputTS(void)
{
    if (m_verbose > 2)
        m_log->info("Cleaning Transport Stream");

    // Signal all execution loops to cease operations
    m_running.store(false);
    Shutdown();

    while (!m_audioPktQ.IsEmpty())
    {
        auto entry = m_audioPktQ.PopValue();
    }
    m_audioPktQ.Shutdown();

    if (m_verbose > 2)
        m_log->info("Waiting for threads to exit.");

    if (m_audio_thread.joinable())
        m_audio_thread.join();

    if (m_video_thread.joinable())
        m_video_thread.join();

    if (m_mux_thread.joinable())
        m_mux_thread.join();

    m_log->info("Releasing core resource footprints...");

    close_container();

    if (m_verbose > 2)
        m_log->info("Transport Stream shutdown");
}

/**
 * @brief Shutdown the output TS handler
 * @note Stops all threads and cleans up resources
 */
void OutputTS::Shutdown(void)
{
    if (m_running.exchange(false))
        f_shutdown();
    m_imageQ_ready.notify_all();
    m_audioQ_ready.notify_all();
    m_pktQ_ready.notify_all();
}

void OutputTS::log_packet(string where, const AVFormatContext* fmt_ctx,
                          const AVPacket* pkt, int version)
{
    AVRational* time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

    m_log->info("{} [id:{}, version:{}] pts: {} pts_time: {} "
                "dts: {} dts_time: {} duration: {} "
                "duration_time: {}",
                where, pkt->stream_index, version, pkt->pts,
                AV_ts2timestr(pkt->pts, time_base),
                AV_ts2str(pkt->dts),
                AV_ts2timestr(pkt->dts, time_base),
                AV_ts2str(pkt->duration),
                AV_ts2timestr(pkt->duration, time_base));
}


void OutputTS::optimize_mpegts(AVFormatContext* format_ctx)
{
    // Maximize the OS Kernel pipe capacity for stdout
    fcntl(STDOUT_FILENO, F_SETPIPE_SZ, 1048576);

    // Enable real-time low-latency packet configurations
    format_ctx->flags |= AVFMT_FLAG_FLUSH_PACKETS;
    av_opt_set_int(format_ctx->priv_data, "muxrate",
                   0, AV_OPT_SEARCH_CHILDREN);
    av_opt_set_int(format_ctx->priv_data, "pes_payload_size",
                   0, AV_OPT_SEARCH_CHILDREN);

    // Optimize FFmpeg's internal pipe buffer to match the 1MB
    // kernel pipe size This stops it from performing hundreds of
    // tiny, unbuffered 188-byte writes
    if (format_ctx->pb)
    {
        constexpr int PIPE_BUFFER_SIZE = 1048576; // 1MB
        uint8_t* new_buf = (uint8_t*)av_malloc(PIPE_BUFFER_SIZE);
        if (new_buf)
        {
            // Safely swap out the tiny default 32KB internal
            // buffer for a 1MB buffer
            av_free(format_ctx->pb->buffer);
            format_ctx->pb->buffer = new_buf;
            format_ctx->pb->buffer_size = PIPE_BUFFER_SIZE;
            format_ctx->pb->buf_ptr = new_buf;
            format_ctx->pb->buf_end = new_buf + PIPE_BUFFER_SIZE;
        }
    }
}

// Open Transport Stream container
bool OutputTS::open_container(void)
{
    close_container();

    if (m_verbose > 1)
        m_log->info("================ open container begin ================");

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

    avcodec_parameters_copy(v_st->codecpar, m_video_marker->codec_par.get());
    v_st->time_base = m_video_marker->time_base;
    v_st->avg_frame_rate = AVRational {
        m_video_marker->frame_duration.den,
        m_video_marker->frame_duration.num
    };

    // TRACK 1: Audio track initialization (Conditional)
    if (!m_no_audio && m_audio_marker.has_value())
    {
        AVStream* a_st = avformat_new_stream(m_formatContext, nullptr);
        if (a_st == nullptr) return false;

        avcodec_parameters_copy(a_st->codecpar,
                                m_audio_marker->codec_par.get());
        a_st->time_base = m_audio_marker->time_base;
        a_st->avg_frame_rate = AVRational {
            m_audio_marker->frame_duration.den,
            m_audio_marker->frame_duration.num
        };
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
    {
        string color_desc;
        {
            std::scoped_lock lock(m_videoStream_mutex);
            if (m_videoStream)
                color_desc = m_videoStream->ColorSpaceDesc();
        }

        m_log->info(DumpAVFormat(m_formatContext, color_desc));
    }

    AVDictionary* muxer_opts = nullptr;
    // Force the muxer to insert a PCR timestamp at minimum every 20ms to 40ms
    av_dict_set(&muxer_opts, "pcr_period", "20", 0);

    // Ensure strict transport stream compliance layout

    // VBR mode, but forces clock packets
    av_dict_set(&muxer_opts, "muxrate", "0", 0);

    optimize_mpegts(m_formatContext);

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
        m_log->info("================ open container end ================");

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

bool OutputTS::queue_packets(int stream_id, int version,
#ifdef DEBUG_TS
                             int64_t mw_ts,
                             int64_t enc_ts,
#endif
                             AVCodecContext* enc,
                             MediaQueue& pktQ, bool flushing)
{
    if (!enc)
        return false;

    int ret = 0;

    for (;;)
    {
        PacketPtr pkt = make_packet();

        if (!pkt)
        {
            m_log->error("Failed allocating AVPacket.");
            return false;
        }

        ret = avcodec_receive_packet(enc, pkt.get());

        if (flushing)
        {
            if (ret == AVERROR_EOF)
                return true;
            if (ret == AVERROR(EAGAIN))
                continue;
            if (ret < 0)
            {
                m_log->warn("Failed encoding frame: {}",
                            AVerr2str(ret));
                return false;
            }
        }

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            return true;
        }
        if (ret < 0)
        {
            m_log->warn("Failed encoding frame: {}",
                        AVerr2str(ret));
            return false;
        }

        pkt->stream_index = stream_id;
        av_packet_rescale_ts(pkt.get(),
                             enc->time_base,
                             TimeBase::MPEG_TS);

        // Some encoders omit DTS
        if (pkt->dts == AV_NOPTS_VALUE)
        {
            m_log->warn("Encoder did not generate a DTS. Using PTS.");
            pkt->dts = pkt->pts;
        }

        if (pkt->stream_index != stream_id)
        {
            m_log->error("Stream ID {}, pkt->stream_index {}",
                         stream_id, pkt->stream_index);
        }

        Packet qp
            {
                .is_marker    = false,
                .stream_id    = stream_id,
                .version      = version,
                .time_base    = enc->time_base,
#ifdef DEBUG_TS
                .mw_ts        = mw_ts,
                .enc_ts       = enc_ts,
#endif
                .pkt          = std::move(pkt),
                .codec_par    = nullptr
            };

        pktQ.Push(std::move(qp));
        m_pktQ_ready.notify_one();
    }

    return true;
}

bool OutputTS::EncodeFrame(int stream_id, int version,
#ifdef DEBUG_TS
                           int64_t mw_ts,
#endif
                           AVCodecContext* enc, AVFrame* frame)
{
    if (!enc)
        return false;

    MediaQueue& pktQ = (stream_id == AUDIO_STREAM_ID)
                       ? m_audioPktQ
                       : m_videoPktQ;

#ifdef DEBUG_TS
    int64_t enc_ts = frame->pts;
#endif

    for (;;)
    {

        // Try to submit the frame
        int ret = avcodec_send_frame(enc, frame);

        if (ret == 0)
        {
            // The encoder accepted ownership of the frame buffers.
            return queue_packets(stream_id, version,
#ifdef DEBUG_TS
                                 mw_ts,
                                 enc_ts,
#endif
                                 enc, pktQ, false);
        }

        if (ret == AVERROR(EAGAIN))
        {
            // The encoder internal buffer is full. Pull packets out
            // to make room.
            m_log->info("Encoder saturated (EAGAIN). "
                        "Flushing packets to clear space.");

            if (!queue_packets(stream_id, version,
#ifdef DEBUG_TS
                               mw_ts,
                               enc_ts,
#endif
                               enc, pktQ, false))
            {
                m_log->error("Failed draining packets during EAGAIN "
                             "recovery loop.");
                return false;
            }

            // Loop around and try to send the exact same frame again.
            continue;
        }

        // If it's EOF (during flush) or another critical error code
        if (ret == AVERROR_EOF)
        {
            return queue_packets(stream_id, version,
#ifdef DEBUG_TS
                                 mw_ts,
                                 enc_ts,
#endif
                                 enc, pktQ, false);
        }

        m_log->warn("Critical failure sending a frame to the encoder: {}",
                    AVerr2str(ret));
        Shutdown();
        return false;
    }
}

bool OutputTS::FlushPackets(int stream_id, int version, AVCodecContext* enc)
{
    if (!enc)
        return true;

    if (!avcodec_is_open(enc))
    {
        m_log->warn("Cannot flush packets: Not open");
        return true;
    }

    MediaQueue& pktQ = (stream_id == AUDIO_STREAM_ID)
                       ? m_audioPktQ
                       : m_videoPktQ;

    m_log->debug("flush_packets id={} version={} Started, PktQ size {}",
                stream_id, version, pktQ.GetSize());

    // Enter draining mode by passing nullptr
    int ret = avcodec_send_frame(enc, nullptr);
    if (ret < 0 && ret != AVERROR_EOF)
    {
        if (m_verbose > 0) {
            m_log->error("Error entering encoder drain mode: {}",
                         AVerr2str(ret));
        }
        return false;
    }

    queue_packets(stream_id, version,
#ifdef DEBUG_TS
                  AV_NOPTS_VALUE,
                  AV_NOPTS_VALUE,
#endif
                  enc, pktQ, true);
    m_log->debug("flush_packets id={} version={} Finished, PktQ size {}",
                stream_id, version, pktQ.GetSize());

    return true;
}

void OutputTS::sync_markers(void)
{
    std::optional<Packet> outPkt;

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    std::scoped_lock lock(m_audio_pktQ_mutex, m_video_pktQ_mutex);

    m_log->debug("MARKER received. Video current {} latest {}; "
                "Audio current {} latest {}",
                m_video_current_version, m_video_latest_version.load(),
                m_audio_current_version, m_audio_latest_version.load());

    if (m_videoPktQ.PeekMarker())
    {
        outPkt = m_videoPktQ.PopValue();
        m_video_current_version = outPkt->version;
#if 0
        m_sequence.Push(*outPkt, outPkt->pkt.get());
#endif
        m_video_marker = std::move(outPkt);
    }
    if (m_audioPktQ.PeekMarker())
    {
        outPkt = m_audioPktQ.PopValue();
        m_audio_current_version = outPkt->version;
        m_audio_marker = std::move(outPkt);
    }

    m_log->debug("Pending DTS; audio {} video {}",
                 m_audioPktQ.PeekDts(), m_videoPktQ.PeekDts());

    ++m_generation;
    open_container();
}

void OutputTS::mux(void)
{
#ifdef DEBUG_TS
    struct StreamState {
        int     stream_id{-1};
        int64_t pts{AV_NOPTS_VALUE};
        int64_t dts{AV_NOPTS_VALUE};
        int64_t dur{0};
        AVRational enc_tb;
        int64_t  mw_ts {AV_NOPTS_VALUE};
        int64_t  enc_ts {AV_NOPTS_VALUE};
    };
    using StreamStateQ = deque<StreamState>;
    std::unordered_map<int, StreamStateQ> tracks;

    StreamStateQ globalQ;
#else
    struct StreamState {
        int     stream_id{-1};
        int64_t pts{AV_NOPTS_VALUE};
        int64_t dts{AV_NOPTS_VALUE};
    } prev_state;
#endif

    for (;;)
    {
        std::shared_ptr<VideoStream> vidStream = nullptr;

        {
            std::unique_lock<std::mutex> cv_lock(m_pktQ_mutex);
            m_pktQ_ready.wait(cv_lock, [this] {
                return (!m_running || !m_videoPktQ.IsEmpty() ||
                        m_videoQsize.load(std::memory_order_acquire) > 0);
            });
        }

        if (m_videoQsize.load(std::memory_order_relaxed) > 0)
        {
            std::scoped_lock lock(m_videoStream_mutex);
            if (m_videoStream)
                vidStream = m_videoStream;
        }

        if (vidStream)
        {
            if (vidStream->EncodeFrame())
                m_videoQsize.fetch_sub(1, std::memory_order_release);
            else
                m_videoQsize.store(0, std::memory_order_release);
        }

        if (m_videoPktQ.IsEmpty())
        {
            if (!m_running)
                break; // shutdown
            continue; // Catch spurious wakeups
        }

        auto* targetQ = &m_videoPktQ;
        bool is_audio_next = false;

        if (!m_audioPktQ.IsEmpty())
        {
            if (m_audioPktQ.PeekDts() < m_videoPktQ.PeekDts())
            {
                targetQ = &m_audioPktQ;
                is_audio_next = true;
            }
        }

        if (targetQ->PeekMarker())
        {
            sync_markers();
            continue;
        }

        std::optional<Packet> outPkt = targetQ->PopValue();
        if (!outPkt || !outPkt->pkt)
        {
            m_log->warn("Mux: Extracted packet payload is null");
            continue;
        }

        PacketPtr pkt = std::move(outPkt->pkt);
        int stream_id = pkt->stream_index;

        m_log->trace("POST {} pts={} dts={} dur={}",
                     is_audio_next ? "audio" : "video",
                     pkt->pts, pkt->dts, pkt->duration);

        if (pkt->dts == AV_NOPTS_VALUE)
        {
            m_log->warn("MUX [id{:<2d} version:{}] Missing DTS timestamp!",
                        stream_id, outPkt->version);
            continue;
        }

#ifdef DEBUG_TS
        auto& stateQ = tracks[stream_id];
        StreamState prev_state;
        if (stateQ.empty())
            prev_state = StreamState {};
        else
            prev_state = stateQ.back();
#endif

        // Non-monotonic Timestamp Protection
        if (prev_state.dts != AV_NOPTS_VALUE && pkt->dts <= prev_state.dts)
        {
            m_log->debug("MUX [{}] DTS delta {} non-monotonic: "
                         "Fix: {:12d} -> {:12d}",
                         stream_id, pkt->dts - prev_state.dts,
                         pkt->dts, prev_state.dts + 1);
            pkt->dts = prev_state.dts + 1;
            pkt->pts = std::max(pkt->pts, pkt->dts);
        }

        if (pkt->pts < pkt->dts)
        {
            m_log->warn("PTS < DTS adjustment on stream {}", stream_id);
            pkt->pts = pkt->dts;
        }

        m_log->debug("MUX [id{:<2d} version:{}] pts:{:#018x} dts:{:#018x} "
                     "duration:{} size:{}",
                     stream_id, outPkt->version, pkt->pts, pkt->dts,
                     pkt->duration, pkt->size);

#ifdef DEBUG_TS
        StreamState state = StreamState {
            .stream_id = stream_id,
            .pts = pkt->pts,
            .dts = pkt->dts,
            .dur = pkt->duration,
            .enc_tb = outPkt->time_base,
            .mw_ts = outPkt->mw_ts,
            .enc_ts = outPkt->enc_ts
        };
#else
        StreamState state = StreamState {
            .stream_id = stream_id,
            .pts = pkt->pts,
            .dts = pkt->dts
        };
#endif

#if 1
        int ret = av_interleaved_write_frame(m_formatContext, pkt.get());
#else
        int ret = av_write_frame(m_formatContext, pkt.get());
#endif
        if (ret < 0)
        {
            if (ret == AVERROR(EINVAL))
            {
                m_log->warn("DAMAGED: Mux rejected packet "
                            "id={} pts={} -> {} dts={} -> {}: {}",
                            stream_id, prev_state.pts, state.pts,
                            prev_state.dts, state.dts,
                            AVerr2str(ret));
#ifdef DEBUG_TS
                int64_t prev_dts = state.dts;
                m_log->info("Previous 10:");
                for (auto &st : stateQ)
                {
                    m_log->info("dts:{} diff:{} pts:{} dur:{} "
                                "Magewell TS:{} TB:{}/{} "
                                "Encoder TS:{} TB:{}/{} "
                                "Stream TB:{}/{}",
                                st.dts, prev_dts - st.dts, st.pts, st.dur,
                                st.mw_ts, 1, 10000000,
                                st.enc_ts, st.enc_tb.num, st.enc_tb.den,
                                1, 90000);

                    prev_dts = st.dts;
                }

                m_log->info("Previous a/v mix:");
                for (auto &st : globalQ)
                {
                    m_log->info("[{}] dts:{} pts:{} dur:{} "
                                "Magewell TS:{} TB:{}/{} "
                                "Encoder TS:{} TB:{}/{} "
                                "Stream TB:{}/{}",
                                st.stream_id,
                                st.dts, st.pts, st.dur,
                                st.mw_ts, 1, 10000000,
                                st.enc_ts, st.enc_tb.num, st.enc_tb.den,
                                1, 90000);
                }
#endif
            }
            else
            {
                m_log->error("av_interleaved_write_frame stream {} failed: {}",
                             stream_id, AVerr2str(ret));
            }
        }
        else
        {
#ifdef DEBUG_TS
            // Cache state parameters for the next frame iteration on this track
            stateQ.push_back(state);
            if (stateQ.size() > 20)
                stateQ.pop_front();
            globalQ.push_back(state);
            if (globalQ.size() > 40)
                globalQ.pop_front();
#else
            prev_state = state;
#endif
        }
    }
}


int OutputTS::AddMarker(int id, CodecParamsPtr&& codecpar,
                        AVRational timebase, AVRational frameduration,
                        int64_t timestamp)
{
    Packet marker;

    int64_t marker_dts_pts = av_rescale_q(timestamp,
                                          TimeBase::Magewell,
                                          TimeBase::MPEG_TS);

    marker.stream_id      = id;
    marker.is_marker      = true;
    marker.codec_par      = std::move(codecpar);
    marker.time_base      = TimeBase::MPEG_TS;
    marker.frame_duration = frameduration;
    marker.pkt = make_packet();
    marker.pkt->pts = marker_dts_pts;
    marker.pkt->dts = marker_dts_pts;

    int version;

    // Note: fetch_add returns the previous value, so incr it.
    if (id == AUDIO_STREAM_ID)
    {
        m_log->debug("AddMarker: Audio");
        version = marker.version =
            m_audio_latest_version.fetch_add(1, std::memory_order_relaxed) + 1;
        m_audioPktQ.Push(std::move(marker));
    }
    else if (id == VIDEO_STREAM_ID)
    {
        m_log->debug("AddMarker: Video");
        version = marker.version =
            m_video_latest_version.fetch_add(1, std::memory_order_relaxed) + 1;
        m_videoPktQ.Push(std::move(marker));
    }
    m_pktQ_ready.notify_one();
    return version;
}

void OutputTS::AddAudioPkt(Packet&& pkt)
{
    m_audioPktQ.Push(std::move(pkt));
    m_pktQ_ready.notify_one();
}

// Thread entry
void OutputTS::process_audio(void)
{
    AudioStream* audioStream {nullptr};

    for (;;)
    {
        AudioStream::Samples audio;

        {
            std::unique_lock<std::mutex> lock(m_audioQ_mutex);

            m_audioQ_ready.wait(lock, [this]() {
                return !m_running.load() || !m_audioQ.empty();
            });

            if (m_audioQ.empty())
            {
                if (!m_running)
                    break;
                continue;
            }

            audio = std::move(m_audioQ.front());
            m_audioQ.pop_front();
        }

        if (audio.oParams.has_value())
        {
            std::scoped_lock lock(m_audio_pktQ_mutex);

            delete audioStream;
            if (audio.oParams->is_lpcm)
            {
                audioStream = new PCMStream(*this, m_verbose,
                                            std::move(*audio.oParams),
                                            audio.timestamp);
            }
            else
            {
                audioStream = new BitStream(*this, m_verbose,
                                            std::move(*audio.oParams),
                                            audio.timestamp);
            }
        }

        audioStream->AddSamples(std::move(audio));
    }

    delete audioStream;
    m_log->info("process_audio thread exited.");
}

void OutputTS::AddAudioSamples(AudioStream::Samples&& samples)
{
    std::scoped_lock lock(m_audioQ_mutex);

    m_audioQ.push_back(std::move(samples));
    m_audioQ_ready.notify_one();

    return;
}

// Thread entry
void OutputTS::process_video(void)
{
    int            vidpool_used_1m  {0};
    array<int, 5>  vidpool_used_5m  {0};
    array<int, 10> vidpool_used_10m {0};
    int            vidpool_5m_idx   {0};
    int            vidpool_10m_idx  {0};
    array<int, 5>::iterator  vidpool_5m_max;
    array<int, 10>::iterator vidpool_10m_max;
    chrono::seconds total_duration;

    int used = 0;

    chrono::steady_clock::time_point start_tm   = chrono::steady_clock::now();
    chrono::steady_clock::time_point vidpool_tm = start_tm;
    chrono::steady_clock::time_point current_tm;
    int duration;

    for (;;)
    {
        VideoStream::Image image;

        {
            // Wait for next image
            std::unique_lock<std::mutex> lock(m_imageQ_mutex);

            m_imageQ_ready.wait(lock, [this]() {
                return !m_running.load() || !m_imageQ.empty();
            });

            if (m_imageQ.empty())
            {
                if (!m_running.load())
                    break;
                continue;
            }

            image = std::move(m_imageQ.front());
            m_imageQ.pop_front();
        }

        // Encoder reconfiguration request
        if (image.oParams.has_value())
        {
            std::scoped_lock lock(m_videoStream_mutex);
            m_videoStream = std::make_shared<VideoStream>
                            (*this, m_verbose,
                             m_video_args,
                             std::move(*image.oParams),
                             f_image_avail,
                             image.timestamp);

            m_videoQsize.exchange(0, std::memory_order_acq_rel);
        }

        while (m_running.load())
        {
            if (m_videoQsize.load(std::memory_order_acquire) <
                m_video_args.buffers)
            {
                int res;

                if ((res = m_videoStream->AddImage(std::move(image))) < 0)
                {
                    Shutdown();
                }
                if (res > 0)
                {
                    used = m_videoQsize.fetch_add(1, std::memory_order_release)
                           + 1; // Returns the previous value
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        m_pktQ_ready.notify_one();


        if (m_verbose > 1)
        {
            if (vidpool_used_1m < used)
                vidpool_used_1m = used;
            if (vidpool_used_5m[vidpool_5m_idx] < used)
                vidpool_used_5m[vidpool_5m_idx] = used;
            if (vidpool_used_10m[vidpool_10m_idx] < used)
                vidpool_used_10m[vidpool_10m_idx] = used;

            current_tm = chrono::steady_clock::now();
            duration = chrono::duration_cast<chrono::seconds>
                       (current_tm - vidpool_tm).count();

            total_duration = chrono::duration_cast<chrono::seconds>
                             (chrono::steady_clock::now() - start_tm);

            if (duration >= 60)
            {
                vidpool_5m_max  = ranges::max_element(vidpool_used_5m);
                vidpool_10m_max = ranges::max_element(vidpool_used_10m);

                // spdlog doesn't support c++20 format yet, so no :%T.
                m_log->info(format("     GPU frame pool used 1m:{:<3d} "
                                   "5m:{:<3d} 10m:{:<3d} "
                                   "of {:<3d} ({:%T} elapsed)",
                                   vidpool_used_1m, *vidpool_5m_max,
                                   *vidpool_10m_max, m_video_args.buffers,
                                   total_duration));

                vidpool_used_1m = 0;

                ++vidpool_5m_idx;
                vidpool_5m_idx %= 5;
                vidpool_used_5m[vidpool_5m_idx] = 0;

                ++vidpool_10m_idx;
                vidpool_10m_idx %= 10;
                vidpool_used_10m[vidpool_10m_idx] = 0;

                vidpool_tm = current_tm;
            }
        }
    }

    std::scoped_lock lock(m_videoStream_mutex);
    m_videoStream.reset();
    m_log->info("process_video thread exited.");
}


void OutputTS::AddVideoImage(VideoStream::Image&& image)
{
    std::scoped_lock lock(m_imageQ_mutex);

    if (m_running.load() == false)
    {
        f_image_avail(image.pImage, image.pEco);
        return;
    }

    m_imageQ.push_back(std::move(image));
    m_imageQ_ready.notify_one();
}
