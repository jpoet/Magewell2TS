#include "PCMStream.h"
#include "OutputTS.h"

using namespace std;

PCMStream::PCMStream(OutputTS& parent, int verbose_level,
                     Params&& params, int64_t timestamp)
    : AudioStream(parent, verbose_level, std::move(params), timestamp)
{
    m_log->info("Opening PCM audio stream");
    if (!open_encoder())
    {
        m_log->critical("Failed to open audio encoder.");
        throw std::runtime_error("Failed to open audio encoder.");
    }
}

PCMStream::~PCMStream(void)
{
    close_encoder();
}

void PCMStream::Reset(void)
{
    close_encoder();
    open_encoder();
}

bool PCMStream::open_encoder(void)
{
    // LPCM -> AC3 ENCODE
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AC3);

    if (!codec)
    {
        m_log->error("AC3 encoder not found");
        return false;
    }

    m_encoder = make_codec_context(codec);
    if (!m_encoder)
    {
        m_log->error("Failed to allocate unique video encoding context.");
        return false;
    }

    // AC3 standard sample rate
    m_encoder->sample_rate = 48000;
    // Internal m_encoderoder format
    m_encoder->sample_fmt = AV_SAMPLE_FMT_FLTP;
    // AC3 timing domain
    m_encoder->time_base = { 1, m_encoder->sample_rate };

    av_channel_layout_default(&m_encoder->ch_layout,
                              m_params.num_channels);

    switch (m_params.num_channels)
    {
        case 1:
        case 2:
          m_encoder->bit_rate = 224000;
          break;
        case 6:
          m_encoder->bit_rate = 448000;
          break;
        default:
          m_encoder->bit_rate = 448000;
          break;
    }

    int ret = avcodec_open2(m_encoder.get(), codec, nullptr);
    if (ret < 0)
    {
        m_log->error("avcodec_open2(audio) failed: {}",
                     AVerr2str(ret));
        close_encoder();
        return false;
    }

    m_log->debug("fmt={} channels={} frame_size={}",
                 av_get_sample_fmt_name(m_encoder->sample_fmt),
                 m_encoder->ch_layout.nb_channels,
                 m_encoder->frame_size);

    if (!m_fifo)
    {
        // Initial allocation size of required_frame_size * 2 allows
        // buffering room
        constexpr int fifo_size = 1536 * 16;
        AVAudioFifo* raw_fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP,
                                                    m_params.num_channels,
                                                    fifo_size);
        if (!raw_fifo)
        {
            m_log->error("Failed to allocate audio FIFO.");
            return false;
        }
        m_fifo.reset(raw_fifo);
    }

    m_log->info("Opened AC3 : {}ch {}Hz {}bps",
                m_encoder->ch_layout.nb_channels,
                m_encoder->sample_rate,
                m_encoder->bit_rate);

    CodecParamsPtr codecpar = make_codec_params();
    avcodec_parameters_from_context(codecpar.get(), m_encoder.get());

    // m_pts has native Magewell timestamp at this point.
    m_version = m_parent.AddMarker(OutputTS::AUDIO_STREAM_ID,
                                   std::move(codecpar),
                                   TimeBase::AUDIO48,
                                   m_params.frame_duration,
                                   m_pts);

    m_pts = av_rescale_q(m_pts,
                         TimeBase::Magewell,
                         TimeBase::AUDIO48);
    return true;
}

void PCMStream::close_encoder(void)
{
    m_parent.FlushPackets(OutputTS::AUDIO_STREAM_ID, m_version,
                          m_encoder.get());

    if (m_verbose > 1)
    {
        if (m_encoder)
        {
            string name = m_encoder->codec
                          ? m_encoder->codec->long_name
                          : "audio";
            m_log->info("Closing {}.", name);
        }
    }

    m_encoder.reset();
}

void PCMStream::encode_frame(void)
{
    constexpr int AC3_FRAME_SAMPLES = 1536;
    constexpr int AC3_SAMPLE_RATE   = 48000;

    AVFrame* frame = av_frame_alloc();
    if (!frame)
        return;

    frame->format      = AV_SAMPLE_FMT_FLTP;
    frame->sample_rate = AC3_SAMPLE_RATE;
    frame->nb_samples  = AC3_FRAME_SAMPLES;
    frame->pts         = m_pts;

    av_channel_layout_copy(&frame->ch_layout,
                           &m_encoder->ch_layout);

    int ret = av_frame_get_buffer(frame, 0);
    if (ret < 0)
    {
        m_log->error("Failed to allocate audio frame buffer.");
        av_frame_free(&frame);
        m_parent.Shutdown();
        return;
    }

    // Pull exactly one AC3 frame sized PCM buffer
    ret = av_audio_fifo_read(m_fifo.get(),
                             reinterpret_cast<void**>(frame->data),
                             AC3_FRAME_SAMPLES);
    if (ret < AC3_FRAME_SAMPLES)
    {
        av_frame_free(&frame);
        return;
    }

    if (!m_parent.EncodeFrame(OutputTS::AUDIO_STREAM_ID, m_version,
                              m_encoder.get(), frame))
    {
        m_log->error("encode_frame(audio) failed.");
    }
    else
    {
        // Advance perfect sample clock
        m_pts += AC3_FRAME_SAMPLES;
    }

    av_frame_free(&frame);
}

void PCMStream::AddSamples(AudioStream::Samples&& audio)
{
    assert(audio.data.size() ==
           static_cast<size_t>(m_params.samples_per_channel *
                               m_params.num_channels *
                               m_params.bytes_per_sample));

    constexpr int AC3_FRAME_SAMPLES = 1536;

    const int  channels      = m_params.num_channels;
    const bool is_24bit      = m_params.bits_per_sample > 16;
    const int  input_samples = m_params.samples_per_channel;

    // Temporary planar float buffer
    std::vector<float> planar(input_samples * channels);
    float* planes[8] {};

    for (int ch = 0; ch < channels; ++ch)
    {
        planes[ch] = planar.data() + (input_samples * ch);
    }


    if (!is_24bit)
    {
        // 16 bits
        const int16_t* src =
            reinterpret_cast<const int16_t*>(audio.data.data());

        constexpr float scale = 1.0f / 32768.0f;

        for (int s = 0; s < input_samples; ++s)
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                planes[ch][s] = static_cast<float>
                                (src[s * channels + ch]) * scale;
            }
        }
    }
    else
    {
        // 24 bits


        // Magewell 24-bit audio is delivered in 32-bit sample containers.
        // Treat as signed S32 PCM and normalize directly.
        const int32_t* src =
            reinterpret_cast<const int32_t*>(audio.data.data());

        constexpr float scale =
            1.0f / 2147483648.0f;

        for (int s = 0; s < input_samples; ++s)
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                planes[ch][s] =
                    static_cast<float>(src[s * channels + ch]) * scale;
            }
        }
    }

    if (av_audio_fifo_write(m_fifo.get(),
                            reinterpret_cast<void**>(planes),
                            input_samples) < input_samples)
    {
        m_log->error("Failed writing audio FIFO.");
        return;
    }

    m_pts = av_rescale_q(audio.timestamp,
                         TimeBase::Magewell,
                         TimeBase::AUDIO48);

    // Encode while enough samples exist
    while (av_audio_fifo_size(m_fifo.get()) >= AC3_FRAME_SAMPLES)
    {
        encode_frame();
    }
}
