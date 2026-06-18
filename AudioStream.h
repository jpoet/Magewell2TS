#pragma once

#include <optional>
#include <deque>

#include "MediaQueue.h"
#include "ffmpeg_types.h"

#include <spdlog/spdlog.h>
#ifdef SPDLOG_FMT_EXTERNAL
#include <fmt/format.h>
#else
#include <spdlog/fmt/bundled/format.h>
#endif

class OutputTS;

class AudioStream
{
  public:
    struct Params
    {
        int        num_channels        {0};
        bool       is_lpcm             {false};
        int        sample_rate         {0};
        int        bits_per_sample     {0};
        int        bytes_per_sample    {0};
        int        buffer_bytes        {0};
        int        samples_per_channel {0};
        AVRational frame_duration      {0, 1};
        bool operator==(const Params&) const = default;
    };

    using samples_t = std::vector<uint8_t>;
    struct Samples
    {
        samples_t   data;
        int64_t     timestamp   {-1};
        std::optional<Params> oParams;
    };
    using audioque_t = std::deque<Samples>;

    explicit AudioStream(OutputTS& parent, int verbose_level,
                         Params&& params, int64_t timestamp);
    virtual ~AudioStream(void) = 0;
    virtual void Reset(void) = 0;

    AudioStream(const AudioStream&) = delete;
    AudioStream& operator=(const AudioStream&) = delete;

    virtual void AddSamples(Samples&& audio) = 0;

  protected:
    // spdlog
    std::shared_ptr<spdlog::logger> m_log;

    OutputTS& m_parent;
    int       m_version;
    int       m_verbose;
    Params    m_params;
    int64_t   m_pts       {-1};
};


template <>
  struct fmt::formatter<AudioStream::Params>
{
    constexpr auto parse(format_parse_context& ctx) -> decltype(ctx.begin())
    {
        return ctx.begin();
    }

    template <typename FormatContext>
      auto format(const AudioStream::Params& params,
                  FormatContext& ctx) const -> decltype(ctx.out())
    {
        return fmt::format_to(ctx.out(),
                              "Audio[Channels:{}, LPCM:{}, "
                              "SampleRate:{}Hz, Bits/Sample:{}, "
                              "SamplesSize:{}]",
                              params.num_channels,
                              params.is_lpcm ? "Y" : "N",
                              params.sample_rate,
                              params.bits_per_sample,
                              params.buffer_bytes
                              );
    }
};
