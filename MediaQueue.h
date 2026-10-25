#pragma once


#include <queue>
#include <deque>

#include <mutex>
#include <optional>
#include <utility>
#include <cstdint>

#include "ffmpeg_types.h"

namespace TimeBase
{
inline constexpr AVRational MPEG_TS  {1, 90000};
inline constexpr AVRational Magewell {1, 10000000};
inline constexpr AVRational AUDIO48  {1, 48000};
inline constexpr AVRational MS       {1, 1000};
}

struct Marker
{
    int                   stream_id {-1};
    AVRational            time_base {0, 1};
    AVRational            frame_duration {0, 1};
    CodecParamsPtr        codec_par;
    SharedCodecContextPtr encoder;
};

struct Packet
{
    int                   version {0};
    PacketPtr             pkt;
    std::optional<Marker> marker;
};

class MediaQueue
{
  public:
    MediaQueue() = default;
    ~MediaQueue() = default;

    MediaQueue(const MediaQueue&) = delete;
    MediaQueue& operator=(const MediaQueue&) = delete;

    void Push(Packet&& value)
    {
        std::scoped_lock lock(m_mutex);
        m_queue.push(std::move(value));
    }

    std::optional<Packet> PopValue()
    {
        std::scoped_lock lock(m_mutex);

        if (m_queue.empty())
        {
            return std::nullopt;
        }

        Packet value = std::move(m_queue.front());
        m_queue.pop();
        return value;
    }

    bool IsEmpty() const
    {
        std::scoped_lock lock(m_mutex);
        return m_queue.empty();
    }

    size_t GetSize() const
    {
        std::scoped_lock lock(m_mutex);
        return m_queue.size();
    }

    int64_t PeekDts() const
    {
        std::scoped_lock lock(m_mutex);
        if (m_queue.empty() || !m_queue.front().pkt)
            return AV_NOPTS_VALUE;

        return m_queue.front().pkt->dts;
    }

    int64_t PeekPts() const
    {
        std::scoped_lock lock(m_mutex);
        if (m_queue.empty() || !m_queue.front().pkt)
            return AV_NOPTS_VALUE;

        return m_queue.front().pkt->pts;
    }

    AVRational PeekTimebase() const
    {
        std::scoped_lock lock(m_mutex);
        if (m_queue.empty() || !m_queue.front().marker)
            return AVRational{0, 1};

        return m_queue.front().marker->time_base;
    }

    bool PeekMarker() const
    {
        std::scoped_lock lock(m_mutex);
        if (m_queue.empty())
            return false;

        return m_queue.front().marker.has_value();
    }

    void Reset()
    {
        decltype(m_queue) emptyQueue;
        {
            std::scoped_lock lock(m_mutex);
            std::swap(m_queue, emptyQueue);
        }
    }

  private:
    std::queue<Packet> m_queue;
    mutable std::mutex m_mutex;
};
