#include "BitstreamAudioParser.h"
#include <cstring>
#include <iterator>
#include <algorithm>
#include <fstream>

BitstreamAudioParser::BitstreamAudioParser(AccessUnitCallback au_callback,
        ConfigChangeCallback config_callback)
    : m_au_callback(std::move(au_callback))
    , m_config_callback(std::move(config_callback))
{
}

void BitstreamAudioParser::consumeBuffer(std::vector<uint8_t>&& buffer,
        uint64_t chunk_timestamp)
{
    if (buffer.empty())
    {
        return;
    }

    m_current_chunk_ts = chunk_timestamp;

    if (m_stream_cache.empty())
    {
        m_stream_cache = std::move(buffer);
    }
    else
    {
        m_stream_cache.insert(m_stream_cache.end(),
                              std::make_move_iterator(buffer.begin()),
                              std::make_move_iterator(buffer.end()));
    }

    processStream();
}

void BitstreamAudioParser::processStream(void)
{
    size_t offset = 0;
    size_t consecutive_unverified_syncs = 0;
    const size_t MAX_UNVERIFIED_SYNCS_BEFORE_PURGE = 3; // Aggressive recovery

    while (offset + 10 <= m_stream_cache.size())
    {
        uint8_t b0 = m_stream_cache[offset];
        uint8_t b1 = m_stream_cache[offset + 1];

        bool is_eac3_sync = ((b0 == 0x77 && b1 == 0x0B) ||
                             (b0 == 0x0B && b1 == 0x77));
        bool is_ac4_sync = ((b0 == 0x40 && b1 == 0xAC) ||
                            (b0 == 0xAC && b1 == 0x40));

        // Strict Codec Lockout: If we have already identified a
        // codec, ignore syncwords for the other type
        if (m_current_params.has_value()
                && m_current_params->codec != DetectedCodec::NONE)
        {
            if (m_current_params->codec == DetectedCodec::EAC3
                    || m_current_params->codec == DetectedCodec::AC3)
            {
                is_ac4_sync = false;
            }
            else if (m_current_params->codec == DetectedCodec::AC4)
            {
                is_eac3_sync = false;
            }
        }

        if (is_eac3_sync || is_ac4_sync)
        {
            size_t frame_size = 0;
            bool current_frame_swapped = false;

            if (is_eac3_sync)
            {
                frame_size = peekEAC3FrameSize(offset, current_frame_swapped);
            }
            else
            {
                frame_size = peekAC4FrameSize(offset, current_frame_swapped);
            }

            // Reject invalid size or obvious overflow structures
            if (frame_size == 0 || frame_size > 16384)
            {
                ++offset;
                continue;
            }

            // --- STRICT BOUNDARY LOOK-AHEAD VERIFICATION ---
            size_t next_frame_offset = offset + frame_size;
            bool is_boundary_verified = false;

            if (next_frame_offset + 2 <= m_stream_cache.size())
            {
                uint8_t n0 = m_stream_cache[next_frame_offset];
                uint8_t n1 = m_stream_cache[next_frame_offset + 1];

                if (is_eac3_sync)
                {
                    if ((n0 == 0x77 && n1 == 0x0B) ||
                        (n0 == 0x0B && n1 == 0x77))
                    {
                        is_boundary_verified = true;
                    }
                }
                else
                {
                    if ((n0 == 0x40 && n1 == 0xAC) ||
                        (n0 == 0xAC && n1 == 0x40))
                    {
                        is_boundary_verified = true;
                    }
                }
            }
            else
            {
                // Incomplete stream cache window edge. Break out and
                // wait for more data.
                break;
            }

            if (is_boundary_verified)
            {
                consecutive_unverified_syncs = 0;

                if (!m_endianness_swapped.has_value())
                {
                    m_endianness_swapped = current_frame_swapped;
                    spdlog::info("Audio bitstream layout locked. "
                                 "Endianness Swapped = {}",
                                 m_endianness_swapped.value());
                }

                size_t consumed = 0;
                if (is_eac3_sync)
                {
                    consumed = parseEAC3Frame(offset);
                }
                else
                {
                    consumed = parseAC4Frame(offset);
                }

                if (consumed == 0)
                {
                    break;
                }
                offset += consumed;
            }
            else
            {
                ++consecutive_unverified_syncs;

                // If we've already locked a layout format and start
                // hitting garbage, the hardware link has dropped out
                // or stalled. Clear cache to reset.
                if (m_current_params.has_value()
                        && consecutive_unverified_syncs >=
                    MAX_UNVERIFIED_SYNCS_BEFORE_PURGE)
                {
                    spdlog::warn("Stream boundary sync lost during "
                                 "capture stall. Purging stream cache window.");
                    m_stream_cache.clear();
                    m_coalesced_frame_buffer.clear();
                    return;
                }

                ++offset;
            }
        }
        else
        {
            ++offset;
        }
    }

    if (offset > 0)
    {
        m_stream_cache.erase(m_stream_cache.begin(),
                             m_stream_cache.begin() + offset);
    }
}

