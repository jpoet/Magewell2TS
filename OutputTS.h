#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <utility>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>
#include <chrono>

#include <spdlog/spdlog.h>
#ifdef SPDLOG_FMT_EXTERNAL
#include <fmt/format.h>
#else
#include <spdlog/fmt/bundled/format.h>
#endif

#include "MediaQueue.h"
#if 0
#include "PacketSequence.h"
#endif

#include "VideoStream.h"
#include "AudioStream.h"

class OutputTS
{
  public:
    using ShutdownCallback = std::function<void (void)>;

    enum ID {
        VIDEO_STREAM_ID = 0,
        AUDIO_STREAM_ID = 1
    };

    OutputTS(int verbose, bool isEco,
             VideoStream::Args&& video_args,
             ShutdownCallback shutdown,
             VideoStream::MagCallback image_buffer_avail);
    ~OutputTS(void);

    void Shutdown(void);

    void log_packet(std::string where, const AVPacket* pkt, int version);

    void setHaveAudio(void) { m_no_audio = false; }

    bool EncodeFrame(int stream_id, int version,
                     AVCodecContext* enc, AVFrame* frame);
    bool FlushPackets(int stream_id, int version, AVCodecContext* enc);

    uint GetVideoVersion(void) const { return m_video_current_version; }
    uint GetAudioVersion(void) const { return m_audio_current_version; }

    int AddMarker(Marker&& marker, int64_t timestamp);

    void AddAudioPkt(Packet&& pkt);
    void AddAudioSamples(AudioStream::Samples&& audio);
    void AddVideoImage(VideoStream::Image&& image);

  private:
    void sync_markers(void);
    void mux(void);
    bool queue_packets(int stream_id, int version,
                       AVCodecContext* enc,
                       MediaQueue& pktQ, bool flushing);
    void process_video(void);
    void process_audio(void);

    void optimize_mpegts(AVFormatContext* format_ctx);
    bool open_container(void);
    void close_container(void);

    // spdlog
    std::shared_ptr<spdlog::logger> m_log;
    int                     m_verbose;

    std::chrono::milliseconds m_frame_ms { 16 };

    std::shared_ptr<VideoStream> m_videoStream {nullptr};
    std::mutex            m_videoStream_mutex;

    std::optional<Packet> m_video_marker;
    std::optional<Packet> m_audio_marker;

    AVFormatContext* m_formatContext {nullptr};

    int64_t          m_last_dts      {0};

    MediaQueue       m_videoPktQ;
    MediaQueue       m_audioPktQ;

    VideoStream::imageque_t m_imageQ;
    AudioStream::audioque_t m_audioQ;

    bool                    m_no_audio     {true};
    VideoStream::Args       m_video_args;

    ShutdownCallback        f_shutdown;
    VideoStream::MagCallback f_image_avail;
    std::thread             m_mux_thread;
    std::thread             m_audio_thread;
    std::thread             m_video_thread;

    std::mutex              m_audio_pktQ_mutex;
    std::mutex              m_video_pktQ_mutex;
    std::mutex              m_pktQ_mutex;
    std::condition_variable m_pktQ_ready;

    std::mutex              m_imageQ_mutex;
    std::condition_variable m_imageQ_ready;
    std::condition_variable m_imageQ_empty;

    std::mutex              m_audioQ_mutex;
    std::condition_variable m_audioQ_ready;
    std::condition_variable m_audioQ_empty;

    std::atomic<bool>       m_running       {true};

    int                     m_video_current_version {0};
    int                     m_audio_current_version {0};
    std::atomic<int>        m_video_latest_version  {0};
    std::atomic<int>        m_audio_latest_version  {0};

#if 0
    PacketSequence m_sequence;
#endif
};
