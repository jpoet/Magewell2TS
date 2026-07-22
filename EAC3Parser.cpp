#include <array>
#include <format>

#include "EAC3Parser.h"

EAC3Parser::EAC3Parser(void)
{
    m_log = spdlog::get("app_logger");
    if (!m_log)
    {
        std::cerr << "OutputTS Error: Logger 'app_logger' not found!"
                  << std::endl;
        return;
    }
}

size_t EAC3Parser::getFrameSizeBytes(std::span<const uint8_t> raw_bytes, CodecType codec_type)
{
    // Fast bounds safety evaluation
    if (raw_bytes.size() < 6) [[unlikely]]
        return 0;

    uint16_t syncword = (static_cast<uint16_t>(raw_bytes[0]) << 8)
                        | raw_bytes[1];
    if (syncword != 0x0B77) [[unlikely]]
    {
        return 0;
    }

    if (codec_type != CodecType::AC3)
    {
        // E-AC-3 sizing footprint
        uint16_t frmsiz = (static_cast<uint16_t>(raw_bytes[2] & 0x07) << 8)
                          | raw_bytes[3];
        return (frmsiz + 1) * 2;
    }

    // Standard AC-3 Path
    uint8_t swapped_4 = raw_bytes[4];
    uint8_t fscod = (swapped_4 >> 6) & 0x03;
    uint8_t frmsizecod = swapped_4 & 0x3F;

    if (fscod == 3 || frmsizecod >= 38) [[unlikely]]
        return 0;

    static constexpr uint16_t ac3_words_flat[38] = {
         32,  32,  40,  40,  48,  48,  56,  56,  64,  64,
         80,  80,  96,  96, 112, 112, 128, 128, 160, 160,
        192, 192, 224, 224, 256, 256, 320, 320, 384, 384,
        448, 448, 512, 512, 576, 576, 640, 640
    };

    uint16_t words = ac3_words_flat[frmsizecod];

    // Branchless conditional add for asymmetric 44.1kHz allocations
    words += (fscod == 1) & (frmsizecod & 1);

    return words * 2;
}

