#pragma once

#include <vector>
#include <cstdint>
#include <optional>
#include <functional>

#define DEBUG_DUMP_FRAMES 1

#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>

#include "ffmpeg_types.h"

enum class DetectedCodec
{
    NONE,
    AC3,
    EAC3,
    AC4
};

struct DynamicStreamParams
{
    DetectedCodec codec = DetectedCodec::NONE;
    uint32_t sample_rate { 48000 };
    uint8_t channels { 2 };
    bool holds_atmos { false };

    bool operator==(const DynamicStreamParams& rhs) const
    {
        return codec == rhs.codec &&
               sample_rate == rhs.sample_rate &&
               channels == rhs.channels &&
               holds_atmos == rhs.holds_atmos;
    }

    bool operator!=(const DynamicStreamParams& rhs) const
    {
        return !(*this == rhs);
    }
};

class BitstreamAudioParser
{
public:
    using AccessUnitCallback = std::function<void(PacketPtr&& pkt)>;
    using ConfigChangeCallback =
        std::function<void(const DynamicStreamParams& new_params)>;

    BitstreamAudioParser(AccessUnitCallback au_callback,
                         ConfigChangeCallback config_callback);
    ~BitstreamAudioParser(void) = default;

    BitstreamAudioParser(const BitstreamAudioParser&) = delete;
    BitstreamAudioParser& operator=(const BitstreamAudioParser&) = delete;

    /**
     * @brief Consumes a moved payload vector, avoiding intermediate
     * copies into the stream cache.
     */
    void consumeBuffer(std::vector<uint8_t>&& buffer, uint64_t chunk_timestamp);

    /**
     * @brief Purges internal cached stream states upon line changes
     * or stream mutations.
     */
    void flush(void );

private:
    void processStream(void);
    void emitAccessUnit(const uint8_t* frame_ptr, size_t size);

    size_t parseEAC3Frame(size_t start_offset);
    size_t parseAC4Frame(size_t start_offset);

    // Structural helper functions added internally for validation
    size_t peekEAC3FrameSize(size_t start_offset, bool& out_swapped);
    size_t peekAC4FrameSize(size_t start_offset, bool& out_swapped);

    AccessUnitCallback m_au_callback;
    ConfigChangeCallback m_config_callback;

    std::vector<uint8_t> m_stream_cache;
    std::optional<DynamicStreamParams> m_current_params;

    uint64_t m_last_assigned_ts = 0;
    uint64_t m_current_chunk_ts = 0;
    bool m_first_frame_after_flush = true;

    // Internal states for stream stability and alignment matching
    std::vector<uint8_t> m_coalesced_frame_buffer;
    std::optional<bool>
    m_endianness_swapped; // Caches endianness layout once discovered
};

// ============================================================================
// spdlog / {fmt} Custom Formatter Specialization
// ============================================================================
template <>
struct fmt::formatter<DynamicStreamParams>
{
    constexpr auto parse(fmt::format_parse_context& ctx)
    -> decltype(ctx.begin())
    {
        auto it = ctx.begin(), end = ctx.end();
        if (it != end && *it != '}')
        {
            throw fmt::format_error("invalid format specifier for "
                                    "DynamicStreamParams");
        }
        return it;
    }

    template <typename FormatContext>
    auto format(const DynamicStreamParams& params,
                FormatContext& ctx) const -> decltype(ctx.out())
    {
        std::string_view codec_name = "UNKNOWN";
        switch (params.codec)
        {
        case DetectedCodec::AC3:
            codec_name = "AC-3";
            break;
        case DetectedCodec::EAC3:
            codec_name = "E-AC-3";
            break;
        case DetectedCodec::AC4:
            codec_name = "AC-4";
            break;
        case DetectedCodec::NONE:
            codec_name = "NONE";
            break;
        }

        return fmt::format_to
               (ctx.out(),
                "[Codec: {}, SampleRate: {} Hz, Channels: {}{}]",
                codec_name,
                params.sample_rate,
                params.channels,
                params.holds_atmos ? " (Dolby Atmos)" : ""
               );
    }
};