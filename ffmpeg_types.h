#pragma once

#include <memory>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}


/*
 *  Custom deleter for safe C++ pointer lifecycle management
 *  Specifically for FFmpeg "free" which take pointers to pointers
 */
template<auto FreeFn>
struct FFmpegDoublePtrDeleter
{
    template<typename T>
    void operator()(T* ptr) const
    {
        if (ptr)
            FreeFn(&ptr);
    }
};

using InputFormatContextPtr =
    std::unique_ptr<AVFormatContext,
        FFmpegDoublePtrDeleter<avformat_close_input> >;

using CodecContextPtr =
    std::unique_ptr<AVCodecContext,
        FFmpegDoublePtrDeleter<avcodec_free_context>>;

using CodecParamsPtr =
    std::unique_ptr<AVCodecParameters,
        FFmpegDoublePtrDeleter<avcodec_parameters_free>>;

using FramePtr =
    std::unique_ptr<AVFrame,
        FFmpegDoublePtrDeleter<av_frame_free>>;

using PacketPtr =
    std::unique_ptr<AVPacket,
        FFmpegDoublePtrDeleter<av_packet_free>>;

using SwrContextPtr =
    std::unique_ptr<SwrContext,
        FFmpegDoublePtrDeleter<swr_free>>;


/*
 * Custom delete from safe C++ pointer
 * Specifically for FFmpeg "free" which take a single pointer
 */
template<auto FreeFn>
struct FFmpegPtrDeleter
{
    template<typename T>
    void operator()(T* ptr) const
    {
        if (ptr)
            FreeFn(ptr);
    }
};

using OutputFormatContextPtr =
    std::unique_ptr<AVFormatContext,
        FFmpegPtrDeleter<avformat_free_context> >;

using ParserPtr =
    std::unique_ptr<AVCodecParserContext,
        FFmpegPtrDeleter<av_parser_close>>;

using SwsContextPtr =
    std::unique_ptr<SwsContext,
        FFmpegPtrDeleter<sws_freeContext>>;


/*
 *  Maker helpers
 */

inline CodecParamsPtr make_codec_params(void)
{
    return CodecParamsPtr(avcodec_parameters_alloc());
}

inline FramePtr make_frame(void)
{
    return FramePtr(av_frame_alloc());
}

inline ParserPtr make_parser(int id)
{
    return ParserPtr(av_parser_init(id));
}

inline CodecContextPtr make_codec_context(const AVCodec* codec)
{
    return CodecContextPtr(avcodec_alloc_context3(codec));
}

inline CodecContextPtr make_codec_context(void)
{
    return CodecContextPtr(avcodec_alloc_context3(nullptr));
}

inline PacketPtr make_packet(void)
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