std::optional<EAC3MetaData>
EAC3Parser::processFrame(std::span<const uint8_t> iec_buffer,
                         CodecType codec_type)
{
    if (iec_buffer.size() < 12) [[unlikely]]
    {
        return std::nullopt;
    }

    // Bit stream loader (Load 8 bytes natively into a 64-bit register)
    // By processing the buffer in Big-Endian, we can pull consecutive
    // bit fields via direct bit shifts.
    uint64_t cache;
    std::memcpy(&cache, iec_buffer.data(), 8);
    cache = __builtin_bswap64(cache); // Shifts to match big-endian field layouts

    // Header Validation
    if ((cache >> 48) != 0x0B77) [[unlikely]]
        return std::nullopt;

    EAC3MetaData out{};
    uint32_t num_audio_blocks = 6;

    if (codec_type == CodecType::AC3)
    {
        // --- AC-3 Bit Extraction Path ---
        out.codec = CodecType::AC3;
        out.strmtyp = 0;
        out.substreamid = 0;

        // Shift down from the correct byte positions in the 64-bit
        // big-endian register
        uint8_t fscod       = (cache >> 30) & 0x03; // Byte 4, top 2 bits
        uint8_t acmod       = (cache >> 13) & 0x07; // Byte 6, top 3 bits

        // Tracks bit location in 'cache'. Starts right after acmod
        // field (bit 13).
        size_t bit_offset = 13;

        if ((acmod & 0x01) && (acmod != 0x01))
            bit_offset -= 2; // skip center mix levels (cmixlev)
        if (acmod & 0x04)
            bit_offset -= 2; // skip surround mix levels (surmixlev)
        if (acmod == 0x02)
            bit_offset -= 2; // skip dolby surround mode flags (dsurmod)

        // Extract the 1-bit LFE flag right after the skipped fields
        uint8_t lfeon = (cache >> (bit_offset - 1)) & 0x01;

        static constexpr uint32_t ac3_rates[] = { 48000, 44100, 32000, 0 };
        out.sample_rate_hz = ac3_rates[fscod];
        if (out.sample_rate_hz == 0) [[unlikely]]
        {
            return std::nullopt;
        }

        out.payload_size_bytes =
            getFrameSizeBytes(iec_buffer.subspan(0, 6), CodecType::AC3);

        base_channels.clear();
        extension_channels.clear();
        has_lfe = (lfeon == 1);
        append_acmod(acmod, base_channels);

        out.total_channels =
            static_cast<uint8_t>(base_channels.size() + (has_lfe ? 1 : 0));
        out.channel_layout = generate_layout();

        // 1536LL * 10000000LL = 15360000000LL
        out.duration = 15360000000LL / out.sample_rate_hz;
        return out;
    }
    else
    {
        // --- Enhanced AC-3 (E-AC-3) Bit Extraction Path ---
        out.codec = CodecType::EAC3;

        uint8_t strmtyp     = (cache >> 46) & 0x03;
        uint8_t substreamid = (cache >> 43) & 0x07;
        uint16_t frmsiz     = (cache >> 32) & 0x07FF;
        uint8_t fscod       = (cache >> 30) & 0x03;

        uint32_t active_sample_rate = 0;

        if (fscod == 3)
        {
            uint8_t fscod2      = (cache >> 28) & 0x03;
            uint8_t numblks_code = (cache >> 26) & 0x03;

            static constexpr uint32_t blocks_per_mode[] = { 1, 2, 3, 6 };
            num_audio_blocks = blocks_per_mode[numblks_code];

            static constexpr uint32_t half_rates[] = { 24000, 22050, 16000, 0 };
            active_sample_rate = half_rates[fscod2];

            // Advance variable tracker past fscod == 3 variations
            [[maybe_unused]] uint8_t acmod = (cache >> 23) & 0x07;
            [[maybe_unused]] uint8_t lfeon = (cache >> 22) & 0x01;
            out.sample_rate_hz = active_sample_rate;
            // Capture these for channel matrix lookups later
            out.strmtyp = strmtyp;
            out.substreamid = substreamid;
        }
        else
        {
            uint8_t numblks_code = (cache >> 28) & 0x03;
            num_audio_blocks = (numblks_code == 3) ? 6 : (numblks_code + 1);

            static constexpr uint32_t norm_rates[] = { 48000, 44100, 32000, 0 };
            active_sample_rate = norm_rates[fscod];

            [[maybe_unused]] uint8_t acmod = (cache >> 25) & 0x07;
            [[maybe_unused]] uint8_t lfeon = (cache >> 24) & 0x01;
            out.sample_rate_hz = active_sample_rate;
        }

        if (out.sample_rate_hz == 0) [[unlikely]]
        {
            return std::nullopt;
        }

        // Re-extract unified positioning variables past the rate
        // calculation matrix. We know exactly where acmod and lfeon
        // sit depending on fscod
        size_t acmod_shift = (fscod == 3) ? 23 : 25;
        uint8_t acmod = (cache >> acmod_shift) & 0x07;
        uint8_t lfeon = (cache >> (acmod_shift - 1)) & 0x01;

        out.substreamid = substreamid;
        out.strmtyp = strmtyp;
        out.payload_size_bytes = (frmsiz + 1) * 2;

        uint32_t total_samples = num_audio_blocks * 256;
        out.duration = (static_cast<int64_t>(total_samples) * 10000000LL) /
                       out.sample_rate_hz;

        if (strmtyp == 0)
        {
            base_channels.clear();
            extension_channels.clear();
            has_lfe = (lfeon == 1);

            append_acmod(acmod, base_channels);
            out.total_channels = static_cast<uint8_t>(base_channels.size() +
                                                      (has_lfe ? 1 : 0));
            out.channel_layout = generate_layout();
            return out;
        }
        else if (strmtyp == 1)
        {
            extension_channels.clear();
            if (lfeon) has_lfe = true;

            // Determine the exact bit position inside the 'cache' register
            // where 'chanmap_exists' resides (immediately below lfeon)
            size_t chanmap_bit_pos = (fscod == 3) ? 21 : 23;

            bool chanmap_exists = ((cache >> chanmap_bit_pos) & 0x01) == 1;

            if (chanmap_exists)
            {
                // The 16-bit channel map sits directly below the chanmap_exists bit
                uint16_t mask = (cache >> (chanmap_bit_pos - 16)) & 0xFFFF;

                // Fixed bit mask alignment to properly map sequential flags per ATSC A/52
                static constexpr uint16_t mask_bits[] = {
                    0x8000, 0x4000, 0x2000, 0x1000, 0x0800, 0x0400,
                    0x0200, 0x0100, 0x0080, 0x0040, 0x0020, 0x0010
                };
                static const char* const mask_labels[] = {
                    "Lp", "Cp", "Rp", "Ls", "Rs", "Lc",
                    "Rc", "Lrs", "Rrs", "Lw", "Rw", "LFE"
                };

                for (size_t i = 0; i < 12; ++i)
                {
                    if (mask & mask_bits[i])
                    {
                        if (mask_bits[i] == 0x0010) // 12th bit is LFE
                        {
                            has_lfe = true;
                        }
                        else
                        {
                            extension_channels.push_back(mask_labels[i]);
                        }
                    }
                }
            }
            else
            {
                append_acmod(acmod, extension_channels);
            }

            out.total_channels = static_cast<uint8_t>(base_channels.size()
                                              + extension_channels.size()
                                              + (has_lfe ? 1 : 0));
            out.channel_layout = generate_layout();
            return out;
        }
    }

    return std::nullopt;
}

