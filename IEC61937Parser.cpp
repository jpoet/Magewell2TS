#include "IEC61937Parser.h"

#include <algorithm>
#include <cstring>
#include <spdlog/spdlog.h>

namespace
{
constexpr uint16_t PA = 0xF872;
constexpr uint16_t PB = 0x4E1F;

constexpr uint8_t TYPE_PAUSE = 0x00;
constexpr uint8_t TYPE_AC3 = 0x01;
constexpr uint8_t TYPE_EAC3 = 0x15;
}

IEC61937Parser::IEC61937Parser(int verbose_level)
    : m_verbose(verbose_level)
{
    m_log = spdlog::get("app_logger");
    if (!m_log)
    {
        std::cerr << "OutputTS Error: Logger 'app_logger' not found!"
                  << std::endl;
        return;
    }
}

uint16_t IEC61937Parser::read_le16(const uint8_t* p)
{
    return (static_cast<uint16_t>(p[0]) |
            (static_cast<uint16_t>(p[1]) << 8));
}

#if 0
uint16_t IEC61937Parser::read_be16(const uint8_t* p)
{
    return (static_cast<uint16_t>(p[1]) |
            (static_cast<uint16_t>(p[0]) << 8));
}
#else
uint16_t IEC61937Parser::read_be16(const uint8_t* p)
{
    uint16_t val;
    std::memcpy(&val, p, 2);
    return __builtin_bswap16(val); // Instantly swaps the bytes at the hardware register layer
}
#endif

std::optional<IEC61937Parser::Frame> IEC61937Parser::PopFrame(void)
{
    if (m_frameQ.empty())
    {
        return std::nullopt;
    }

    Frame frame = std::move(m_frameQ.front());
    m_frameQ.pop_front();

    return frame;
}

void IEC61937Parser::Init(void)
{
    m_metaNeeded = true;
    m_codecpar.reset();
}

