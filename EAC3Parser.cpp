#include <array>
#include <format>

#include "EAC3Parser.h"
#include "BitReader.h"

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
    // bounds evaluation
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
    // Minimal safety envelope check for an AC3/EAC3 frame sync structure
    if (iec_buffer.size() < 12) [[unlikely]]
    {
        return std::nullopt;
    }

    BitReader br(iec_buffer);

    // Header Validation: Look for the universal 16-bit syncword (0x0B77)
    uint16_t syncword = br.getBits(16);
    if (syncword != 0x0B77) [[unlikely]]
    {
        return std::nullopt;
    }

    EAC3MetaData out{};
    out.is_atmos = false; // Initialize to default state
    uint32_t num_audio_blocks = 6; // Standard default context for AC-3

    if (codec_type == CodecType::AC3)
    {
        // --- AC-3 Bit Extraction Path ---
        out.codec = CodecType::AC3;
        out.strmtyp = 0;
        out.substreamid = 0;

        // Skip over the 16-bit crc1 field which natively follows the syncword
        br.skipBits(16);

        // Extract native legacy AC-3 bitstream parameters sequentially
        uint8_t fscod      = br.getBits(2);
        [[maybe_unused]] uint8_t frmsizecod = br.getBits(6);
        [[maybe_unused]] uint8_t bsid       = br.getBits(5);
        [[maybe_unused]] uint8_t bsmod      = br.getBits(3);
        uint8_t acmod      = br.getBits(3);

        // Track variable layout configuration skips
        if ((acmod & 0x01) && (acmod != 0x01))
            br.skipBits(2); // skip center mix levels (cmixlev)

        if (acmod & 0x04)
            br.skipBits(2); // skip surround mix levels (surmixlev)

        if (acmod == 0x02)
            br.skipBits(2); // skip dolby surround mode flags (dsurmod)

        // LFE flag
        bool lfeon = br.getBit();

        static constexpr uint32_t ac3_rates[] = { 48000, 44100, 32000, 0 };
        out.sample_rate_hz = ac3_rates[fscod];
        if (out.sample_rate_hz == 0) [[unlikely]]
        {
            return std::nullopt;
        }

        out.payload_size_bytes =
            getFrameSizeBytes(iec_buffer.subspan(0, 6), CodecType::AC3);

        m_base_channels.clear();
        m_extension_channels.clear();
        m_has_lfe = lfeon;
        append_acmod(acmod, m_base_channels);

        out.total_channels =
            static_cast<uint8_t>(m_base_channels.size() + (m_has_lfe ? 1 : 0));
        out.channel_layout = generate_layout();

        // Calculate frame presentation duration properties
        out.duration = 15360000000LL / out.sample_rate_hz;
        return out;
    }
    else
    {
        // --- Enhanced AC-3 (E-AC-3) Bit Extraction Path ---
        out.codec = CodecType::EAC3;

        uint8_t  strmtyp     = br.getBits(2);
        uint8_t  substreamid = br.getBits(3);
        uint16_t frmsiz      = br.getBits(11);
        uint8_t  fscod       = br.getBits(2);

        if (fscod == 3)
        {
            static constexpr uint32_t half_rates[] = { 24000, 22050, 16000, 0 };
            uint8_t fscod2 = br.getBits(2);
            out.sample_rate_hz = half_rates[fscod2];

            static constexpr uint32_t blocks_per_mode[] = { 1, 2, 3, 6 };
            uint8_t numblks_code = br.getBits(2);
            num_audio_blocks = blocks_per_mode[numblks_code];
        }
        else
        {
            static constexpr uint32_t norm_rates[] = { 48000, 44100, 32000, 0 };
            out.sample_rate_hz = norm_rates[fscod];

            uint8_t numblks_code = br.getBits(2);
            num_audio_blocks = (numblks_code == 3) ? 6 : (numblks_code + 1);
        }

        uint8_t acmod = br.getBits(3);
        bool    lfeon = br.getBit();

        if (out.sample_rate_hz == 0) [[unlikely]]
        {
            return std::nullopt;
        }

        out.substreamid = substreamid;
        out.strmtyp = strmtyp;
        out.payload_size_bytes = (frmsiz + 1) * 2;

        uint32_t total_samples = num_audio_blocks * 256;
        out.duration = (static_cast<int64_t>(total_samples) * 10000000LL) /
                       out.sample_rate_hz;

        m_base_channels.clear();
        m_extension_channels.clear();
        m_has_lfe = lfeon;

        // Process Stream Type Conditional Channel Mapping
        if (strmtyp == 0)
        {
            append_acmod(acmod, m_base_channels);
        }
        else if (strmtyp == 1)
        {
            bool chanmap_exists = br.getBit();
            if (chanmap_exists)
            {
                uint16_t mask = br.getBits(16);

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
                            m_has_lfe = true;
                        }
                        else
                        {
                            m_extension_channels.push_back(mask_labels[i]);
                        }
                    }
                }
            }
            else
            {
                append_acmod(acmod, m_extension_channels);
            }
        }

        // Finalize Metadata Generation
        out.total_channels = static_cast<uint8_t>(m_base_channels.size()
                                                  + m_extension_channels.size()
                                                  + (m_has_lfe ? 1 : 0));
        out.channel_layout = generate_layout();

        return out;
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
                       "  Speaker Layout: [ {} ] {}\n",
                       codec_str, stream_type_str, out.substreamid,
                       out.sample_rate_hz, out.payload_size_bytes,
                       out.total_channels, out.channel_layout,
                       (out.is_atmos ? "Atmos" : "")
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
    for (const auto& ch : m_base_channels)
    {
        s += ch + " ";
    }
    if (!m_extension_channels.empty())
    {
        s += "+ [ ";
        for (const auto& ch : m_extension_channels)
        {
            s += ch + " ";
        }
        s += "]";
    }
    if (m_has_lfe)
    {
        s += "+ LFE";
    }
    return s;
}
