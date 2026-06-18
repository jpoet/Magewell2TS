#include <iostream>

#include "AudioStream.h"
#include "OutputTS.h"

AudioStream::AudioStream(OutputTS& parent, int verbose_level,
                         Params&& params, int64_t timestamp)
    : m_parent(parent)
    , m_verbose(verbose_level)
    , m_params(params)
    , m_pts(timestamp)
{
    m_log = spdlog::get("app_logger");
    if (!m_log)
    {
        std::cerr << "BitStream Error: Logger 'app_logger' not found!"
                  << std::endl;
        return;
    }
}

AudioStream::~AudioStream(void)
{
}
