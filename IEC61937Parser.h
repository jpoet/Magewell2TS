#pragma once

#include <cstdint>
#include <vector>
#include <deque>
#include <optional>
#include <utility>
#include <spdlog/spdlog.h>

#include "EAC3Parser.h"
#include "ffmpeg_types.h"

class IEC61937Parser
{
    static constexpr size_t COMPACT_THRESHOLD = 64 * 1024;
    static constexpr size_t EAC3_HEADER_SIZE = 12;

  public:

    struct Frame
    {
        AVCodecID codec_id{AV_CODEC_ID_NONE};
        std::vector<uint8_t> payload;
        int64_t timestamp { 0 };

        bool is_pause { false };
        bool is_atmos { false };
    };

  public:

    IEC61937Parser(void);

    void Init(void);

    CodecParamsPtr PushSamples(const uint8_t* data,
                               size_t size,
                               int64_t timestamp100ns,
                               uint32_t sampleRate);

    std::optional<Frame> PopFrame(void);

  private:

    enum class State
    {
        FIND_PA,
        FIND_PB,
        READ_PC,
        READ_PD,
        READ_EAC3_HEADER,
        READ_PAYLOAD
    };

  private:

    static uint16_t read_le16(const uint8_t* p);

    bool begin_payload();
    void finalize_frame();

  private:
    // spdlog
    std::shared_ptr<spdlog::logger> m_log;

    State m_state { State::FIND_PA };

    bool m_codec_known { false };
    bool m_init_stream { false };

    EAC3Parser m_eac3Parser;
    bool m_metaNeeded { true };

    std::deque<Frame> m_frameQ;

    // Persistent stream buffer
    std::vector<uint8_t> m_stream;

    size_t m_streamOffset { 0 };

    // Current burst
    uint16_t m_pc { 0 };
    uint16_t m_pd { 0 };

    size_t m_payloadTarget { 0 };

    std::vector<uint8_t> m_payload;
    size_t m_maxPayloadSize {4096};
    size_t m_frameCnt       {0};
    size_t m_dependentCnt   {0};
    size_t m_independentCnt {0};

    int m_sampleRate {0};
    int64_t m_currentTimestamp { 0 };
    CodecParamsPtr m_codecpar;
};
