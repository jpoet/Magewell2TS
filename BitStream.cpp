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
    , m_iec61937(verbose_level)
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
        Marker marker {
            .stream_id = OutputTS::AUDIO_STREAM_ID,
            .time_base = TimeBase::MPEG_TS,
            .frame_duration = m_params.frame_duration,
            .codec_par = std::move(codecpar)
        };
        m_version = m_parent.AddMarker(std::move(marker), m_pts);
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
            .version      = m_version,
            .pkt          = std::move(pkt),
        };

        m_parent.AddAudioPkt(std::move(qp));
    }
}