std::string EAC3Parser::formatOutput(const EAC3MetaData& out)
{
    std::string codec_str = (out.codec == CodecType::AC3)
                            ? "AC-3 (Dolby Digital)"
                            : "E-AC-3 (Dolby Digital Plus)";
    std::string stream_type_str = (out.strmtyp == 0)
                                  ? "Independent" : "Dependent";

    return std::format("--- {} ---\n"
                       "  Stream Type:    {}\n"
                       "  Substream ID:   {}\n"
                       "  Sample Rate:    {} Hz\n"
                       "  Payload Size:   {} Bytes\n"
                       "  Total Channels: {}\n"
                       "  Speaker Layout: [ {} ]\n",
                       codec_str, stream_type_str, out.substreamid,
                       out.sample_rate_hz, out.payload_size_bytes,
                       out.total_channels, out.channel_layout
                       );
}

void EAC3Parser::append_acmod(uint8_t acmod, std::vector<std::string>& target)
{
    switch (acmod)
    {
        case 0:
          target.push_back("L");
          target.push_back("R");
          break;
        case 1:
          target.push_back("C");
          break;
        case 2:
          target.push_back("L");
          target.push_back("R");
          break;
        case 3:
          target.push_back("L");
          target.push_back("C");
          target.push_back("R");
          break;
        case 4:
          target.push_back("L");
          target.push_back("R");
          target.push_back("S");
          break;
        case 5:
          target.push_back("L");
          target.push_back("C");
          target.push_back("R");
          target.push_back("S");
          break;
        case 6:
          target.push_back("L");
          target.push_back("R");
          target.push_back("Ls");
          target.push_back("Rs");
          break;
        case 7:
          target.push_back("L");
          target.push_back("C");
          target.push_back("R");
          target.push_back("Ls");
          target.push_back("Rs");
          break;
    }
}

std::string EAC3Parser::generate_layout() const
{
    std::string s = "";
    for (const auto& ch : base_channels)
    {
        s += ch + " ";
    }
    if (!extension_channels.empty())
    {
        s += "+ [ ";
        for (const auto& ch : extension_channels)
        {
            s += ch + " ";
        }
        s += "]";
    }
    if (has_lfe)
    {
        s += "+ LFE";
    }
    return s;
}
