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
                                   " Channels: {} ({}) [",
                                   cp->ch_layout.nb_channels,
                                   layout);

                    // Iterate through every channel index and append its layout name
                    for (int i = 0; i < cp->ch_layout.nb_channels; ++i)
                    {
                        enum AVChannel ch = av_channel_layout_channel_from_index(&cp->ch_layout, i);
                        char ch_name[16] {};
                        av_channel_name(ch_name, sizeof(ch_name), ch);

                        // Add a space separator between channel names
                        fmt::format_to(std::back_inserter(out),
                                       "{}{}",
                                       (i > 0) ? " " : "",
                                       ch_name);
                    }

                    fmt::format_to(std::back_inserter(out), "]");
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
    // Give the stdout pipe some room for transient output bursts.
    fcntl(STDOUT_FILENO, F_SETPIPE_SZ, 1024 * 1024);

    // Flush MPEG-TS output promptly rather than allowing AVIO buffering
    // to introduce additional latency.
    format_ctx->flags |= AVFMT_FLAG_FLUSH_PACKETS;
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

    ret = avcodec_parameters_from_context(v_st->codecpar,
                                         m_video_marker->marker->encoder.get());
    v_st->time_base = m_video_marker->marker->time_base;
    v_st->avg_frame_rate = AVRational {
        m_video_marker->marker->frame_duration.den,
        m_video_marker->marker->frame_duration.num
    };

    // TRACK 1: Audio track initialization (Conditional)
    if (!m_no_audio && m_audio_marker.has_value())
    {
        AVStream* a_st = avformat_new_stream(m_formatContext, nullptr);
        if (a_st == nullptr) return false;

        avcodec_parameters_copy(a_st->codecpar,
                                m_audio_marker->marker->codec_par.get());
        a_st->time_base = m_audio_marker->marker->time_base;
        a_st->avg_frame_rate = AVRational {
            m_audio_marker->marker->frame_duration.den,
            m_audio_marker->marker->frame_duration.num
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

    // Request PCR insertion at least every 20 ms.
    av_dict_set(&muxer_opts, "pcr_period", "20", 0);

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
            {
                m_log->warn("Encoder returned EAGAIN while flushing.");
                return true;
            }

            if (ret < 0)
            {
                m_log->warn("Failed encoding frame: {}",
                            AVerr2str(ret));
                return false;
            }
        }

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return true;

#if 0
        if (stream_id == VIDEO_STREAM_ID)
        {
            log_packet("queue_packets (after receive, before scale)", m_formatContext, pkt.get(), 0);
            m_log->info(
                        "ENCODER packet TB={}/{} pts={} ({:.6f}s) dts={} ({:.6f}s)",
                        enc->time_base.num,
                        enc->time_base.den,
                        pkt->pts,
                        pkt->pts * av_q2d(enc->time_base),
                        pkt->dts,
                        pkt->dts * av_q2d(enc->time_base));
        }
#endif

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

#if 0
        if (stream_id == VIDEO_STREAM_ID)
        {
            log_packet("queue_packets (after receive, after scale)", m_formatContext, pkt.get(), 0);
            m_log->info(
                        "STREAM packet TB={}/{} pts={} ({:.6f}s) dts={} ({:.6f}s)",
                        TimeBase::MPEG_TS.num,
                        TimeBase::MPEG_TS.den,
                        pkt->pts,
                        pkt->pts * av_q2d(TimeBase::MPEG_TS),
                        pkt->dts,
                        pkt->dts * av_q2d(TimeBase::MPEG_TS));
        }
#endif

        if (pkt->dts == AV_NOPTS_VALUE)
        {
            m_log->error("Encoder returned packet without DTS: pts={}",
                         pkt->pts);
            return false;
        }

        if (pkt->stream_index != stream_id)
        {
            m_log->error("Stream ID {}, pkt->stream_index {}",
                         stream_id, pkt->stream_index);
        }

        Packet qp {
            .version      = version,
            .pkt          = std::move(pkt),
        };

        pktQ.Push(std::move(qp));
    }

    return true;
}

