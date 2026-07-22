#include "BitStream.h"
#include "OutputTS.h"
#include "IEC61937Parser.h"

//#define DUMP_RAW

#ifdef DUMP_RAW
#include <fstream>
std::ofstream fraw("bitstream-audio.bin", std::ofstream::binary);
#endif

using namespace std;

BitStream::BitStream(OutputTS& parent, int verbose_level,
                     Params&& params, int64_t timestamp)
    : AudioStream(parent, verbose_level, std::move(params), timestamp)
{
    m_log->info("Opening bitstream audio");
}

BitStream::~BitStream(void)
{
}

void BitStream::Reset(void)
{
}

void BitStream::AddSamples(AudioStream::Samples&& samples)
{
    if (CodecParamsPtr&& codecpar =
        m_iec61937.PushSamples(samples.data.data(), samples.data.size(),
                               samples.timestamp,   m_params.sample_rate))
    {
        m_version = m_parent.AddMarker(OutputTS::AUDIO_STREAM_ID,
                                       std::move(codecpar),
                                       TimeBase::MPEG_TS,
                                       m_params.frame_duration,
                                       m_pts);
    }

    while(auto frame = m_iec61937.PopFrame())
    {
        if (frame->is_pause)
        {
#if 0
            // TODO: If receiving pause frames, maybe track ...
            m_lastAudioTimestamp =
                audio.frame.timestamp;
#endif
            m_log->trace("Audio pause burst");
            continue;
        }

#ifdef DUMP_RAW
        fraw.write(reinterpret_cast<char*>(frame->payload.data()),
                   frame->payload.size());
#endif

        PacketPtr pkt = make_packet(frame->payload.size());
        std::memcpy(pkt->data,
                    frame->payload.data(),
                    frame->payload.size());

        // Preserve original capture timestamp
        pkt->pts = pkt->dts = frame->timestamp;

        // Codec frame durations
        if (frame->codec_id == AV_CODEC_ID_AC3 ||
            frame->codec_id == AV_CODEC_ID_EAC3)
            pkt->duration =
                av_rescale_q(m_params.buffer_bytes,
                             AVRational {1, m_params.sample_rate},
                             TimeBase::Magewell);
        else
            pkt->duration = 0;

        av_packet_rescale_ts(pkt.get(),
                             TimeBase::Magewell,
                             TimeBase::MPEG_TS);

        pkt->stream_index = OutputTS::AUDIO_STREAM_ID;

        m_log->trace("Queuing bitstream [{}] "
                     "pts={} dts={} dur={} : "
                     "encoder TB:{}/{}; ",
                     pkt->stream_index,
                     pkt->pts,
                     pkt->dts,
                     pkt->duration,
                     TimeBase::Magewell.num,
                     TimeBase::Magewell.den);

        Packet qp {
            .is_marker    = false,
            .version      = m_version,
            .time_base    = TimeBase::Magewell,
            .pkt          = std::move(pkt),
            .codec_par    = nullptr
        };

        m_parent.AddAudioPkt(std::move(qp));
    }
}
