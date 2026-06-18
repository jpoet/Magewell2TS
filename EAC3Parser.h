#include <iostream>
#include <cstdint>
#include <span>
#include <optional>
#include <string>
#include <vector>
#include <spdlog/spdlog.h>

enum class CodecType
{
    AC3  = 0x01,
    EAC3 = 0x15
};

struct EAC3MetaData
{
    CodecType   codec;
    uint8_t     substreamid;
    uint8_t     strmtyp;
    uint8_t     total_channels;
    uint32_t    sample_rate_hz;
    int64_t     duration;
    size_t      payload_size_bytes;
    std::string channel_layout;
};

class EAC3Parser
{
  public:
    EAC3Parser(void);

    /**
     * @brief Automatically extracts the frame size for BOTH AC-3 and
     * E-AC-3 streams.
     * @param raw_bytes Minimum 6 raw bytes from the IEC61937 stream
     * @return Frame payload size in bytes, or 0 if missing or invalid syncword.
     */
    static size_t getFrameSizeBytes(std::span<const uint8_t> raw_bytes,
                                    CodecType codec_type);

    std::optional<EAC3MetaData>
      processFrame(std::span<const uint8_t>iec_buffer, CodecType codec_type);


    static std::string formatOutput(const EAC3MetaData& out);

  private:
    // spdlog
    std::shared_ptr<spdlog::logger> m_log;

    void append_acmod(uint8_t acmod, std::vector<std::string>& target);
    std::string generate_layout() const;

    std::vector<std::string> base_channels{};
    std::vector<std::string> extension_channels{};
    uint32_t active_sample_rate = 0;
    bool has_lfe = false;
};
