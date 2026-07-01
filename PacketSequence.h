#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <format>
#include <cstdint>

#include "MediaQueue.h"

// Forward declarations or include headers matching your codebase
extern "C" {
#include <libavcodec/packet.h>
}


class PacketSequence
{
  private:
    struct DebugSnapshot
    {
        bool is_marker;
        int version;
        int64_t dts;
        AVRational time_base;
        bool is_disordered{false};

        // Appended Video Bitstream Properties
        bool is_keyframe{false};
        bool has_idr{false};
        bool has_sps{false};
        bool has_pps{false};
    };

    /**
     * @brief Helper to parse H.264/H.265 Annex B payloads for
     * specific NAL unit types.
     */
    static void ParseNalUnits(const AVPacket* av_pkt, bool& out_idr,
                              bool& out_sps, bool& out_pps)
    {
        out_idr = false;
        out_sps = false;
        out_pps = false;

        if (!av_pkt || !av_pkt->data || av_pkt->size < 4)
        {
            return;
        }

        const uint8_t* data = av_pkt->data;
        const int size = av_pkt->size;

        // Scan payload for 0x000001 or 0x00000001 Annex B boundaries
        for (int idx = 0; idx < size - 4; ++idx)
        {
            if (data[idx] == 0x00 && data[idx + 1] == 0x00)
            {
                int nal_index = -1;
                if (data[idx + 2] == 0x01)
                {
                    nal_index = idx + 3;    // 3-byte start code
                }
                else if (data[idx + 2] == 0x00 && data[idx + 3] == 0x01)
                {
                    nal_index = idx + 4;    // 4-byte start code
                }

                if (nal_index != -1 && nal_index < size)
                {
                    // Sniff the NAL unit type byte

                    // H.264 extraction format
                    uint8_t nal_type = data[nal_index] & 0x1F;

                    if (nal_type == 5)
                    {
                        out_idr = true;    // IDR Slice
                    }
                    else if (nal_type == 7)
                    {
                        out_sps = true;    // Sequence Parameter Set
                    }
                    else if (nal_type == 8)
                    {
                        out_pps = true;    // Picture Parameter Set
                    }

                    // Optional H.265 Sniffing support fallback:
                    // uint8_t h265_type = (data[nal_index] >> 1) & 0x3F;
                    // if (h265_type >= 16 && h265_type <= 21) out_idr = true;
                    // else if (h265_type == 32) out_sps = true;
                    // else if (h265_type == 33) out_pps = true;
                }
            }
        }
    }

  public:
    PacketSequence() = default;
    ~PacketSequence() = default;

    PacketSequence(const PacketSequence&) = delete;
    PacketSequence& operator=(const PacketSequence&) = delete;

    /**
     * @brief Pushes metadata and evaluates internal frame structure
     * flags safely via references.
     * @param pkt Custom wrapper packet container.
     * @param av_pkt Core raw reference pointer (usually pulled from
     * pkt.pkt or your active loop step).
     */
    bool Push(const Packet& pkt, const AVPacket* av_pkt)
    {
        std::scoped_lock lock(m_mutex);

        bool is_chronological = true;
        bool is_disordered = false;

        // Calculate Chronological DTS Consistency
        if (!pkt.is_marker && av_pkt->dts != 0)
        {
            for (auto it = m_history.rbegin(); it != m_history.rend(); ++it)
            {
                if (!it->is_marker && it->dts != 0)
                {
                    if (av_pkt->dts < it->dts)
                    {
                        is_chronological = false;
                        is_disordered = true;
                    }
                    break;
                }
            }
        }

        // Parse bitstream attributes out of raw container
        bool is_key = false;
        bool idr = false, sps = false, pps = false;

        if (av_pkt)
        {
            is_key = (av_pkt->flags & AV_PKT_FLAG_KEY) != 0;
            ParseNalUnits(av_pkt, idr, sps, pps);
        }

        // Commit snapshot to rolling history window
        m_history.push_back(DebugSnapshot {
                .is_marker = pkt.is_marker,
                .version = pkt.version,
                .dts = av_pkt->dts,
                .time_base = pkt.time_base,
                .is_disordered = is_disordered,
                .is_keyframe = is_key,
                .has_idr = idr,
                .has_sps = sps,
                .has_pps = pps
            });

        if (m_history.size() > MAX_HISTORY_SIZE)
        {
            m_history.pop_front();
        }

        return is_chronological;
    }

    /**
     * @brief Formats current trace tracking history for print dumps.
     */
    std::string DebugStr() const
    {
        std::scoped_lock lock(m_mutex);

        if (m_history.empty())
        {
            return "PacketSequence: [Empty]\n";
        }

        std::string result =
            std::format("PacketSequence History (Count: {}):\n",
                        m_history.size());
        result += "---------------------------------------------------------------------------------------\n";

        for (size_t idx = 0; idx < m_history.size(); ++idx)
        {
            const auto& current = m_history[idx];
            std::string delta_str = "N/A";

            if (current.is_marker || current.dts == 0)
            {
                delta_str = "[MARKER]";
            }
            else
            {
                for (size_t j = idx; j > 0; --j)
                {
                    const auto& prev = m_history[j - 1];
                    if (!prev.is_marker && prev.dts != 0)
                    {
                        int64_t delta = current.dts - prev.dts;
                        delta_str = std::format("{:+d}", delta);
                        break;
                    }
                }
            }

            // Generate a concise flag footprint string block
            // (e.g. "[K I S P]" or "[. . . .]")
            std::string flags_str =
                std::format("[{}{}{}{}]",
                            current.is_keyframe ? "K" : ".",
                            current.has_idr ? "I" : ".",
                            current.has_sps ? "S" : ".",
                            current.has_pps ? "P" : "."
                            );

            result +=
                std::format("[{:2d}] DTS: {:12d} | Delta: {:9} | "
                            "Flags: {:7} | Marker: {:5} | "
                            "Ver: {} | TB: {}/{}{}\n",
                            idx,
                            current.dts,
                            delta_str,
                            flags_str,
                            current.is_marker ? "TRUE" : "FALSE",
                            current.version,
                            current.time_base.num,
                            current.time_base.den,
                            current.is_disordered
                            ? "  <-- !!! DTS DROP/STALL DETECTED !!!"
                            : ""
                            );
        }

        result += "---------------------------------------------------------------------------------------\n";
        return result;
    }

    void Clear()
    {
        std::scoped_lock lock(m_mutex);
        m_history.clear();
    }

  private:
    static constexpr size_t MAX_HISTORY_SIZE = 128;
    std::deque<DebugSnapshot> m_history;
    mutable std::mutex m_mutex;
};