CodecParamsPtr
  IEC61937Parser::PushSamples(const uint8_t* data,
                              size_t size,
                              int64_t timestamp100ns,
                              uint32_t sampleRate)
{
    m_sampleRate = sampleRate;
    size_t existingBufferedBytes = m_stream.size();

    // Append to persistent stream
    m_stream.insert(m_stream.end(), data, data + size);

    while (true)
    {
        // Need at least 2 bytes
        if ((m_stream.size() - m_streamOffset) < 2)
        {
            break;
        }

        uint16_t word = read_be16(&m_stream[m_streamOffset]);

        switch (m_state)
        {
            case State::FIND_PA:
            {
                if (word == PA)
                {
                    int64_t relativeBytes =
                        static_cast<int64_t>(m_streamOffset) -
                        static_cast<int64_t>(existingBufferedBytes);

                    // 4 bytes per IEC 61937 stereo sample period
                    int64_t sampleOffset = relativeBytes / 4;

                    m_currentTimestamp =
                        timestamp100ns +
                        ((sampleOffset * 10000000LL) /
                         static_cast<int64_t>(sampleRate));

                    m_state = State::FIND_PB;
                }

                m_streamOffset += 2;
                break;
            }

            case State::FIND_PB:
            {
                if (word == PB)
                {
                    m_state = State::READ_PC;
                }
                else if (word != PA)
                {
                    m_state = State::FIND_PA;
                }
                // Note: if word == PA, stay in FIND_PB so the second PA
                // in a [PA, PA, PB] sequence is not lost!

                m_streamOffset += 2;
                break;
            }

            case State::READ_PC:
            {
                m_pc = word;
                m_state = State::READ_PD;
                m_streamOffset += 2;
                break;
            }

            case State::READ_PD:
            {
                m_pd = word;

                if (!begin_payload())
                {
                    m_streamOffset += 2;
                    m_state = State::FIND_PA;
                    break;
                }

                if (m_payloadTarget == 0)
                {
                    finalize_frame();
                    m_state = State::FIND_PA;
                }
                else if ((m_pc & 0x1F) == TYPE_EAC3)
                {
                    m_frameOffset = 0;
                    // For EAC3, m_pd is either in bits or bytes
                    // depending on the transmitter, or 0. Primarily
                    // rely on EAC3 frmsiz, but track PD if it's a
                    // valid byte/bit count.
                    m_state = State::READ_EAC3_HEADER;
                }
                else
                {
                    m_state = State::READ_PAYLOAD;
                }
                m_streamOffset += 2;

                break;
            }

            case State::READ_EAC3_HEADER:
            {
                size_t available = m_stream.size() - m_streamOffset;
                size_t needed = (m_frameOffset + EAC3_HEADER_SIZE) -
                                m_payload.size();
                size_t toCopy = std::min(available, needed);

                if (toCopy > 1)
                {
                    // Round down to the nearest even number of bytes
                    toCopy &= ~1;
                }
                else if (toCopy == 1 && needed > 1)
                {
                    // Wait for the next loop/packet to get the full
                    // 16-bit word
                    break;
                }

                m_payload.insert(m_payload.end(),
                                 m_stream.begin() + m_streamOffset,
                                 m_stream.begin() + m_streamOffset + toCopy);

                m_streamOffset += toCopy;

                // Need more header for the current syncframe
                if (m_payload.size() < (m_frameOffset + EAC3_HEADER_SIZE))
                {
                    break;
                }

                // View of the current syncframe header starting at
                // m_frameOffset
                const uint8_t* hdr = m_payload.data() + m_frameOffset;

                // Validate syncword (0x0B77)
                if (hdr[0] != 0x0b || hdr[1] != 0x77)
                {
                    if (m_frameOffset > 0)
                    {
                        size_t extra_read = m_payload.size() - m_frameOffset;
                        if (m_streamOffset >= extra_read)
                        {
                            m_streamOffset -= extra_read;
                        }
                        m_payload.resize(m_frameOffset);
                        finalize_frame();
                    }
                    else
                    {
                        m_payload.clear();
                    }
                    m_state = State::FIND_PA;
                    break;
                }

                std::vector<uint8_t> hdrVec(hdr, hdr + EAC3_HEADER_SIZE);
                size_t syncframeBytes = EAC3Parser::getFrameSizeBytes(hdrVec,
                                                              CodecType::EAC3);

                if (syncframeBytes < EAC3_HEADER_SIZE)
                {
                    if (m_frameOffset > 0)
                    {
                        size_t extra_read = m_payload.size() - m_frameOffset;
                        if (m_streamOffset >= extra_read)
                        {
                            m_streamOffset -= extra_read;
                        }
                        m_payload.resize(m_frameOffset);
                        finalize_frame();
                    }
                    else
                    {
                        m_payload.clear();
                    }
                    m_state = State::FIND_PA;
                    break;
                }

                // Expand target to cover the existing payload + this
                // syncframe
                m_payloadTarget = m_frameOffset + syncframeBytes;

#if 0
                if (m_frameCnt < 15)
                {
                    m_log->info("EAC3 frame [{}] (offset={}): frmsiz_bytes={} "
                                "m_pd={} m_pc={:#x}",
                                m_frameCnt, m_frameOffset,
                                syncframeBytes, m_pd, m_pc);
                }
#endif
                m_state = State::READ_PAYLOAD;
                break;
            }

            case State::READ_PAYLOAD:
            {
                if (m_payload.capacity() < m_payloadTarget)
                {
                    m_maxPayloadSize = std::max(m_maxPayloadSize,
                                                m_payloadTarget);
                    m_payload.reserve(m_maxPayloadSize);
                }

                size_t remaining = (m_streamOffset < m_stream.size())
                                   ? (m_stream.size() - m_streamOffset)
                                   : 0;
                size_t needed = m_payloadTarget - m_payload.size();
                size_t toCopy = std::min(remaining, needed);

                if (toCopy == 0)
                {
                    break;
                }

                const uint8_t* begin = m_stream.data() + m_streamOffset;
                const uint8_t* end = begin + toCopy;

                m_payload.insert(m_payload.end(), begin, end);
                m_streamOffset += toCopy;

                if (m_payload.size() >= m_payloadTarget)
                {
                    if ((m_pc & 0x1F) == TYPE_EAC3)
                    {
                        // Transition to READ_EAC3_HEADER to check for
                        // a concatenated dependent substream (or
                        // finalize if none)
                        m_frameOffset = m_payload.size();
                        m_payloadTarget = m_frameOffset + EAC3_HEADER_SIZE;
                        m_state = State::READ_EAC3_HEADER;
                        break;
                    }

                    finalize_frame();
                    m_state = State::FIND_PA;
                }

                break;
            }
        }
    }

    // Compact consumed bytes
    if (m_streamOffset > COMPACT_THRESHOLD)
    {
        m_stream.erase(m_stream.begin(),
                       m_stream.begin() + m_streamOffset);

        m_streamOffset = 0;
    }

    return std::move(m_codecpar);
}


