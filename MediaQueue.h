#pragma once


#include <queue>
#include <deque>
#include <vector>
#include <algorithm>

#include <mutex>
#include <optional>
#include <utility>
#include <cstdint>
#include <condition_variable>
#include <concepts>

#include "ffmpeg_types.h"

struct Packet
{
    bool is_marker {false};
    int stream_id {-1};

    int version {0};

    AVRational time_base {0, 1};
    AVRational frame_duration {0, 1};

    PacketPtr pkt;
    CodecParamsPtr codec_par;
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
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(std::move(value));
    }

    std::optional<Packet> PopValue()
    {
        std::unique_lock<std::mutex> lock(m_mutex);

        if (m_queue.empty())
        {
            return std::nullopt;
        }

        Packet value = std::move(m_queue.front());
        m_queue.pop();
        return value;
    }

    bool IsEmpty()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // A bit of a hack, but it works.
        if (m_queue.empty())
        {
            return true;
        }
        /*
          If the next packet is market, it is probably followed by a
          discontinuity, which can have mixed up packets. Save the
          next six in that situation, so they can be sorted.
        */
        if (!m_queue.front().is_marker || m_queue.front().stream_id != 0)
        {
            return false;
        }
        if (m_queue.size() < 6)
        {
            return true;
        }
        FixUpDtsPts();
        return false;
    }

    size_t GetSize() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

    int64_t PeekDts() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty())
        {
            return 0;
        }

        return m_queue.front().pkt->dts;
    }

    AVRational PeekTimebase() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty())
        {
            return AVRational{0, 1};
        }

        return m_queue.front().time_base;
    }

    bool PeekMarker() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty())
        {
            return false;
        }

        return m_queue.front().is_marker;
    }

    void FixUpDtsPts(void)
    {
        /*
          The DTS of the 6 packets after a discontinuity can be
          messy. Attempt to fix them up.
        */

        // Must be called with > 2 elements and the mutex locked.

        // Only extract up to the first 6 packets for fixing
        std::vector<Packet> tmpVec;
        const size_t elementsToFix = 6;
        tmpVec.reserve(elementsToFix);

        for (size_t i = 0; i < elementsToFix; ++i)
        {
            tmpVec.push_back(std::move(m_queue.front()));
            m_queue.pop();
        }

        // Sort, skipping the marker and the keyframe
        std::sort(tmpVec.begin() + 2, tmpVec.end(),
                  [](const Packet& a, const Packet& b)
                  {
                      return a.pkt->dts < b.pkt->dts;
                  });

        // Calculate step size using the first valid packet's frame
        // rate and time_base
        int64_t dts_step = av_rescale_q(1,
                                        tmpVec[0].frame_duration,
                                        tmpVec[0].time_base);

        // Adjust the DTS within the "discontinuity" block backwards
        // from our anchor (Index 5)
        for (size_t i = tmpVec.size() - 1; i > 0; --i)
        {
            size_t current_idx = i - 1;
            size_t next_idx = i;

            // Calculate the original offset between this packet's PTS and DTS
            int64_t pts_dts_offset = tmpVec[current_idx].pkt->pts -
                                     tmpVec[current_idx].pkt->dts;

            // Assign the new clean DTS step
            tmpVec[current_idx].pkt->dts = tmpVec[next_idx].pkt->dts - dts_step;

            // Re-apply the original offset to the new DTS to get the
            // new correct PTS
            tmpVec[current_idx].pkt->pts = tmpVec[current_idx].pkt->dts +
                                           pts_dts_offset;
        }

        // Recombine everything back into m_queue in the exact correct order.
        // First, push the 6 freshly fixed packets back into the queue.
        for (Packet& pkt : tmpVec)
        {
            m_queue.push(std::move(pkt));
        }

        // Then, cycle the remaining old packets behind them to
        // restore the FIFO structure.
        size_t remainingElements = m_queue.size() - elementsToFix;
        for (size_t i = 0; i < remainingElements; ++i)
        {
            m_queue.push(std::move(m_queue.front()));
            m_queue.pop();
        }
    }

    void Shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_isShutdown = true;
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // This clean pattern works for swapping out BOTH types of containers
        decltype(m_queue) emptyQueue;
        std::swap(m_queue, emptyQueue);

        m_isShutdown = false;
    }

  private:
    std::queue<Packet> m_queue;
    mutable std::mutex m_mutex;
    bool m_isShutdown{ false };
};
