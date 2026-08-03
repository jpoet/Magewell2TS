#pragma once

#include "AudioStream.h"
#include "IEC61937Parser.h"

class BitStream : public AudioStream
{
  public:

    explicit BitStream(OutputTS& parent, int verbose_level,
                       Params&& params, int64_t timestamp);
    ~BitStream(void) override;
    void Reset(void) override;

    void AddSamples(AudioStream::Samples&& audio) override;

  private:
    IEC61937Parser m_iec61937;
};