size_t BitstreamAudioParser::peekEAC3FrameSize(size_t start_offset,
        bool& out_swapped)
{
    if (start_offset + 6 > m_stream_cache.size())
    {
        return 0;
    }

    uint8_t b0 = m_stream_cache[start_offset];
    out_swapped = (b0 == 0x77);

    uint16_t frmsiz = 0;
    uint8_t bsid = 0;

    if (out_swapped)
    {
        frmsiz = ((m_stream_cache[start_offset + 3] & 0x07) << 8) |
                 m_stream_cache[start_offset + 2];
        bsid = m_stream_cache[start_offset + 4] >> 3;
    }
    else
    {
        frmsiz = ((m_stream_cache[start_offset + 2] & 0x07) << 8) |
                 m_stream_cache[start_offset + 3];
        bsid = m_stream_cache[start_offset + 5] >> 3;
    }

    if (bsid > 10)
    {
        return (static_cast<size_t>(frmsiz) + 1) * 2;
    }
    else
    {
        static const uint16_t ac3_words[] =
        {
            32,32,40,40,48,48,56,56,64,64,80,80,96,96,112,112,128,128,
            160,160,192,192,224,224,256,256,320,320,384,384,448,448,
            512,512,576,576,640,640
        };
        uint8_t frmsizecod = frmsiz & 0x3F;
        if (frmsizecod < 38)
        {
            return static_cast<size_t>(ac3_words[frmsizecod]) * 2;
        }
    }
    return 0;
}

size_t BitstreamAudioParser::peekAC4FrameSize(size_t start_offset,
        bool& out_swapped)
{
    if (start_offset + 7 > m_stream_cache.size())
    {
        return 0;
    }

    uint8_t b0 = m_stream_cache[start_offset];
    uint8_t b1 = m_stream_cache[start_offset + 1];

    // 0x40AC is swapped, 0xAC40 is normal big-endian layout
    out_swapped = (b0 == 0x40 && b1 == 0xAC);

    uint32_t raw_size = 0;
    if (out_swapped)
    {
        raw_size = (static_cast<uint32_t>(m_stream_cache[start_offset + 2]) << 16) |
                   (static_cast<uint32_t>(m_stream_cache[start_offset + 3]) << 8) |
                   m_stream_cache[start_offset + 4];
    }
    else
    {
        raw_size = (static_cast<uint32_t>(m_stream_cache[start_offset + 3]) << 16) |
                   (static_cast<uint32_t>(m_stream_cache[start_offset + 2]) << 8) |
                   m_stream_cache[start_offset + 5];
    }

    size_t size_val = raw_size & 0xFFFF;
    if (size_val == 0xFFFF)
    {
        if (out_swapped)
        {
            size_val = (static_cast<size_t>(m_stream_cache[start_offset + 5]) << 8) |
                       m_stream_cache[start_offset + 6];
        }
        else
        {
            size_val = (static_cast<size_t>(m_stream_cache[start_offset + 6]) << 8) |
                       m_stream_cache[start_offset + 7];
        }
        return size_val + 7;
    }
    return size_val + 5;
}

