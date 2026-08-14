#pragma once

#include "AudioStream.h"

extern "C" {
#include <libavutil/audio_fifo.h>
}

class PCMStream : public AudioStream
{
  public:
    explicit PCMStream(OutputTS& parent, int verbose_level,
                       Params&& params, int64_t timestamp);
    ~PCMStream(void) override;
    void Reset(void) override;

    void AddSamples(AudioStream::Samples&& audio) override;

  private:
    bool open_encoder(void);
    void close_encoder(void);
    void encode_frame(void);

    CodecContextPtr m_encoder;
    AudioFifoPtr    m_fifo;

    SwrContextPtr m_swr{nullptr};
};
