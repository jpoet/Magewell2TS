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

size_t
  EAC3Parser::getFrameSizeBytes(std::span<const uint8_t> raw_bytes,
                                  CodecType codec_type)
{
        if (raw_bytes.size() < 6)
        {
            return 0;
        }

        uint16_t syncword = (static_cast<uint16_t>(raw_bytes[1]) << 8) |
                            raw_bytes[0];
        if (syncword != 0x0B77)
        {
            return 0;
        }

        uint8_t swapped_2 = raw_bytes[3];
        uint8_t swapped_3 = raw_bytes[2];
        uint8_t swapped_4 = raw_bytes[5];

        if (codec_type == CodecType::AC3)
        {
            // --- Standard AC-3 Sizing Path ---
            uint8_t fscod = (swapped_4 >> 6) & 0x03;
            uint8_t frmsizecod = swapped_4 & 0x3F;

            if (fscod == 3 || frmsizecod >= 38)
            {
                return 0;
            }

            // ATSC A/52 Table 5.18 Words-per-frame mapping (rows:
            // frmsizecod, cols: fscod)
            static constexpr uint16_t ac3_words_table[38][3] =
                {
                    {32,32,32}, {32,32,32}, {40,40,40},
                    {40,40,40}, {48,48,48}, {48,48,48},
                    {56,56,56}, {56,56,56}, {64,64,64},
                    {64,64,64}, {80,80,80}, {80,80,80},
                    {96,96,96}, {96,96,96}, {112,112,112},
                    {112,112,112}, {128,128,128}, {128,128,128},
                    {160,160,160}, {160,160,160}, {192,192,192},
                    {192,192,192}, {224,224,224}, {224,224,224},
                    {256,256,256}, {256,256,256}, {320,320,320},
                    {320,320,320}, {384,384,384}, {384,384,384},
                    {448,448,448}, {448,448,448}, {512,512,512},
                    {512,512,512}, {576,576,576}, {576,576,576},
                    {640,640,640}, {640,640,640}
                };

            uint16_t words = ac3_words_table[frmsizecod][fscod];

            if (fscod == 1 && (frmsizecod & 1))
            {
                words += 1;
            }

            return words * 2;
        }
        else
        {
            // --- Enhanced AC-3 Sizing Path ---
            uint16_t frmsiz = (static_cast<uint16_t>(swapped_2 & 0x07) << 8) |
                              swapped_3;
            return (frmsiz + 1) * 2;
        }
}

std::optional<EAC3MetaData>
  EAC3Parser::processFrame(std::span<const uint8_t>iec_buffer,
                          CodecType codec_type)
{
        if (iec_buffer.size() < 12)
        {
            return std::nullopt;
        }

        std::array<uint8_t, 12> head{};
        for (size_t i = 0; i < 12; i += 2)
        {
            head[i] = iec_buffer[i + 1];
            head[i + 1] = iec_buffer[i];
        }

        spdlog::info("head = {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                     head[0], head[1], head[2],
                     head[3], head[4], head[5]);

        size_t bit_pos = 0;
        auto read_bits = [&](size_t count) -> uint32_t
        {
            uint32_t val = 0;
            for (size_t i = 0; i < count; ++i)
            {
                size_t b_idx = bit_pos / 8;
                size_t bit_idx = 7 - (bit_pos % 8);
                uint8_t bit = (head[b_idx] >> bit_idx) & 1;
                val = (val << 1) | bit;
                bit_pos++;
            }
            return val;
        };

        if (read_bits(16) != 0x0B77)
        {
            return std::nullopt;
        }

        EAC3MetaData out { };

        if (codec_type == CodecType::AC3)
        {
            // --- Standard AC-3 Decoder Path ---
            out.codec = CodecType::AC3;
            out.strmtyp = 0;
            out.substreamid = 0;

            uint8_t fscod = read_bits(2);
            [[maybe_unused]] uint8_t frmsizecod = read_bits(6);
            [[maybe_unused]] uint8_t bsid_read = read_bits(5);
            [[maybe_unused]] uint8_t bsmod = read_bits(3);
            uint8_t acmod = read_bits(3);

            uint8_t lfeon = 0;
            if ((acmod & 0x01) && (acmod != 0x01))
            {
                read_bits(2);
            }
            if (acmod & 0x04)
            {
                read_bits(2);
            }
            if (acmod == 0x02)
            {
                read_bits(2);
            }
            lfeon = read_bits(1);

            static constexpr uint32_t ac3_rates[] = { 48000, 44100, 32000, 0 };
            out.sample_rate_hz = ac3_rates[fscod];

            out.payload_size_bytes =
                getFrameSizeBytes(iec_buffer.subspan(0, 6), CodecType::AC3);

            base_channels.clear();
            extension_channels.clear();
            has_lfe = (lfeon == 1);
            append_acmod(acmod, base_channels);

            out.total_channels = static_cast<uint8_t>(base_channels.size() +
                                                      (has_lfe ? 1 : 0));
            out.channel_layout = generate_layout();
            return out;
        }
        else
        {
            // --- Enhanced AC-3 Decoder Path ---
            out.codec = CodecType::EAC3;
            uint8_t strmtyp = read_bits(2);
            uint8_t substreamid = read_bits(3);
            uint16_t frmsiz = read_bits(11);

            uint8_t fscod = read_bits(2);
            if (fscod == 3)
            {
                uint8_t fscod2 = read_bits(2);
                read_bits(2);
                static constexpr uint32_t half_rates[] =
                    { 24000, 22050, 16000, 0 };
                active_sample_rate = half_rates[fscod2 & 0x03];
            }
            else
            {
                read_bits(2);
                static constexpr uint32_t norm_rates[] =
                    { 48000, 44100, 32000 };
                active_sample_rate = norm_rates[fscod];
            }

            uint8_t acmod = read_bits(3);
            uint8_t lfeon = read_bits(1);

            out.substreamid = substreamid;
            out.strmtyp = strmtyp;
            out.sample_rate_hz = active_sample_rate;
            out.payload_size_bytes = (frmsiz + 1) * 2;

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
                bool chanmap_exists = (read_bits(1) == 1);
                if (chanmap_exists)
                {
                    uint16_t mask = read_bits(16);
                    static const std::pair<uint16_t, std::string> bits[] =
                        {
                            {0x8000,"Lp"}, {0x4000,"Cp"}, {0x2000,"Rp"},
                            {0x1000,"Ls"}, {0x0800,"Rs"}, {0x0400,"Lc"},
                            {0x0200,"Rc"}, {0x0100,"Lrs"}, {0x0080,"Rrs"},
                            {0x0040,"Lw"}, {0x0020,"Rw"}, {0x0001,"LFE"}
                        };
                    for (const auto& [bit, lbl] : bits)
                    {
                        if (mask & bit)
                        {
                            if (bit == 0x0001)
                            {
                                has_lfe = true;
                            }
                            else
                            {
                                extension_channels.push_back(lbl);
                            }
                        }
                    }
                }
                else
                {
                    append_acmod(acmod, extension_channels);
                    if (lfeon)
                    {
                        has_lfe = true;
                    }
                }

                out.total_channels =
                    static_cast<uint8_t>(base_channels.size() +
                                         extension_channels.size() +
                                         (has_lfe ? 1 : 0));
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

    return std::format(
                       "--- {} Snapshot ---\n"
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
