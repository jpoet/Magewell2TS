#pragma once

#include <memory>
#include <string>
#include <format>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixfmt.h>
#include <libavutil/timestamp.h>
#include <libavutil/pixdesc.h>
#include <libavutil/audio_fifo.h>
}

inline std::string AV_ts2str(int64_t ts)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] {};
    av_ts_make_string(buf, ts);
    return buf;
}

inline std::string AV_ts2timestr(int64_t ts, const AVRational* tb)
{
    if (!tb || tb->num == 0)
    {
        return "0.00";
    }

    return std::format("{:.2f}", av_q2d(*tb) * ts);
}

inline std::string AVerr2str(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] {};
    av_make_error_string(buf, sizeof(buf), err);
    return buf;
}

inline bool operator==(const AVRational& a,
                       const AVRational& b) noexcept
{
    return a.num == b.num &&
        a.den == b.den;
}

/*
  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&ctx);
*/
template<auto FreeFn>
  struct FFmpegDoublePtrDeleter
{
    template<typename T>
      void operator()(T* ptr) const noexcept
    {
        if (ptr)
        {
            FreeFn(&ptr);
        }
    }
};

/*
  av_parser_close(parser);
  sws_freeContext(ctx);
  avformat_free_context(fmt);
*/
template<auto FreeFn>
  struct FFmpegPtrDeleter
{
    template<typename T>
      void operator()(T* ptr) const noexcept
    {
        if (ptr)
        {
            FreeFn(ptr);
        }
    }
};

struct AVBufferRefDeleter
{
    void operator()(AVBufferRef* ref) const noexcept
    {
        if (ref)
        {
            av_buffer_unref(&ref);
        }
    }
};

struct AVAudioFifoDeleter
{
    void operator()(AVAudioFifo* fifo) const noexcept
    {
        if (fifo)
        {
            av_audio_fifo_free(fifo);
        }
    }
};

struct MasteringDisplayMetadataDeleter
{
    void operator()(AVMasteringDisplayMetadata* p) const noexcept
    {
        av_free(p);
    }
};

struct ContentLightMetadataDeleter
{
    void operator()(AVContentLightMetadata* p) const noexcept
    {
        av_free(p);
    }
};

using InputFormatContextPtr =
    std::unique_ptr<
        AVFormatContext,
        FFmpegDoublePtrDeleter<avformat_close_input>>;

using OutputFormatContextPtr =
    std::unique_ptr<
        AVFormatContext,
        FFmpegPtrDeleter<avformat_free_context>>;

using CodecContextPtr =
    std::unique_ptr<
        AVCodecContext,
        FFmpegDoublePtrDeleter<avcodec_free_context>>;

using CodecParamsPtr =
    std::unique_ptr<
        AVCodecParameters,
        FFmpegDoublePtrDeleter<avcodec_parameters_free>>;

using FramePtr =
    std::unique_ptr<
        AVFrame,
        FFmpegDoublePtrDeleter<av_frame_free>>;

using PacketPtr =
    std::unique_ptr<
        AVPacket,
        FFmpegDoublePtrDeleter<av_packet_free>>;

using ParserPtr =
    std::unique_ptr<
        AVCodecParserContext,
        FFmpegPtrDeleter<av_parser_close>>;

using SwsContextPtr =
    std::unique_ptr<
        SwsContext,
        FFmpegPtrDeleter<sws_freeContext>>;

using SwrContextPtr =
    std::unique_ptr<
        SwrContext,
        FFmpegDoublePtrDeleter<swr_free>>;

using AudioFifoPtr =
    std::unique_ptr<
        AVAudioFifo,
        AVAudioFifoDeleter>;

using BufferRefPtr =
    std::unique_ptr<
        AVBufferRef,
        AVBufferRefDeleter>;

using MasteringDisplayMetadataPtr =
    std::unique_ptr<
        AVMasteringDisplayMetadata,
        MasteringDisplayMetadataDeleter>;

using ContentLightMetadataPtr =
    std::unique_ptr<
        AVContentLightMetadata,
        ContentLightMetadataDeleter>;

inline CodecParamsPtr make_codec_params()
{
    return CodecParamsPtr(avcodec_parameters_alloc());
}

inline CodecContextPtr make_codec_context()
{
    return CodecContextPtr(avcodec_alloc_context3(nullptr));
}

inline CodecContextPtr make_codec_context(const AVCodec* codec)
{
    return CodecContextPtr(avcodec_alloc_context3(codec));
}

inline FramePtr make_frame()
{
    return FramePtr(av_frame_alloc());
}

inline PacketPtr make_packet()
{
    return PacketPtr(av_packet_alloc());
}

inline PacketPtr make_packet(int size)
{
    auto pkt = make_packet();

    if (!pkt)
        return {};

    if (av_new_packet(pkt.get(), size) < 0)
        return {};

    return pkt;
}

inline ParserPtr make_parser(int codec_id)
{
    return ParserPtr(av_parser_init(codec_id));
}

inline BufferRefPtr make_buffer_ref(int size)
{
    if (size <= 0)
        return {};

    return BufferRefPtr(av_buffer_alloc(size));
}

inline MasteringDisplayMetadataPtr
  make_mastering_display_metadata()
{
    return MasteringDisplayMetadataPtr(av_mastering_display_metadata_alloc());
}

inline ContentLightMetadataPtr
  make_content_light_metadata()
{
    return ContentLightMetadataPtr(av_content_light_metadata_alloc(nullptr));
}
