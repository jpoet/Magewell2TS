#include <iostream>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>

// Only include QSV headers if CMake detected Intel MediaSDK / VPL headers
#if defined(HAS_MAGEWELL_QSV_SUPPORT)
  #include <libavutil/hwcontext_qsv.h>  // Contains AVQSVFramesContext

  #if (LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 0, 0))
    #include <vpl/mfxstructures.h> // Contains MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET
  #endif
#endif
}

#include "VideoStream.h"
#include "OutputTS.h"

using namespace std;
// using namespace std::chrono_literals;

VideoStream::VideoStream(OutputTS& parent, int verbose_level, Args& args,
                         Params&& params, MagCallback image_buffer_avail,
                         int64_t timestamp)
    : m_parent(parent)
    , m_verbose(verbose_level)
    , m_args(args)
    , m_params(std::move(params))
    , f_image_avail(image_buffer_avail)
{
    m_log = spdlog::get("app_logger");
    if (!m_log)
    {
        std::cerr << "VideoStream Error: Logger 'app_logger' not found!"
                  << std::endl;
        return;
    }

    // Allocate HDR metadata tracking blocks
    m_display_primaries = make_mastering_display_metadata();
    m_content_light = make_content_light_metadata();

    set_light(m_params.color);

    // Open replacement encoder
    if (!open_video())
    {
        m_encoderType = EncoderType::UNKNOWN;
        m_log->critical("Failed to open video encoder.");
        Shutdown();
    }

    CodecParamsPtr codecpar = make_codec_params();
    avcodec_parameters_from_context(codecpar.get(), m_encoder.get());

    m_version = m_parent.AddMarker(OutputTS::VIDEO_STREAM_ID,
                                   std::move(codecpar),
                                   m_encoder->time_base,
                                   m_params.frame_duration,
                                   timestamp);
}

VideoStream::~VideoStream(void)
{
    stop_frame_preparation();
    close_video();
}

void VideoStream::Shutdown(void)
{
    m_running.store(false);
    m_empty_avail.notify_all();
    m_hwframe_used.notify_all();
}

void VideoStream::close_video(void)
{
    m_parent.FlushPackets(OutputTS::VIDEO_STREAM_ID, m_version,
                          m_encoder.get());

    if (m_verbose > 1)
    {
        if (m_encoder)
        {
            string name = m_encoder->codec
                          ? m_encoder->codec->long_name
                          : "video";
            m_log->info("Closing {} encoder.", name);
        }
    }

    if (m_encoder && m_encoder->hw_frames_ctx)
    {
        m_log->info("VideoStream::close_video: encoder hw_frames_ctx refs={}",
                    av_buffer_get_ref_count(m_encoder->hw_frames_ctx));
    }

    if (m_hw_frames_ctx)
    {
        m_log->info("VideoStream::close_video: local hw_frames_ctx refs={}",
                    av_buffer_get_ref_count(m_hw_frames_ctx.get()));
    }

    m_encoder.reset();
    m_hw_frames_ctx.reset();

    m_log->info("VideoStream:Close {}", m_params);
}

void VideoStream::start_frame_preparation(void)
{
    if (m_running.load())
    {
        m_log->warn("Frame preparation thread is already arunning.");
        return;
    }

    m_running.store(true);

    for (int idx = 0; idx < m_args.buffers; ++idx)
    {
        PreparedFrame shell = {
            .hw_frame  = av_frame_alloc(),
            .cpu_frame = av_frame_alloc()
        };
        m_empty_shells.push_back(shell);
    }

    // Spawn the worker thread using a member function pointer
    m_prep_thread = std::thread(&VideoStream::prepare_frames, this);
    pthread_setname_np(m_prep_thread.native_handle(), "gpubuf");
    m_log->info("Started frame preparation thread.");
}

void VideoStream::stop_frame_preparation(void)
{
    if (!m_running.load() && !m_prep_thread.joinable())
        return;

    m_log->info("Stopping frame preparation thread...");
    m_running.store(false);
    m_hwframe_used.notify_all();
    m_empty_avail.notify_all();

    if (m_prep_thread.joinable())
        m_prep_thread.join();

    // Purge and free any leftover frames remaining in the queue
    {
        std::unique_lock<std::mutex> lock(m_hwframe_mutex);
        while (!m_preped_frames.empty())
        {
            PreparedFrame item = std::move(m_preped_frames.front());
            m_preped_frames.pop_front();

            if (item.cpu_frame)
                av_frame_free(&item.cpu_frame);
            if (item.hw_frame)
                av_frame_free(&item.hw_frame);
        }
    }

    m_log->info("Frame preparation thread fully stopped and queue flushed.");
}