size_t BitstreamAudioParser::parseEAC3Frame(size_t start_offset)
{
    bool swap_detected = false;
    size_t frame_size = peekEAC3FrameSize(start_offset, swap_detected);

    if (start_offset + frame_size > m_stream_cache.size())
    {
        return 0;
    }

    // Read parameters branchless using our cached locked endian state configuration
    bool swapped = m_endianness_swapped.value_or(swap_detected);

    uint8_t strmtyp = 0;
    uint8_t acmod = 0;
    uint8_t lfeon = 0;
    uint8_t fscod = 0;
    uint8_t bsid = 0;

    if (swapped)
    {
        strmtyp = (m_stream_cache[start_offset + 3] >> 6) & 3;
        acmod = (m_stream_cache[start_offset + 5] >> 3) & 7;
        lfeon = (m_stream_cache[start_offset + 5] >> 2) & 1;
        fscod = (m_stream_cache[start_offset + 5] >> 6) & 3;
        bsid = m_stream_cache[start_offset + 4] >> 3;
    }
    else
    {
        strmtyp = (m_stream_cache[start_offset + 2] >> 6) & 3;
        acmod = (m_stream_cache[start_offset + 4] >> 3) & 7;
        lfeon = (m_stream_cache[start_offset + 4] >> 2) & 1;
        fscod = (m_stream_cache[start_offset + 4] >> 6) & 3;
        bsid = m_stream_cache[start_offset + 5] >> 3;
    }

    bool is_eac3 = (bsid > 10);
    static const uint8_t chans[] = {2, 1, 2, 3, 3, 4, 4, 5};

    if (!is_eac3 || strmtyp == 0)
    {
        // Independent boundary. Output complete accumulated chunk
        if (!m_coalesced_frame_buffer.empty())
        {
            emitAccessUnit(m_coalesced_frame_buffer.data(),
                           m_coalesced_frame_buffer.size());
            m_coalesced_frame_buffer.clear();
        }

        DynamicStreamParams params;
        params.codec = is_eac3 ? DetectedCodec::EAC3 : DetectedCodec::AC3;
        params.sample_rate = 48000;
        if (is_eac3)
        {
            if (fscod == 1)
            {
                params.sample_rate = 44100;
            }
            else if (fscod == 2)
            {
                params.sample_rate = 32000;
            }
        }
        params.channels = chans[acmod & 7] + (lfeon & 1);
        params.holds_atmos = false;

        if (is_eac3 && frame_size >= 10)
        {
            uint8_t b8 = swapped ? m_stream_cache[start_offset + 9] :
                         m_stream_cache[start_offset + 8];
            uint8_t b9 = swapped ? m_stream_cache[start_offset + 8] :
                         m_stream_cache[start_offset + 9];
            if ((b8 & 0x10) || ((b9 & 0x04) && (acmod == 7)))
            {
                params.holds_atmos = true;
            }
        }

        if (m_config_callback && (!m_current_params ||
                                  *m_current_params != params))
        {
            m_current_params = params;
            m_config_callback(*m_current_params);
        }

        m_coalesced_frame_buffer.insert(m_coalesced_frame_buffer.end(),
                                        &m_stream_cache[start_offset],
                                        &m_stream_cache[start_offset + frame_size]);
    }
    else if (strmtyp == 1)
    {
        if (!m_coalesced_frame_buffer.empty())
        {
            m_coalesced_frame_buffer.insert(m_coalesced_frame_buffer.end(),
                                            &m_stream_cache[start_offset],
                                            &m_stream_cache[start_offset + frame_size]);
        }
    }

    return frame_size;
}

size_t BitstreamAudioParser::parseAC4Frame(size_t start_offset)
{
    bool swap_detected = false;
    size_t frame_size = peekAC4FrameSize(start_offset, swap_detected);

    if (start_offset + frame_size > m_stream_cache.size())
    {
        return 0;
    }

    // Safely emit any lingering E-AC-3 frames before shifting down into AC-4
    if (!m_coalesced_frame_buffer.empty())
    {
        emitAccessUnit(m_coalesced_frame_buffer.data(),
                       m_coalesced_frame_buffer.size());
        m_coalesced_frame_buffer.clear();
    }

    DynamicStreamParams params;
    params.codec = DetectedCodec::AC4;
    params.sample_rate = 48000;
    params.channels = 6;
    params.holds_atmos = true;

    if (m_config_callback && (!m_current_params || *m_current_params != params))
    {
        m_current_params = params;
        m_config_callback(*m_current_params);
    }

    emitAccessUnit(&m_stream_cache[start_offset], frame_size);
    return frame_size;
}

void BitstreamAudioParser::emitAccessUnit(const uint8_t* frame_ptr, size_t size)
{
    if (size == 0)
    {
        return;
    }

#if DEBUG_DUMP_FRAMES
    std::ofstream dump_file("frame-audio.bin",
                            std::ios::binary | std::ios::app);
    if (dump_file.is_open())
    {
        dump_file.write(reinterpret_cast<const char*>(frame_ptr), size);
        dump_file.close();
    }
#endif

    if (!m_au_callback)
    {
        return;
    }

    PacketPtr pkt = make_packet(static_cast<int>(size));
    if (pkt)
    {
        std::memcpy(pkt->data, frame_ptr, size);

        int64_t duration = (1536 * 10000000LL) / (m_current_params ?
                           m_current_params->sample_rate : 48000);
        m_last_assigned_ts = (m_last_assigned_ts >= m_current_chunk_ts) ?
                             (m_last_assigned_ts + duration) : m_current_chunk_ts;

        pkt->pts = pkt->dts = m_last_assigned_ts;
        pkt->duration = duration;

        m_au_callback(std::move(pkt));
    }
}

void BitstreamAudioParser::flush(void)
{
    m_stream_cache.clear();
    m_coalesced_frame_buffer.clear();
    m_current_params.reset();
    m_first_frame_after_flush = true;

    // Clear alignment memories on pipeline alterations
    m_endianness_swapped.reset();
}
