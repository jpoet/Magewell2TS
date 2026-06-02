#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <cstdint>
#include <concepts>

// Forward declaring standard FFmpeg types for template constraints
extern "C" {
#include <libavutil/rational.h>
}

namespace FFmpegHelper
{
    // Evaluates true if the object has a .timestamp member (e.g., raw
    // parsed audio frames)
    template<typename T>
    concept HasTimestamp = requires(T a)
        { { a.timestamp } -> std::convertible_to<uint64_t>; };

    // Evaluates true if the object is a pointer containing a ->dts
    // member (e.g., raw AVPacket*)
    template<typename T>
    concept HasPointerDts = requires(T a)
        { { a->dts } -> std::convertible_to<int64_t>; };

    // Evaluates true if the object has a direct .dts member (e.g.,
    // your custom Packet wrapper)
    template<typename T>
    concept HasDirectDts = requires(T a)
        { { a.dts } -> std::convertible_to<int64_t>; };

    // Evaluates true if the object has an .is_marker flag
    template<typename T>
    concept HasMarkerFlag = requires(T a)
        { { a.is_marker } -> std::convertible_to<bool>; };

    // Evaluates true if the object carries a snapshot of the time_base
    template<typename T>
      concept HasTimebase = requires(T a)
      {
          a.time_base.num;
          a.time_base.den;
      };
}

template <typename T>
class MediaQueue
{
public:
    MediaQueue() = default;
    ~MediaQueue() = default;

    // Disallow copies to protect queue structure across threads
    MediaQueue(const MediaQueue&) = delete;
    MediaQueue& operator=(const MediaQueue&) = delete;

    void Push(T&& value)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push(std::move(value));
        }
    }

    std::optional<T> PopValue()
    {
        std::unique_lock<std::mutex> lock(m_mutex);

        if (m_queue.empty() && m_isShutdown)
        {
            return std::nullopt;
        }

        T value = std::move(m_queue.front());
        m_queue.pop();
        return value;
    }

    bool IsEmpty() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.empty();
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

        const auto& item = m_queue.front();

        // Safe, compile-time dispatching for all types
        if constexpr (FFmpegHelper::HasDirectDts<T>)
        {
            return static_cast<int64_t>(item.dts);
        }
        return 0;
    }

    // New thread-safe inspection function to retrieve the packet
    // timebase snapshot natively
    AVRational PeekTimebase() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty())
        {
            return AVRational{0, 1};
        }

        const auto& item = m_queue.front();

        if constexpr (FFmpegHelper::HasTimebase<T>)
        {
            return item.time_base;
        }

        return AVRational{0,1}; // Safe chronological fallback baseline
    }

    // Thread-safe inspection function to support your muxing logic
    bool PeekMarker() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty())
        {
            return false;
        }

        if constexpr (FFmpegHelper::HasMarkerFlag<T>)
        {
            return m_queue.front().is_marker;
        }

        return false;
    }

    void Shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_isShutdown = true;
        }
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::queue<T> emptyQueue;
        std::swap(m_queue, emptyQueue);
        m_isShutdown = false;
    }

private:
    std::queue<T> m_queue;
    mutable std::mutex m_mutex;
    bool m_isShutdown{ false };
};