/*
 * Set HDR light metadata
 * Copies HDR metadata for use in video encoding
 */
void VideoStream::set_light(const ColorSpace& color)
{
    m_display_primaries->has_primaries = false;
    m_display_primaries->has_luminance = false;

    std::copy(&color.display_primaries[0][0],
              &color.display_primaries[0][0] + (3 * 2),
              &m_display_primaries->display_primaries[0][0]);

    std::copy(std::begin(color.white_point),
              std::end(color.white_point),
              m_display_primaries->white_point);

    m_display_primaries->max_luminance = color.max_luminance;
    m_display_primaries->min_luminance = color.min_luminance;

    m_content_light->MaxCLL = color.MaxCLL;
    m_content_light->MaxFALL = color.MaxFALL;
}


/**
 * Open video encoder for output
 */
bool VideoStream::open_video(void)
{
    // Perform basic hardware parameter safety validation
    if (m_params.width <= 0 || m_params.height <= 0)
    {
        m_log->error("Invalid dimensions received: {}x{}",
                     m_params.width, m_params.height);
        return false;
    }

    if (m_verbose > 1)
    {
        m_log->info("Opening {} encoder {}",
                    m_args.codecName, m_params);
    }

    // Codec allocation & context setup
    const AVCodec* video_codec =
        avcodec_find_encoder_by_name(m_args.codecName.c_str());
    if (!video_codec)
    {
        m_log->error("Could not locate requested video encoder: '{}'",
                     m_args.codecName);
        return false;
    }

    m_encoder = make_codec_context(video_codec);
    if (!m_encoder)
    {
        m_log->error("Failed to allocate unique video encoding context.");
        return false;
    }

    // Assign hardware metrics straight to the active encoder context
    m_encoder->codec_id = video_codec->id;
    m_encoder->width = m_params.width;
    m_encoder->height = m_params.height;
    m_sw_pix_fmt = m_params.pix_fmt;

    if (m_sw_pix_fmt != AV_PIX_FMT_NV12 &&
        m_sw_pix_fmt != AV_PIX_FMT_P010LE)
    {
        m_log->error("Unsupported input pixel format: {}",
                     av_get_pix_fmt_name(m_sw_pix_fmt));
        return false;
    }

    m_encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    AVRational encoder_fps = AVRational
                             {
                                 m_params.frame_duration.den,
                                 m_params.frame_duration.num
                             };

    // Reduces the fraction while preserving accuracy within a safe
    // limit
    av_reduce(&encoder_fps.num, &encoder_fps.den,
              m_params.frame_duration.den, m_params.frame_duration.num,
              INT_MAX);

    m_encoder->framerate = encoder_fps;
    m_encoder->time_base = m_params.frame_duration;

    // Calculate dynamic GOP size threshold boundaries based on target seconds
    if (m_args.gopSecs > 0)
    {
        m_encoder->gop_size =
            static_cast<int>((static_cast<double>(encoder_fps.num) /
                              static_cast<double>(encoder_fps.den)) *
                             static_cast<double>(m_args.gopSecs) + 0.5);
        m_encoder->keyint_min = 1;
        if (m_verbose > 2)
            m_log->info("GOP size {} frames.",
                        m_encoder->gop_size);
    }

    // Assign color spaces using existing baseline tracking variables
    m_encoder->color_range     = m_params.color.range;
    m_encoder->color_primaries = m_params.color.primaries;
    m_encoder->color_trc       = m_params.color.trc;
    m_encoder->colorspace      = m_params.color.space;

    // Evaluate hardware thread slicing permissions
    if (m_encoder->codec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
    {
        m_encoder->thread_type = FF_THREAD_SLICE;
        if (m_verbose > 1)
        {
            m_log->info(" Video Encoder Strategy = THREAD SLICE");
        }
    }
    else if (m_encoder->codec->capabilities &
             AV_CODEC_CAP_FRAME_THREADS)
    {
        m_encoder->thread_type = FF_THREAD_FRAME;
        if (m_verbose > 1)
        {
            m_log->info(" Video Encoder Strategy = THREAD FRAME");
        }
    }

    // Setup GPU
    AVDictionary* local_opt = nullptr;
    bool success = false;

    switch (m_params.encoder_type)
    {
        case EncoderType::QSV:
          success = open_qsv(video_codec, &local_opt);
          break;
        case EncoderType::VAAPI:
          success = open_vaapi(video_codec, &local_opt);
          break;
        case EncoderType::NV:
          success = open_nvidia(video_codec, &local_opt);
          break;
        default:
          m_log->error("Unsupported hardware encoder architecture selected.");
          break;
    }

    start_frame_preparation();

    if (local_opt != nullptr)
    {
        av_dict_free(&local_opt);
    }

    return success;
}

bool VideoStream::open_nvidia(const AVCodec* codec, AVDictionary** opt_arg)
{
    int ret;
    AVDictionary* opt = nullptr;

    if (opt_arg && *opt_arg)
    {
        av_dict_copy(&opt, *opt_arg, 0);
    }

    // ---------------------------------------------------------------------
    // Configure NVENC private encoder options
    // ---------------------------------------------------------------------

    av_dict_set(&opt, "rc", "constqp", 0);
    m_encoder->global_quality = m_args.quality;

    if (!m_args.preset.empty())
    {
        av_dict_set(&opt, "preset", m_args.preset.c_str(), 0);

        if (m_verbose > 0)
        {
            m_log->info("Nvidia Engine: Applied preset '{}' for {}",
                        m_args.preset,
                        m_args.codecName);
        }
    }

#if 0
    // Configure real-time, low-latency streaming pipeline behavior
    av_dict_set(&opt, "delay", "0", 0);
    av_dict_set(&opt, "forced-idr", "1", 0);
    av_dict_set(&opt, "zerolatency", "1", 0);
#endif

    // This is critical! Otherwise there will be muxing issues.
    av_dict_set(&opt, "bf", "0", 0);

    if (m_args.lookahead > 0)
    {
        av_dict_set_int(&opt, "rc-lookahead", m_args.lookahead, 0);
        av_dict_set_int(&opt, "no-scenecut", 1, 0);
    }
    else
    {
        av_dict_set(&opt, "rc-lookahead", "0", 0);
    }

    // ---------------------------------------------------------------------
    // Create the persistent CUDA device context
    // ---------------------------------------------------------------------

    if (m_hw_device_ctx == nullptr)
    {
        AVBufferRef* raw_hw_ctx = nullptr;

        ret = av_hwdevice_ctx_create(&raw_hw_ctx,
                                     AV_HWDEVICE_TYPE_CUDA,
                                     m_args.device.c_str(),
                                     nullptr,
                                     0);

        if (ret < 0 || !raw_hw_ctx)
        {
            m_log->error("Failed to acquire persistent Nvidia CUDA Device "
                         "Context on '{}': {}",
                         m_args.device,
                         AVerr2str(ret));

            if (opt)
                av_dict_free(&opt);

            return false;
        }

        m_hw_device_ctx.reset(raw_hw_ctx);

        m_log->trace("nVidia CUDA hardware runtime engine successfully bound.");
    }

    // ---------------------------------------------------------------------
    // Create the CUDA hardware-frame context.
    //
    // The encoder receives AV_PIX_FMT_CUDA frames, while sw_format
    // describes the actual video pixels stored in those frames.
    //
    // m_sw_pix_fmt will be:
    //     AV_PIX_FMT_NV12   for normal 8-bit video
    //     AV_PIX_FMT_P010LE for HDR / forced P010
    // ---------------------------------------------------------------------

    AVBufferRef* raw_frames_ctx =
        av_hwframe_ctx_alloc(m_hw_device_ctx.get());

    if (!raw_frames_ctx)
    {
        m_log->error("Failed to allocate Nvidia CUDA frames context.");

        if (opt)
            av_dict_free(&opt);

        return false;
    }

    AVHWFramesContext* frames_ctx =
        reinterpret_cast<AVHWFramesContext*>(raw_frames_ctx->data);

    frames_ctx->format = AV_PIX_FMT_CUDA;
    frames_ctx->sw_format = m_sw_pix_fmt;
    frames_ctx->width = m_params.width;
    frames_ctx->height = m_params.height;

    // Maintain a pool of reusable CUDA surfaces.  The extra surfaces give
    // NVENC room for asynchronous operation and lookahead.
    frames_ctx->initial_pool_size =
        m_args.buffers + m_args.lookahead + 16;

    ret = av_hwframe_ctx_init(raw_frames_ctx);

    if (ret < 0)
    {
        m_log->error("Failed to initialize Nvidia CUDA frames context: {}",
                     AVerr2str(ret));

        av_buffer_unref(&raw_frames_ctx);

        if (opt)
            av_dict_free(&opt);

        return false;
    }

    // Transfer ownership into our persistent hardware-frame context.
    m_hw_frames_ctx.reset(raw_frames_ctx);

    // ---------------------------------------------------------------------
    // NVENC receives CUDA hardware frames.
    // ---------------------------------------------------------------------

    m_encoder->pix_fmt = AV_PIX_FMT_CUDA;

    // The encoder needs access to the CUDA device.
    if (m_encoder->hw_device_ctx)
    {
        av_buffer_unref(&m_encoder->hw_device_ctx);
    }

    m_encoder->hw_device_ctx =
        av_buffer_ref(m_hw_device_ctx.get());

    if (!m_encoder->hw_device_ctx)
    {
        m_log->error("Failed to reference Nvidia CUDA device context.");

        if (opt)
            av_dict_free(&opt);

        return false;
    }

    // Tell NVENC which CUDA frame pool it will receive frames from.
    if (m_encoder->hw_frames_ctx)
    {
        av_buffer_unref(&m_encoder->hw_frames_ctx);
    }

    m_encoder->hw_frames_ctx =
        av_buffer_ref(m_hw_frames_ctx.get());

    if (!m_encoder->hw_frames_ctx)
    {
        m_log->error("Failed to reference Nvidia CUDA frames context.");

        if (opt)
            av_dict_free(&opt);

        return false;
    }

    m_log->info("Nvidia CUDA frames: {}x{}, software format={}, "
                "hardware format=CUDA, pool={}",
                frames_ctx->width,
                frames_ctx->height,
                av_get_pix_fmt_name(frames_ctx->sw_format),
                frames_ctx->initial_pool_size);

    // ---------------------------------------------------------------------
    // Open the encoder
    // ---------------------------------------------------------------------

    ret = avcodec_open2(m_encoder.get(), codec, &opt);

    if (opt)
    {
        av_dict_free(&opt);
    }

    if (ret < 0)
    {
        m_log->critical("Fatal Error: Nvidia NVENC codec activation "
                        "rejected: {}",
                        AVerr2str(ret));

        return false;
    }

    // ---------------------------------------------------------------------
    // Report the final encoder configuration
    // ---------------------------------------------------------------------

    m_log->info("Nvidia NVENC pipeline fully active at resolution {}x{} "
                "with CUDA frames, sw_format={}",
                m_encoder->width,
                m_encoder->height,
                av_get_pix_fmt_name(m_sw_pix_fmt));

    return true;
}

bool VideoStream::open_vaapi(const AVCodec* codec, AVDictionary** opt_arg)
{
    int ret;
    AVDictionary* opt = nullptr;

    if (opt_arg && *opt_arg)
    {
        av_dict_copy(&opt, *opt_arg, 0);
    }

    m_encoder->global_quality = m_args.quality;

    if (!m_args.preset.empty())
    {
        av_opt_set(m_encoder->priv_data, "preset", m_args.preset.c_str(), 0);
        if (m_verbose > 0)
        {
            m_log->info("VAAPI Engine: Applied preset '{}' for {}",
                        m_args.preset, m_args.codecName);
        }
    }

    av_opt_set(m_encoder->priv_data, "async_depth", "4", 0);
    int surface_count_padding = m_args.extraHWframes + m_args.buffers + 4;
    if (m_args.lookahead > 0)
    {
        surface_count_padding += m_args.lookahead;
    }

    string child_device = "/dev/dri/" + m_args.device;

    if (m_hw_device_ctx == nullptr)
    {
        setenv("LIBVA_MESSAGING_LEVEL", "0", 1);

        AVBufferRef* raw_hw_ctx = nullptr;
        ret = av_hwdevice_ctx_create(&raw_hw_ctx,
                                     AV_HWDEVICE_TYPE_VAAPI,
                                     child_device.c_str(), nullptr, 0);

        if (ret < 0 || !raw_hw_ctx)
        {
            m_log->error("Failed to acquire persistent VAAPI Device "
                         "Context on path '{}': {}",
                         child_device, AVerr2str(ret));
            if (opt)
            {
                av_dict_free(&opt);
            }
            return false;
        }

        // Safely transfer ownership to the smart pointer
        m_hw_device_ctx.reset(raw_hw_ctx);

        m_log->trace("VAAPI hardware runtime engine successfully bound.");
    }

    if (opt)
    {
        av_dict_free(&opt);
    }

    m_encoder->pix_fmt = AV_PIX_FMT_VAAPI;

    // Clear out the old generation context safely using the smart pointer API
    m_hw_frames_ctx.reset();

    AVBufferRef* raw_frames_ctx = av_hwframe_ctx_alloc(m_hw_device_ctx.get());
    if (raw_frames_ctx == nullptr)
    {
        m_log->error("VAAPI Pool Builder: Failed to allocate hardware "
                     "frames memory block.");
        return false;
    }

    m_hw_frames_ctx.reset(raw_frames_ctx);

    AVHWFramesContext* frames_ctx =
        reinterpret_cast<AVHWFramesContext*>(m_hw_frames_ctx->data);

    frames_ctx->width = m_encoder->width;
    frames_ctx->height = m_encoder->height;
    frames_ctx->format = AV_PIX_FMT_VAAPI;
    frames_ctx->sw_format = m_sw_pix_fmt;
    frames_ctx->initial_pool_size = surface_count_padding + 16;

    ret = av_hwframe_ctx_init(m_hw_frames_ctx.get());
    if (ret < 0)
    {
        m_log->error("VAAPI Kernel Driver rejected fresh format "
                     "surface pool request: {}", AVerr2str(ret));
        m_hw_frames_ctx.reset(); // Safely calls av_buffer_unref internally
        return false;
    }

    // Explicitly unref the encoder's old context if it exists to avoid leakage
    if (m_encoder->hw_frames_ctx)
    {
        av_buffer_unref(&m_encoder->hw_frames_ctx);
    }

    // av_buffer_ref increments the internal atomic reference counter.
    // The encoder takes ownership of this reference, and will clean
    // it up automatically via avcodec_free_context().
    m_encoder->hw_frames_ctx = av_buffer_ref(m_hw_frames_ctx.get());
//    m_encoder->thread_count = 1;

    // Kernel initialization codec activation
    ret = avcodec_open2(m_encoder.get(), codec, nullptr);
    if (ret < 0)
    {
        m_log->critical("Fatal Error: VAAPI codec activation rejected "
                        "by system kernel: {}", AVerr2str(ret));
        Shutdown();
        return false;
    }

    m_log->trace("VAAPI pipeline fully active at "
                 "hardware resolution {}x{}",
                 frames_ctx->width, frames_ctx->height);
    return true;
}

bool VideoStream::open_qsv(const AVCodec* codec, AVDictionary** opt_arg)
{
    int ret;
    AVDictionary* opt = nullptr;

    if (opt_arg && *opt_arg)
    {
        av_dict_copy(&opt, *opt_arg, 0);
    }

    // Configure Intel QSV private encoder compression options
    av_opt_set(m_encoder->priv_data, "rc_mode", "ICQ", 0);
    m_encoder->global_quality = m_args.quality;

    if (m_args.codecName != "av1_qsv")
    {
        if (!m_args.preset.empty())
        {
            av_opt_set(m_encoder->priv_data, "preset",
                       m_args.preset.c_str(), 0);
            if (m_verbose > 0)
                m_log->info("QSV Engine: Applied preset '{}' for {}",
                            m_args.preset, m_args.codecName);
        }

        av_opt_set(m_encoder->priv_data, "scenario",
                   "livestreaming", 0);

        if (m_args.lookahead > 0 && av_opt_find(m_encoder->priv_data,
                                                "lookahead", nullptr, 0,
                                                AV_OPT_SEARCH_CHILDREN))
        {
            av_opt_set_int(m_encoder->priv_data, "lookahead", 1, 0);
            av_opt_set_int(m_encoder->priv_data, "lookahead_depth",
                           m_args.lookahead, 0);
        }

        av_opt_set(m_encoder->priv_data, "skip_frame",
                   "insert_dummy", 0);
        av_opt_set(m_encoder->priv_data, "async_depth", "4", 0);
    }

    av_opt_set_int(m_encoder->priv_data, "extra_hw_frames",
                   m_args.extraHWframes + m_args.lookahead + 4, 0);

    if (m_args.gopSecs > 0)
    {
        if (av_opt_set_int(m_encoder->priv_data,
                           "forced_idr", 1, 0) < 0)
        {
            m_log->warn("qsv: failed to set forced_idr");
        }
    }

    if (m_args.idrInterval > 0)
    {
        if (av_opt_set_int(m_encoder->priv_data,
                           "idr_interval", m_args.idrInterval, 0) < 0)
        {
            m_log->warn("qsv: failed to set idr_interval");
        }
    }

    string child_device = "/dev/dri/" + m_args.device;

    if (m_hw_device_ctx == nullptr)
    {
        setenv("LIBVA_DRIVER_NAME", "iHD", 0);
        setenv("LIBVA_MESSAGING_LEVEL", "0", 1);

        av_dict_set(&opt, "child_device", child_device.c_str(), 0);

        AVBufferRef* raw_hw_ctx = nullptr;
        // Pass the address of our local raw pointer variable
        ret = av_hwdevice_ctx_create(&raw_hw_ctx,
                                     AV_HWDEVICE_TYPE_QSV,
                                     m_args.device.c_str(), opt, 0);

        // Check the raw output pointer state explicitly
        if (ret < 0 || !raw_hw_ctx)
        {
            m_log->error("Failed to acquire persistent Intel QSV "
                         "Device Context on device '{}': {}",
                         m_args.device, AVerr2str(ret));
            if (opt)
            {
                av_dict_free(&opt);
            }
            return false;
        }

        // Anchor safely into our smart pointer
        m_hw_device_ctx.reset(raw_hw_ctx);

        m_log->trace("Intel QSV hardware runtime engine successfully bound");
    }

    m_log->trace("VideoStream::open_qsv: device refs={}",
                 av_buffer_get_ref_count(m_hw_device_ctx.get()));

    if (opt)
    {
        av_dict_free(&opt);
    }

    m_encoder->pix_fmt = AV_PIX_FMT_QSV;

    // Safely free the old generation context layout using the smart pointer API
    m_hw_frames_ctx.reset();

    AVBufferRef* raw_frames_ctx = av_hwframe_ctx_alloc(m_hw_device_ctx.get());
    if (raw_frames_ctx == nullptr)
    {
        m_log->error("QSV Pool Builder: Failed to allocate hardware "
                     "frames memory block.");
        return false;
    }

    // Bind to smart pointer immediately
    m_hw_frames_ctx.reset(raw_frames_ctx);

    AVHWFramesContext* frames_ctx =
        reinterpret_cast<AVHWFramesContext*>(m_hw_frames_ctx->data);

#define INTEL_ALIGN(x) (((x) + 31) & ~31)
    frames_ctx->width = INTEL_ALIGN(m_encoder->width);
    frames_ctx->height = INTEL_ALIGN(m_encoder->height);
    frames_ctx->format = AV_PIX_FMT_QSV;
    frames_ctx->sw_format = m_sw_pix_fmt;
    frames_ctx->initial_pool_size = m_args.extraHWframes + m_args.lookahead + 16;

#if defined(HAS_MAGEWELL_QSV_SUPPORT) && (LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 0, 0))
    // This code only compiles if the hardware/library dependencies exist system-wide
    if (frames_ctx->hwctx)
    {
        AVQSVFramesContext* qsv_hwctx =
            reinterpret_cast<AVQSVFramesContext*>(frames_ctx->hwctx);
        qsv_hwctx->frame_type = MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET;
    }
#endif

    // Initialize the context using our smart pointer inner raw pointer
    ret = av_hwframe_ctx_init(m_hw_frames_ctx.get());
    if (ret < 0)
    {
        m_log->error("Intel Media Driver rejected fresh format surface "
                     "pool request: {}", AVerr2str(ret));
        m_hw_frames_ctx.reset(); // Automatically unreferences cleanly
        return false;
    }

    if (m_encoder->hw_frames_ctx)
    {
        av_buffer_unref(&m_encoder->hw_frames_ctx);
    }
    m_encoder->hw_frames_ctx = av_buffer_ref(m_hw_frames_ctx.get());
//    m_encoder->thread_count = 1;

    // Intel kernel initialization codec activation
    ret = avcodec_open2(m_encoder.get(), codec, nullptr);
    if (ret < 0)
    {
        m_log->critical("Fatal Error: Intel QSV codec activation rejected "
                        "by system kernel: {}", AVerr2str(ret));
        Shutdown();
        return false;
    }

    m_log->trace("Intel QSV pipeline fully active at "
                 "hardware resolution {}x{}",
                 frames_ctx->width, frames_ctx->height);

    return true;
}

bool VideoStream::EncodeFrame(void)
{
#ifdef LOG_ELAPSED
    chrono::steady_clock::time_point active_start
        = chrono::steady_clock::now();
#endif

    PreparedFrame job;
    {
        std::unique_lock<std::mutex> lock(m_queue_mutex);
        if (m_active_frames.empty())
            return false;

        job = std::move(m_active_frames.front());
        m_active_frames.pop_front();
    }

#ifdef LOG_ELAPSED
    chrono::steady_clock::time_point active_end
        = chrono::steady_clock::now();

    chrono::steady_clock::time_point encode_start
        = chrono::steady_clock::now();
#endif

    bool result = m_parent.EncodeFrame(OutputTS::VIDEO_STREAM_ID,
                                       m_version,
                                       m_encoder.get(), job.hw_frame);

#ifdef LOG_ELAPSED
    chrono::steady_clock::time_point encode_end
        = chrono::steady_clock::now();

    auto active_dur = chrono::duration_cast<chrono::microseconds>
                      (active_end - active_start);
    auto encode_dur = chrono::duration_cast<chrono::microseconds>
                      (encode_end - encode_start);

    if (active_dur > 1ms || encode_dur > 7ms)
    {
        m_log->debug("Wait for active buf {}μs. Encode {}μs",
                     active_dur.count(), encode_dur.count());
    }
#endif

    av_frame_unref(job.cpu_frame);
    av_frame_unref(job.hw_frame);

    {
        std::unique_lock<std::mutex> lock(m_pool_mutex);
        m_empty_shells.push_back(std::move(job));
    }

    m_empty_avail.notify_one();

    if (!result)
    {
        m_log->error("Video encode_frame pipeline step dropped out or failed.");
        return false;
    }

    return true;
}

bool VideoStream::HasActiveFrames(void) const
{
    std::scoped_lock lock(m_queue_mutex);
    return !m_active_frames.empty();
}

void VideoStream::prepare_frames(void)
{
    while (m_running.load())
    {
        PreparedFrame job;
        {
            std::unique_lock<std::mutex> lock(m_pool_mutex);

            m_empty_avail.wait(lock, [this]() {
                return !m_running.load() || !m_empty_shells.empty();
            });

            if (!m_running.load())
                return;

            job = std::move(m_empty_shells.front());
            m_empty_shells.pop_front();
        }

        size_t retries = 0;

#ifdef LOG_ELAPSED
        chrono::steady_clock::time_point get_start;
        chrono::steady_clock::time_point get_end;
#endif

        for (;;)
        {
            av_frame_unref(job.cpu_frame);
            av_frame_unref(job.hw_frame);

#ifdef LOG_ELAPSED
            get_start = chrono::steady_clock::now();
#endif

            int ret = av_hwframe_get_buffer(m_hw_frames_ctx.get(),
                                            job.hw_frame,
                                            0);

#ifdef LOG_ELAPSED
            get_end = chrono::steady_clock::now();
#endif

            if (ret == 0)
                break;

            if (ret != AVERROR(ENOMEM))
            {
                m_log->error("av_hwframe_get_buffer failed: {}",
                             AVerr2str(ret));
            }

            ++retries;

            m_log->warn("av_hwframe_get_buffer ENOMEM retry {}",
                        retries);

            {
                std::unique_lock<std::mutex> lock(m_hwframe_mutex);

                m_hwframe_used.wait_for(lock,
                                        std::chrono::milliseconds(3),
                                        [this]() {
                                            return !m_running.load();
                                        });
            }

            if (!m_running.load())
            {
                m_log->info("Frame preparation loop aborted via "
                            "shutdown signal.");

                av_frame_free(&job.hw_frame);
                return;
            }
        }

        if (retries)
        {
            m_log->warn("Prepared frame took {} retries",
                        retries);
        }

        // -------------------------------------------------------------
        // QSV / VAAPI: create a CPU-visible frame.
        //
        // NVIDIA/CUDA: leave hw_frame as a CUDA frame. The CPU -> CUDA
        // transfer is performed by AddImage().
        // -------------------------------------------------------------

        if (m_params.encoder_type != NV)
        {
#ifdef LOG_ELAPSED
            chrono::steady_clock::time_point map_start =
                chrono::steady_clock::now();
#endif

            int ret = av_hwframe_map(job.cpu_frame,
                                     job.hw_frame,
                                     AV_HWFRAME_MAP_WRITE |
                                     AV_HWFRAME_MAP_OVERWRITE
                                     );

#ifdef LOG_ELAPSED
            chrono::steady_clock::time_point map_end =
                chrono::steady_clock::now();

            auto get_dur =
                chrono::duration_cast<chrono::microseconds>(get_end - get_start);

            auto map_dur =
                chrono::duration_cast<chrono::microseconds>(map_end - map_start);

            if (map_dur > 7ms)
            {
                m_log->debug("Prepare: get: {} s map:{} s",
                             get_dur.count(),
                             map_dur.count());
            }
#endif

            if (ret < 0)
            {
                m_log->error("av_hwframe_map failed: {}",
                             AVerr2str(ret));

                av_frame_free(&job.cpu_frame);
                av_frame_free(&job.hw_frame);
                return;
            }
        }

        {
            std::unique_lock<std::mutex> lock(m_pool_mutex);
            m_preped_frames.push_back(std::move(job));
        }

        m_preped_avail.notify_one();
    }
}

int VideoStream::add_image_error_cleanup(Image&& image, PreparedFrame&& hw)
{
    av_frame_unref(hw.cpu_frame);
    av_frame_unref(hw.hw_frame);

    f_image_avail(image.pImage, image.pEco);

    {
        std::unique_lock<std::mutex> lock(m_pool_mutex);
        m_empty_shells.push_back(std::move(hw));
    }
    m_empty_avail.notify_one();
    Shutdown();
    return -1;
}

int VideoStream::AddImage(Image&& image)
{
    int size_bytes;
    uint8_t* src_data[4] = { nullptr };
    int src_linesize[4] = { 0 };
    PreparedFrame hw;

    /* Make sure encoder is open */
    if (!m_encoder || !m_hw_frames_ctx)
    {
        m_log->warn("Video encoder is not initialized or open");
        f_image_avail(image.pImage, image.pEco);
        Shutdown();
        return -1;
    }

    /* Get a prepared VRAM frame */
#ifdef LOG_ELAPSED
    chrono::steady_clock::time_point map_start
        = chrono::steady_clock::now();
#endif
    {
        std::unique_lock<std::mutex> lock(m_pool_mutex);

        m_preped_avail.wait(lock, [this]() {
            return !m_running.load() || !m_preped_frames.empty();
        });

        if (!m_running.load())
        {
            f_image_avail(image.pImage, image.pEco);
            return -1;
        }

        hw = std::move(m_preped_frames.front());
        m_preped_frames.pop_front();
    }
#ifdef LOG_ELAPSED
    chrono::steady_clock::time_point map_end
        = chrono::steady_clock::now();

    chrono::steady_clock::time_point copy_start
        = chrono::steady_clock::now();
#endif

    if (m_params.encoder_type == NV)
    {
        hw.cpu_frame->format = m_sw_pix_fmt;
        hw.cpu_frame->width = m_params.width;
        hw.cpu_frame->height = m_params.height;

        size_bytes = av_image_fill_arrays(hw.cpu_frame->data,
                                          hw.cpu_frame->linesize,
                                          image.pImage,
                                          m_sw_pix_fmt,
                                          m_params.width,
                                          m_params.height,
                                          1);

        if (size_bytes < 0)
        {
            m_log->error("av_image_fill_arrays failed: {}",
                         AVerr2str(size_bytes));
            return add_image_error_cleanup(std::move(image),
                                           std::move(hw));
        }

        int ret = av_hwframe_transfer_data(hw.hw_frame,
                                           hw.cpu_frame,
                                           0);

        if (ret < 0)
        {
            m_log->error("av_hwframe_transfer_data failed: {}",
                         AVerr2str(ret));
            return add_image_error_cleanup(std::move(image),
                                           std::move(hw));
        }

        // The image has now been copied into the CUDA frame.
        // cpu_frame still points at the Magewell buffer
        av_frame_unref(hw.cpu_frame);
    }
    else
    {
        size_bytes = av_image_fill_arrays(src_data, src_linesize, image.pImage,
                                          m_sw_pix_fmt, m_params.width,
                                          m_params.height, 1);
        if (size_bytes < 0)
        {
            m_log->error("av_image_fill_arrays failed: {}",
                         AVerr2str(size_bytes));
            return add_image_error_cleanup(std::move(image), std::move(hw));
        }

        // Direct memory block transfer into the cpu_frame staging area
        av_image_copy(hw.cpu_frame->data, hw.cpu_frame->linesize,
                      const_cast<const uint8_t**>(src_data),
                      src_linesize,
                      m_sw_pix_fmt, m_params.width, m_params.height);
    }

#ifdef LOG_ELAPSED
    chrono::steady_clock::time_point copy_end
        = chrono::steady_clock::now();

    auto buf_dur = chrono::duration_cast<chrono::microseconds>
                   (map_end - map_start);
    auto transfer_dur = chrono::duration_cast<chrono::microseconds>
                        (copy_end - copy_start);

    if (buf_dur > 1ms || transfer_dur > 15ms)
    {
        m_log->debug("Wait for prepared buf {}μs. Image transfer {}μs",
                     buf_dur.count(),
                     transfer_dur.count());
    }
#endif

    // Release Magewell frame buffer after copying is complete
    f_image_avail(image.pImage, image.pEco);

    // Metadata injection
    hw.hw_frame->colorspace      = m_params.color.space;
    hw.hw_frame->color_primaries = m_params.color.primaries;
    hw.hw_frame->color_trc       = m_params.color.trc;
    hw.hw_frame->color_range     = m_params.color.range;

    // Set Presentation Timestamps
    hw.hw_frame->pts = av_rescale_q(image.timestamp, TimeBase::Magewell,
                                    m_encoder->time_base);

    // Inject HDR metadata properties safely if present
    if (m_params.color.is_HDR)
    {
        if (m_display_primaries)
        {
            AVMasteringDisplayMetadata* primaries =
                av_mastering_display_metadata_create_side_data(hw.hw_frame);
            if (primaries)
            {
                *primaries = *m_display_primaries;
            }
        }

        if (m_content_light)
        {
            AVContentLightMetadata* light =
                av_content_light_metadata_create_side_data(hw.hw_frame);
            if (light)
            {
                *light = *m_content_light;
            }
        }
    }

    // Push hw_frame/cpu_frame into the queue for the encoder thread
    {
        std::scoped_lock lock(m_queue_mutex);
        m_active_frames.push_back(hw);
        return m_args.buffers - m_preped_frames.size();
    }
}