bool IEC61937Parser::begin_payload(void)
{
    m_payload.clear();

    switch (m_pc & 0x1F)
    {
        case TYPE_PAUSE:
        {
            m_payloadTarget = 0;
            return true;
        }

        case TYPE_AC3:
        {
            m_payloadTarget = (static_cast<size_t>(m_pd) + 7) / 8;
            return true;
        }

        case TYPE_EAC3:
        {
            //
            // First read only enough
            // for EAC3 header parsing.
            //
            m_payloadTarget = EAC3_HEADER_SIZE;
            return true;
        }

        default:
          return false;
    }
}

void IEC61937Parser::finalize_frame(void)
{
    if ((m_pc & 0x1F) == TYPE_PAUSE)
    {
        Frame frame {
            .timestamp = m_currentTimestamp,
            .is_pause  = true
        };
        m_frameQ.push_back(std::move(frame));
        return;
    }

    if (m_payload.size() < 4)
    {
        return;
    }

    if (m_payload[0u] != 0x0b || m_payload[1u] != 0x77)
    {
        m_log->warn("[IEC61937] Invalid syncword");
        return;
    }

    Frame frame {
        .payload = std::move(m_payload),
        .timestamp = m_currentTimestamp
    };

    switch (m_pc & 0x1F)
    {
        case TYPE_AC3:
        {
            frame.codec_id = AV_CODEC_ID_AC3;
            break;
        }

        case TYPE_EAC3:
        {
            frame.codec_id = AV_CODEC_ID_EAC3;

            // Extract 2-bit strmtyp without parsing the entire frame.
            // Shifting byte 2 right by 6 leaves us with the stream
            // type value (0=Ind, 1=Dep).
            uint8_t strmtyp = (frame.payload[2u] >> 6) & 0x03;
            if (strmtyp == 1)
            {
                ++m_dependentCnt;
                if (!m_frameQ.empty() && !m_frameQ.back().is_pause)
                {
                    auto& prev = m_frameQ.back().payload;
                    prev.insert(prev.end(), frame.payload.begin(),
                                frame.payload.end());
                }
                return; // Append and skip the queue
            }
            break;
        }

        default:
          return;
    }

    if (m_metaNeeded)
    {
        if (frame.codec_id == AV_CODEC_ID_AC3)
        {
            if (auto result = m_eac3Parser.processFrame(frame.payload,
                                                        CodecType::AC3))
            {
                m_log->info(EAC3Parser::formatOutput(*result));
                if (result->total_channels < 2)
                {
                    m_log->warn("Failed to parse valid AC3");
                }
                else
                {
                    m_metaNeeded = false;
                    m_codecpar = make_codec_params();
                    m_codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
                    m_codecpar->format = AV_SAMPLE_FMT_NONE;
                    m_codecpar->codec_id = frame.codec_id;
                    m_codecpar->sample_rate = result->sample_rate_hz;
                    av_channel_layout_default(&m_codecpar->ch_layout,
                                              result->total_channels);

                    std::array<char, 64> buffer{};
                    av_channel_layout_describe(&(m_codecpar->ch_layout),
                                               buffer.data(),
                                               buffer.size());
                }
            }
        }
        else if (frame.codec_id == AV_CODEC_ID_EAC3)
        {
            if (auto result = m_eac3Parser.processFrame(frame.payload,
                                                        CodecType::EAC3))
            {
                if (result->total_channels < 2)
                {
                    m_log->warn("Failed to parse valid EAC3");
                }
                else if (result->strmtyp == 0)
                {
                    m_metaNeeded = false;
                    ++m_independentCnt;
                    if (m_verbose > 3)
                    {
                        m_log->debug("[{}] {}", m_frameCnt,
                                     EAC3Parser::formatOutput(*result));
                    }
                    m_codecpar = make_codec_params();
                    m_codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
                    m_codecpar->format = AV_SAMPLE_FMT_NONE;
                    m_codecpar->codec_id = frame.codec_id;
                    m_codecpar->sample_rate = result->sample_rate_hz;
                    av_channel_layout_default(&m_codecpar->ch_layout,
                                              result->total_channels);
                }
            }
        }
    }

    ++m_frameCnt;
    m_frameQ.push_back(std::move(frame));
}