bool OutputTS::EncodeFrame(int stream_id, int version,
                           AVCodecContext* enc, AVFrame* frame)
{
    if (!enc)
        return false;

    MediaQueue& pktQ = (stream_id == AUDIO_STREAM_ID)
                       ? m_audioPktQ
                       : m_videoPktQ;

    for (;;)
    {

#ifdef LOG_ELAPSED
        chrono::steady_clock::time_point encode_start
            = chrono::steady_clock::now();
#endif

#if 0
        if (stream_id == VIDEO_STREAM_ID)
        {
            m_log->info(
                         "ENCODE SEND stream={} frame_pts={} duration={}",
                         stream_id,
                         frame->pts,
                         frame->duration);
            m_log->info(
                        "ENCODER time_base={}/{} framerate={}/{} frame_pts={}",
                        enc->time_base.num,
                        enc->time_base.den,
                        enc->framerate.num,
                        enc->framerate.den,
                        frame->pts);
        }
#endif

        // Try to submit the frame
        int ret = avcodec_send_frame(enc, frame);

#ifdef LOG_ELAPSED
        chrono::steady_clock::time_point encode_end
            = chrono::steady_clock::now();

        chrono::steady_clock::time_point queue_start
            = chrono::steady_clock::now();
#endif
        if (ret == 0)
        {
            ret = queue_packets(stream_id, version,
                                enc, pktQ, false);
#ifdef LOG_ELAPSED
            chrono::steady_clock::time_point queue_end
                = chrono::steady_clock::now();

            auto encode_dur = chrono::duration_cast<chrono::microseconds>
                              (encode_end - encode_start);
            auto queue_dur = chrono::duration_cast<chrono::microseconds>
                             (queue_end - queue_start);

            if (stream_id == VIDEO_STREAM_ID &&
                (encode_dur > 6ms || queue_dur > 1ms))
            {
                m_log->debug("avcodec_send_frame {}μs queue_packets {}μs",
                             encode_dur.count(), queue_dur.count());
            }
#endif
            m_pktQ_ready.notify_all();
            return ret;
        }

        if (ret == AVERROR(EAGAIN))
        {
            // A packet needs drained before the encoder can accept
            // another frame.
            m_log->info("Encoder saturated (EAGAIN). "
                        "Flushing packets to clear space.");

            if (!queue_packets(stream_id, version,
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
            return queue_packets(stream_id, version, enc, pktQ, false);
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

    m_log->trace("flush_packets id={} version={} Started, PktQ size {}",
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

    if (!queue_packets(stream_id, version, enc, pktQ, true))
    {
        m_log->error("Failed draining encoder during flush.");
        return false;
    }

    m_log->trace("flush_packets id={} version={} Finished, PktQ size {}",
                 stream_id, version, pktQ.GetSize());

    return true;
}

void OutputTS::sync_markers(void)
{
    std::optional<Packet> outPkt;

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    std::scoped_lock lock(m_audio_pktQ_mutex, m_video_pktQ_mutex);

    m_log->trace("MARKER received. Video current {} latest {}; "
                 "Audio current {} latest {}",
                 m_video_current_version, m_video_latest_version.load(),
                 m_audio_current_version, m_audio_latest_version.load());

    if (m_videoPktQ.PeekMarker())
    {
        outPkt = m_videoPktQ.PopValue();
        m_video_current_version = outPkt->version;

        constexpr AVRational ms_time_base = {1, 1000};
        m_frame_ms = std::chrono::milliseconds(av_rescale_q(1,
                                            outPkt->marker->frame_duration,
                                                            ms_time_base));
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

    m_log->trace("Pending DTS; audio {} video {}",
                 m_audioPktQ.PeekDts(), m_videoPktQ.PeekDts());

    open_container();
}

void OutputTS::mux(void)
{
    struct StreamState {
        int64_t pts{AV_NOPTS_VALUE};
        int64_t dts{AV_NOPTS_VALUE};
    };

    std::array<StreamState, 2> prev_state;

    for (;;)
    {
        {
            std::unique_lock<std::mutex> cv_lock(m_pktQ_mutex);
            m_pktQ_ready.wait(cv_lock, [this] {
                return !m_running || !m_videoPktQ.IsEmpty();
            });
        }

        if (m_videoPktQ.IsEmpty())
        {
            if (!m_running.load())
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

        // Non-monotonic Timestamp Protection
        auto& prev = prev_state[stream_id];

        if (prev.dts != AV_NOPTS_VALUE && pkt->dts <= prev.dts)
        {
            m_log->info("MUX [{}] DTS delta {} non-monotonic: "
                         "Fix: {} -> {}",
                         stream_id,
                         pkt->dts - prev.dts,
                         pkt->dts,
                         prev.dts + 1);

            pkt->dts = prev.dts + 1;
        }

        m_log->trace("MUX [id{:<2d} version:{}] pts:{:#018x} dts:{:#018x} "
                     "duration:{} size:{}",
                     stream_id, outPkt->version, pkt->pts, pkt->dts,
                     pkt->duration, pkt->size);

        StreamState state = StreamState {
            .pts = pkt->pts,
            .dts = pkt->dts
        };

        int ret = av_interleaved_write_frame(m_formatContext, pkt.get());
        if (ret < 0)
        {
            if (ret == AVERROR(EINVAL))
            {
                m_log->warn("DAMAGED: Mux rejected packet "
                            "id={} pts={} -> {} dts={} -> {}: {}",
                            stream_id, prev.pts, state.pts,
                            prev.dts, state.dts,
                            AVerr2str(ret));
            }
            else
            {
                m_log->error("DAMAGED: write frame stream {} failed: {}",
                             stream_id, AVerr2str(ret));
            }
        }
        else
        {
            prev_state[stream_id] = state;
        }
    }
}

int OutputTS::AddMarker(Marker&& marker, int64_t timestamp)
{
    Packet packet;

    int64_t marker_dts_pts = av_rescale_q(timestamp,
                                          TimeBase::Magewell,
                                          TimeBase::MPEG_TS);

    packet.marker = std::move(marker);
    packet.pkt = make_packet();
    packet.pkt->pts = marker_dts_pts;
    packet.pkt->dts = marker_dts_pts;

    int version;

    // Note: fetch_add returns the previous value, so incr it.
    if (marker.stream_id == AUDIO_STREAM_ID)
    {
        m_log->trace("AddMarker: Audio");
        version = packet.version =
                  m_audio_latest_version.fetch_add(1, std::memory_order_relaxed) + 1;
        m_audioPktQ.Push(std::move(packet));
    }
    else if (marker.stream_id == VIDEO_STREAM_ID)
    {
        m_log->trace("AddMarker: Video");
        version = packet.version =
                  m_video_latest_version.fetch_add(1, std::memory_order_relaxed) + 1;
        m_videoPktQ.Push(std::move(packet));
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
    int vidpool_used_1m;
    std::array<int, 5>  vidpool_used_5m  {};
    std::array<int, 10> vidpool_used_10m {};
    int                 vidpool_5m_idx   {0};
    int                 vidpool_10m_idx  {0};

    int vidpool_5m_max  {0};
    int vidpool_10m_max {0};

    std::chrono::seconds total_duration;
    std::chrono::steady_clock::time_point start_tm   = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point vidpool_tm = start_tm;
    std::chrono::steady_clock::time_point current_tm;
    int duration;

    for (;;)
    {
        VideoStream::Image image;

#ifdef LOG_ELAPSED
        chrono::steady_clock::time_point start
            = chrono::steady_clock::now();
#endif
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
#ifdef LOG_ELAPSED
        chrono::steady_clock::time_point end
            = chrono::steady_clock::now();

        auto dur = chrono::duration_cast<chrono::microseconds>
                   (end - start);

        if (dur > m_frame_ms)
            m_log->debug("Wait for image {}μs(>{}ms) queue size {}",
                         dur.count(), m_frame_ms.count(),
                         m_imageQ.size());
#endif
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
        }

#ifdef LOG_ELAPSED
        auto encode_start = chrono::steady_clock::now();
#endif

        int used = m_videoStream->AddImage(std::move(image));

#ifdef LOG_ELAPSED
        auto encode_end = chrono::steady_clock::now();
        auto encode_dur = chrono::duration_cast<chrono::microseconds>
                          (encode_end - encode_start);
        if (m_imageQ.size())
        {
            m_log->debug("Total AddImage {}us image q size {}",
                         encode_dur.count(), m_imageQ.size());
        }
#endif
        m_pktQ_ready.notify_one();

        {
            // Capture snapshots based on total high-water marks
            if (vidpool_used_1m < used)
                vidpool_used_1m = used;
            if (vidpool_used_5m[vidpool_5m_idx] < used)
                vidpool_used_5m[vidpool_5m_idx] = used;
            if (vidpool_used_10m[vidpool_10m_idx] < used)
                vidpool_used_10m[vidpool_10m_idx] = used;

            current_tm = std::chrono::steady_clock::now();
            duration = std::chrono::duration_cast<std::chrono::seconds>(current_tm - vidpool_tm).count();
            total_duration = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_tm);

            if (duration >= 60)
            {
                vidpool_5m_max = std::ranges::max(vidpool_used_5m);
                vidpool_10m_max = std::ranges::max(vidpool_used_10m);

                m_log->debug(format
                             ("GPU pool used 1m:{:<3} 5m:{:<3} 10m:{:<3} "
                              "of {:<3d} ({:%T} elapsed)",
                              vidpool_used_1m,
                              vidpool_5m_max,
                              vidpool_10m_max,
                              m_video_args.buffers,
                              total_duration));

                // Reset windows to clean baseline structures
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
    {
        std::scoped_lock lock(m_imageQ_mutex);

        if (!m_running.load())
        {
            f_image_avail(image.pImage, image.pEco);
            return;
        }

        m_imageQ.push_back(std::move(image));
    }

    m_imageQ_ready.notify_one();
}
