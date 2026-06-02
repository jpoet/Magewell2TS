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

IEC61937Parser::IEC61937Parser(void)
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
            break;

        uint16_t word = read_le16(&m_stream[m_streamOffset]);

        switch (m_state)
        {
            case State::FIND_PA:
            {
                if (word == PA)
                {
                    size_t relativeOffset =
                        m_streamOffset - existingBufferedBytes;

                    size_t wordIndex =
                        relativeOffset / 2;

                    m_currentTimestamp =
                        timestamp100ns +
                        ((wordIndex * 10000000ULL) / sampleRate);

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
                else
                {
                    m_state = State::FIND_PA;
                }

                m_streamOffset += 2;
                break;
            }

            case State::READ_PC:
            {
                m_pc    = word;
                m_state = State::READ_PD;
                m_streamOffset += 2;

                break;
            }

            case State::READ_PD:
            {
                m_pd = word;

                if (!begin_payload())
                {
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
                    m_state = State::READ_EAC3_HEADER;
                }
                else
                {
                    m_state = State::READ_PAYLOAD;
                }
                m_streamOffset += 2;

                break;
            }

            case State::READ_EAC3_HEADER :
            {
                size_t available = m_stream.size() - m_streamOffset;
                size_t needed    = EAC3_HEADER_SIZE - m_payload.size();
                size_t toCopy    = std::min(available, needed);

                m_payload.insert(m_payload.end(),
                                 &m_stream[m_streamOffset],
                                 &m_stream[m_streamOffset + toCopy]);

                m_streamOffset += toCopy;

                // Need more header
                if (m_payload.size() < EAC3_HEADER_SIZE)
                {
                    break;
                }

                m_payloadTarget = EAC3Parser::getFrameSizeBytes(m_payload,
                                                        CodecType::EAC3);

                // Header already included
                m_state = State::READ_PAYLOAD;

                break;
            }

            case State::READ_PAYLOAD:
            {
                if (m_payload.capacity() < m_payloadTarget)
                {
                    m_maxPayloadSize =
                        std::max(m_maxPayloadSize, m_payloadTarget);

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
                    finalize_frame();

                    m_state = State::FIND_PA;
                }

                break;
            }
        }
    }

    // Compact consumed bytes
#if 0
    if (m_streamOffset > 0)
    {
        m_stream.erase(m_stream.begin(),
                       m_stream.begin() + m_streamOffset);

        m_streamOffset = 0;
    }
#else
    if (m_streamOffset > COMPACT_THRESHOLD)
    {
        m_stream.erase(m_stream.begin(),
                       m_stream.begin() + m_streamOffset);

        m_streamOffset = 0;
    }
#endif

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
    if (m_payload.size() < 2)
    {
        return;
    }

    //
    // Validate syncword
    //
    if (m_payload[0] != 0x77 || m_payload[1] != 0x0B)
    {
        spdlog::warn("[IEC61937] Invalid syncword");
        return;
    }

    Frame frame
        {
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
                spdlog::info(EAC3Parser::formatOutput(*result));

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
        else if (frame.codec_id == AV_CODEC_ID_EAC3)
        {
            if (auto result = m_eac3Parser.processFrame(frame.payload,
                                                        CodecType::EAC3))
            {
                if (++m_frameCnt > 10)
                {
                    m_metaNeeded = false;
                }

                if (result->strmtyp == 0)
                {
                    ++m_independentCnt;
                    spdlog::info("[{}] {}", m_frameCnt,
                                 EAC3Parser::formatOutput(*result));
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
                else
                {
                    ++m_dependentCnt;
                    spdlog::info("[{}] {}", m_frameCnt,
                                 EAC3Parser::formatOutput(*result));
                }
            }
        }
    }

    m_frameQ.push_back(std::move(frame));
}
