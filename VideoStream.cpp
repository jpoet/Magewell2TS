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
    if (!open_encoder())
    {
        m_encoderType = EncoderType::UNKNOWN;
        m_log->error("Failed to open video encoder.");
        Shutdown();
    }

    Marker marker {
        .stream_id      = OutputTS::VIDEO_STREAM_ID,
        .time_base      = m_encoder->time_base,
        .frame_duration = m_params.frame_duration,
        .encoder        = m_encoder
    };

    m_version = m_parent.AddMarker(std::move(marker), timestamp);
}

VideoStream::~VideoStream()
{
    Shutdown();
    stop_work();
    close_encoder();
}

void VideoStream::Shutdown()
{
    m_running.store(false, std::memory_order_release);
}

void VideoStream::close_encoder(void)
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
    m_encoder.reset();

    if (m_hw_frames_ctx)
    {
        m_log->info("VideoStream::close_video: local hw_frames_ctx refs={}",
                    av_buffer_get_ref_count(m_hw_frames_ctx.get()));
    }
    m_hw_frames_ctx.reset();

    m_log->info("VideoStream:Close {}", m_params);
}

void VideoStream::start_work(void)
{
    std::scoped_lock lock(m_workers_mutex);

    if (m_running.load())
    {
        m_log->warn("Encoder thread is already running.");
        return;
    }

    int num_threads = (m_args.num_threads > 0)
                      ? m_args.num_threads
                      : 1;

    m_log->debug("Initializing encoder with {} copy worker threads.",
                 num_threads);

    // Configure workers before starting threads.
    m_workers.clear();
    m_workers.resize(num_threads);

    m_next_capture_worker = 0;
    m_next_encode_worker  = 0;

    m_running.store(true, std::memory_order_release);
    for (int idx = 0; idx < num_threads; ++idx)
    {
        CopyThread& worker = m_workers[idx];

        worker.running.store(true);
        worker.name = format("vdcpy{}", idx);

        worker.cpy_thread =
            std::thread(&VideoStream::worker_thread_loop,
                        this,
                        std::ref(worker));

        pthread_setname_np(worker.cpy_thread.native_handle(),
                           worker.name.c_str());
    }

    // The pipeline is now ready to accept images.

    // Start encoder processing thread.
    m_encode_thread =
        std::thread(&VideoStream::encode_frames_loop, this);

    pthread_setname_np(m_encode_thread.native_handle(), "videnc");

    m_log->info("Started frame encoding thread.");
}

void VideoStream::stop_work(void)
{
    m_log->debug("Shutting down encoder pipeline...");

    m_running.store(false, std::memory_order_release);

    {
        std::scoped_lock workers_lock(m_workers_mutex);

        // Wake everybody.
        for (auto& worker : m_workers)
        {
            worker.running.store(false);
            worker.image_avail.notify_all();
            worker.frame_avail.notify_all();
        }
    }

    // Stop the encoder first. It accesses m_workers.
    if (m_encode_thread.joinable())
    {
        m_log->debug("Stopping video encoder worker");
        m_encode_thread.join();
    }

    // Now the encoder cannot access m_workers anymore.
    for (size_t idx = 0; idx < m_workers.size(); ++idx)
    {
        m_log->debug("Stopping vidcpy worker {}", idx);

        CopyThread& worker = m_workers[idx];

        if (worker.cpy_thread.joinable())
            worker.cpy_thread.join();

        std::scoped_lock lock(worker.mtx);

        while (!worker.images.empty())
        {
            Image img = std::move(worker.images.front());
            worker.images.pop_front();
            f_image_avail(img.pImage, img.pEco);
        }

        worker.frames.clear();
    }

    {
        std::scoped_lock workers_lock(m_workers_mutex);
        m_workers.clear();
    }

    if (m_verbose > 1)
        m_log->info("Encoder pipeline stopped and worker resources cleared.");
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
bool VideoStream::open_encoder(void)
{
    if (m_params.width <= 0 || m_params.height <= 0)
    {
        m_log->error("Invalid dimensions received: {}x{}",
                     m_params.width, m_params.height);
        return false;
    }

    if (m_verbose > 1)
        m_log->info("Opening {} encoder {}", m_args.codecName, m_params);

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

    // Assign hardware metrics to the active encoder context
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

    // Reduces the fraction while preserving accuracy within a safe limit
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
            m_log->info("GOP size {} frames.", m_encoder->gop_size);
    }

    // Assign color spaces using baseline tracking variables
    m_encoder->color_range     = m_params.color.range;
    m_encoder->color_primaries = m_params.color.primaries;
    m_encoder->color_trc       = m_params.color.trc;
    m_encoder->colorspace      = m_params.color.space;

    // Evaluate hardware thread slicing permissions
    if (m_encoder->codec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
    {
        m_encoder->thread_type = FF_THREAD_SLICE;
        m_log->debug("Video Encoder Strategy = THREAD SLICE");
    }
    else if (m_encoder->codec->capabilities &
             AV_CODEC_CAP_FRAME_THREADS)
    {
        m_encoder->thread_type = FF_THREAD_FRAME;
        m_log->debug("Video Encoder Strategy = THREAD FRAME");
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

    start_work();

    if (local_opt != nullptr)
        av_dict_free(&local_opt);

    return success;
}


bool VideoStream::open_nvidia(const AVCodec* codec, AVDictionary** opt_arg)
{
    int ret;
    AVDictionary* opt = nullptr;

    if (opt_arg && *opt_arg)
        av_dict_copy(&opt, *opt_arg, 0);

    // Configure NVENC private encoder options
    av_dict_set(&opt, "rc", "constqp", 0);
    av_dict_set_int(&opt, "qp", m_args.quality, 0);

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

    // B-frames
    av_dict_set_int(&opt, "bf", m_args.bframes, 0);
    // Adaptive B-frame decisions
    av_dict_set_int(&opt, "b_adapt", 1, 0);
    // Use B frames as references
    av_dict_set(&opt, "b_ref_mode", "middle", 0);
    // Spatial adaptive quantization
    av_dict_set_int(&opt, "spatial_aq", 1, 0);
    // Temporal AQ
    av_dict_set_int(&opt, "temporal_aq", 1, 0);

#if 0
    if (m_args.lookahead > 0)
    {
        av_dict_set_int(&opt, "rc-lookahead", m_args.lookahead, 0);
        av_dict_set_int(&opt, "no-scenecut", 1, 0);
    }
    else
#endif
    {
        av_dict_set(&opt, "rc-lookahead", "0", 0);
    }

    // Create the persistent CUDA device context
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

    /*
     * ---------------------------------------------------------------------
     * Create the CUDA hardware-frame context.
     *
     * The encoder receives AV_PIX_FMT_CUDA frames, while sw_format
     * describes the actual video pixels stored in those frames.
     *
     * m_sw_pix_fmt will be:
     *     AV_PIX_FMT_NV12   for normal 8-bit video
     *     AV_PIX_FMT_P010LE for HDR / forced P010
     * ---------------------------------------------------------------------
    */

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
    frames_ctx->initial_pool_size = m_args.lookahead +
                                    (m_args.num_threads * m_args.buffers) + 16;

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

    // NVENC receives CUDA hardware frames.
    m_encoder->pix_fmt = AV_PIX_FMT_CUDA;

    // The encoder needs access to the CUDA device.
    if (m_encoder->hw_device_ctx)
        av_buffer_unref(&m_encoder->hw_device_ctx);

    m_encoder->hw_device_ctx =
        av_buffer_ref(m_hw_device_ctx.get());

    if (!m_encoder->hw_device_ctx)
    {
        m_log->error("Failed to reference Nvidia CUDA device context.");

        if (opt)
            av_dict_free(&opt);

        return false;
    }

    if (m_encoder->hw_frames_ctx)
        av_buffer_unref(&m_encoder->hw_frames_ctx);

    m_encoder->hw_frames_ctx =
        av_buffer_ref(m_hw_frames_ctx.get());

    if (!m_encoder->hw_frames_ctx)
    {
        m_log->error("Failed to reference Nvidia CUDA frames context.");

        if (opt)
            av_dict_free(&opt);

        return false;
    }

    if (m_verbose > 1)
    {
        m_log->info("Nvidia CUDA frames: {}x{}, software format={}, "
                    "hardware format=CUDA, pool={}",
                    frames_ctx->width,
                    frames_ctx->height,
                    av_get_pix_fmt_name(frames_ctx->sw_format),
                    frames_ctx->initial_pool_size);
    }

    // Open the encoder
    ret = avcodec_open2(m_encoder.get(), codec, &opt);

    if (opt)
    {
        av_dict_free(&opt);
    }

    if (ret < 0)
    {
        m_log->error("Fatal Error: Nvidia NVENC codec activation "
                     "rejected: {}",
                     AVerr2str(ret));

        return false;
    }

    // Report the final encoder configuration
    if (m_verbose > 1)
    {
        m_log->info("Nvidia NVENC pipeline active at resolution {}x{} "
                    "with CUDA frames, sw_format={}",
                    m_encoder->width,
                    m_encoder->height,
                    av_get_pix_fmt_name(m_sw_pix_fmt));
    }

    return true;
}

bool VideoStream::open_vaapi(const AVCodec* codec, AVDictionary** opt_arg)
{
    int ret;
    AVDictionary* opt = nullptr;

    if (opt_arg && *opt_arg)
        av_dict_copy(&opt, *opt_arg, 0);

    m_encoder->global_quality = m_args.quality;

    av_dict_set_int(&opt, "bf", m_args.bframes, 0);
    av_opt_set_int(m_encoder->priv_data, "async_depth", 4, 0);

    if (m_args.gopSecs > 0)
    {
        AVRational encoder_fps = AVRational {
            m_params.frame_duration.den,
            m_params.frame_duration.num
        };

        // Reduces the fraction while preserving accuracy within a safe limit
        av_reduce(&encoder_fps.num, &encoder_fps.den,
                  m_params.frame_duration.den, m_params.frame_duration.num,
                  INT_MAX);

        m_encoder->gop_size =
            static_cast<int>((static_cast<double>(encoder_fps.num) /
                              static_cast<double>(encoder_fps.den)) *
                             static_cast<double>(m_args.gopSecs) + 0.5);
    }

    string child_device = "/dev/dri/" + m_args.device;

    if (m_hw_device_ctx == nullptr)
    {
        setenv("LIBVA_MESSAGING_LEVEL", "0", 1);

        AVBufferRef* raw_hw_ctx = nullptr;

        ret = av_hwdevice_ctx_create(&raw_hw_ctx,
                                     AV_HWDEVICE_TYPE_VAAPI,
                                     child_device.c_str(),
                                     nullptr,
                                     0);

        if (ret < 0 || !raw_hw_ctx)
        {
            m_log->error("Failed to acquire persistent VAAPI Device "
                         "Context on path '{}': {}",
                         child_device, AVerr2str(ret));

            if (opt)
                av_dict_free(&opt);

            return false;
        }

        // Bind to smart pointer
        m_hw_device_ctx.reset(raw_hw_ctx);

        m_log->trace("VAAPI hardware runtime engine successfully bound.");
    }

    m_encoder->pix_fmt = AV_PIX_FMT_VAAPI;

    m_hw_frames_ctx.reset();
    AVBufferRef* raw_frames_ctx = av_hwframe_ctx_alloc(m_hw_device_ctx.get());
    if (!raw_frames_ctx)
    {
        m_log->error("VAAPI Pool: Failed to allocate hardware "
                     "frames memory block.");

        if (opt)
            av_dict_free(&opt);

        return false;
    }

    // Bind to smart pointer.
    m_hw_frames_ctx.reset(raw_frames_ctx);

    AVHWFramesContext* frames_ctx =
        reinterpret_cast<AVHWFramesContext*>(m_hw_frames_ctx->data);

    frames_ctx->initial_pool_size = (m_args.num_threads * m_args.buffers)
                                    + m_args.extraHWframes;
    frames_ctx->width = m_encoder->width;
    frames_ctx->height = m_encoder->height;
    frames_ctx->format = AV_PIX_FMT_VAAPI;
    frames_ctx->sw_format = m_sw_pix_fmt;

    ret = av_hwframe_ctx_init(m_hw_frames_ctx.get());

    if (ret < 0)
    {
        m_log->error("VAAPI Kernel Driver rejected fresh format "
                     "surface pool request: {}",
                     AVerr2str(ret));

        m_hw_frames_ctx.reset();

        if (opt)
            av_dict_free(&opt);

        return false;
    }

    if (m_encoder->hw_frames_ctx)
        av_buffer_unref(&m_encoder->hw_frames_ctx);

    m_encoder->hw_frames_ctx =
        av_buffer_ref(m_hw_frames_ctx.get());

    ret = avcodec_open2(m_encoder.get(), codec, &opt);

    if (ret < 0)
    {
        m_log->error("Fatal Error: VAAPI codec activation failed: {}",
                     AVerr2str(ret));

        if (opt)
            av_dict_free(&opt);

        Shutdown();
        return false;
    }

    if (opt)
    {
        AVDictionaryEntry* entry = nullptr;

        while ((entry = av_dict_get(opt, "", entry,
                                    AV_DICT_IGNORE_SUFFIX)))
        {
            m_log->warn("VAAPI encoder option '{}' was rejected "
                        "(value '{}')",
                        entry->key,
                        entry->value);
        }

        av_dict_free(&opt);
    }

    m_log->trace("VAAPI pipeline hardware resolution {}x{}",
                 frames_ctx->width,
                 frames_ctx->height);

    return true;
}

bool VideoStream::open_qsv(const AVCodec* codec, AVDictionary** opt_arg)
{
    int ret;
    AVDictionary* opt = nullptr;

    if (opt_arg && *opt_arg)
        av_dict_copy(&opt, *opt_arg, 0);

    // Configure Intel QSV private encoder compression options
    av_opt_set(m_encoder->priv_data, "rc_mode", "ICQ", 0);
    m_encoder->global_quality = m_args.quality;

    if (!m_args.preset.empty())
    {
        av_opt_set(m_encoder->priv_data, "preset",
                   m_args.preset.c_str(), 0);

        if (m_verbose > 1)
        {
            m_log->info("QSV Engine: Applied preset '{}' for {}",
                        m_args.preset, m_args.codecName);
        }
    }

#if 0
    av_opt_set(m_encoder->priv_data, "scenario", "cameracapture", 0);
#else
    av_opt_set(m_encoder->priv_data, "scenario", "archive", 0);
#endif

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

    /* b-frames */
    av_dict_set_int(&opt, "bf", m_args.bframes, 0);
    av_dict_set_int(&opt, "b_strategy", 1, 0);
    av_dict_set_int(&opt, "extbrc", 1, 0);

    av_opt_set_int(m_encoder->priv_data, "async_depth", 4, 0);

    av_opt_set_int(m_encoder->priv_data, "extra_hw_frames",
                   m_args.extraHWframes + m_args.lookahead + 4, 0);

    /* GOP */
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

/*
 *  Open the encoder
 */
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
                av_dict_free(&opt);

            return false;
        }

        // Bind to smart pointer
        m_hw_device_ctx.reset(raw_hw_ctx);

        m_log->trace("Intel QSV hardware runtime engine successfully bound");
    }

    m_log->trace("VideoStream::open_qsv: device refs={}",
                 av_buffer_get_ref_count(m_hw_device_ctx.get()));

    if (opt)
        av_dict_free(&opt);

    m_encoder->pix_fmt = AV_PIX_FMT_QSV;

    m_hw_frames_ctx.reset();
    AVBufferRef* raw_frames_ctx = av_hwframe_ctx_alloc(m_hw_device_ctx.get());
    if (raw_frames_ctx == nullptr)
    {
        m_log->error("QSV Pool: Failed to allocate hardware "
                     "frames memory block.");
        return false;
    }

    // Bind to smart pointer.
    m_hw_frames_ctx.reset(raw_frames_ctx);

    AVHWFramesContext* frames_ctx =
        reinterpret_cast<AVHWFramesContext*>(m_hw_frames_ctx->data);

    frames_ctx->width = m_encoder->width;
    frames_ctx->height = m_encoder->height;
    frames_ctx->format = AV_PIX_FMT_QSV;
    frames_ctx->sw_format = m_sw_pix_fmt;
    frames_ctx->initial_pool_size = (m_args.num_threads * m_args.buffers)
                                    + m_args.extraHWframes
                                    + m_args.lookahead + 4;

#if defined(HAS_MAGEWELL_QSV_SUPPORT) && (LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 0, 0))
    // This code only compiles if the hardware/library dependencies exist system-wide
    if (frames_ctx->hwctx)
    {
        AVQSVFramesContext* qsv_hwctx =
            reinterpret_cast<AVQSVFramesContext*>(frames_ctx->hwctx);
        qsv_hwctx->frame_type = MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET;
    }
#endif

    ret = av_hwframe_ctx_init(m_hw_frames_ctx.get());
    if (ret < 0)
    {
        m_log->error("Intel Media Driver rejected fresh format surface "
                     "pool request: {}", AVerr2str(ret));
        m_hw_frames_ctx.reset();
        return false;
    }

    if (m_encoder->hw_frames_ctx)
        av_buffer_unref(&m_encoder->hw_frames_ctx);

    m_encoder->hw_frames_ctx = av_buffer_ref(m_hw_frames_ctx.get());

    // Intel kernel initialization codec activation
    ret = avcodec_open2(m_encoder.get(), codec, nullptr);
    if (ret < 0)
    {
        m_log->error("Fatal Error: Intel QSV codec activation failed: ",
                     AVerr2str(ret));
        Shutdown();
        return false;
    }

    m_log->trace("Intel QSV pipeline hardware resolution {}x{}",
                 frames_ctx->width, frames_ctx->height);

    return true;
}

void VideoStream::encode_frames_loop(void)
{
#ifdef LOG_ELAPSED
    int      frame_counter = 0;
    uint64_t total_work_time_us  = 0;
#endif

    while (m_running.load())
    {
        // Round-robin worker queue to guarantee chronologically
        // correct indexing
        CopyThread& worker = m_workers[m_next_encode_worker];
        m_next_encode_worker = (m_next_encode_worker + 1) % m_workers.size();

        FramePtr hw_frame;

#ifdef LOG_ELAPSED
        chrono::steady_clock::time_point work_start =
            chrono::steady_clock::now();
#endif
        {
            std::unique_lock lock(worker.mtx);
            worker.frame_avail.wait(lock, [&worker, this] {
                return !m_running.load() || !worker.frames.empty();
            });

            if (!m_running.load())
                break;

            hw_frame = std::move(worker.frames.front());
            worker.frames.pop_front();
        }
#ifdef LOG_ELAPSED
        chrono::steady_clock::time_point work_end =
            chrono::steady_clock::now();
        auto work_dur =
            chrono::duration_cast<chrono::microseconds>(work_end - work_start);

        total_work_time_us += work_dur.count();
        ++frame_counter;

        if (frame_counter >= 1800) [[unlikely]]
        {
            uint64_t avg = total_work_time_us / frame_counter;

            m_log->debug("Over past {} frames -> Avg wait for work: {}μs",
                         frame_counter, avg);

            total_work_time_us = 0;
            frame_counter           = 0;
        }

        chrono::steady_clock::time_point encode_start
            = chrono::steady_clock::now();
#endif

        bool result = m_parent.EncodeFrame(OutputTS::VIDEO_STREAM_ID,
                                           m_version,
                                           m_encoder.get(),
                                           hw_frame.get());

#ifdef LOG_ELAPSED
        chrono::steady_clock::time_point encode_end
            = chrono::steady_clock::now();

        auto encode_dur = chrono::duration_cast<chrono::microseconds>
                          (encode_end - encode_start);

        if (encode_dur > 7ms)
        {
            m_log->debug("Encode took {}μs", encode_dur.count());
        }
#endif

        av_frame_unref(hw_frame.get());

        if (!result)
        {
            m_log->error("Video encode_frame pipeline failed.");
            Shutdown();
        }
    }
}

void VideoStream::worker_thread_loop(CopyThread& worker)
{
    auto* hw_ctx = reinterpret_cast<AVHWFramesContext*>(m_hw_frames_ctx->data);

    m_log->info("Started {} worker thread", worker.name);

    // Track time and accumulation variables
    auto last_report_time = std::chrono::steady_clock::now();
    uint64_t backlog_sum = 0;
    uint64_t sample_count = 0;

    while (worker.running.load() && m_running.load())
    {
        Image image;
        size_t current_backlog = 0;

        {
            std::unique_lock lock(worker.mtx);
            worker.image_avail.wait(lock, [&worker, this] {
                return !worker.running || !m_running.load() ||
                    !worker.images.empty();
            });

            if (!worker.running || !m_running.load() || worker.images.empty())
                continue;

            image = std::move(worker.images.front());
            worker.images.pop_front();

            current_backlog = worker.images.size();
        }

        backlog_sum += current_backlog;
        ++sample_count;

        // Check if 60 seconds have passed
        auto now = std::chrono::steady_clock::now();
        if (now - last_report_time >= std::chrono::seconds(60))
        {
            double average_backlog = static_cast<double>(backlog_sum)
                                     / sample_count;

            if (average_backlog > 2.5)
            {
                m_log->info("{} worker average backlog {:.2f} over "
                            "the past 60s. Consider increasing '--threads'",
                            worker.name, average_backlog);
            }

            // Reset trackers
            backlog_sum = 0;
            sample_count = 0;
            last_report_time = now;
        }

        int ret;
        FramePtr hw = make_frame();
        while (worker.running.load() && m_running.load())
        {
            ret = av_hwframe_get_buffer(m_hw_frames_ctx.get(), hw.get(), 0);
            if (ret == 0)
                break;

            if (ret != AVERROR(ENOMEM))
            {
                m_log->error("{} worker failed to grab hardware "
                             "pool surface: {}", worker.name, AVerr2str(ret));
                Shutdown();
            }
            else
            {
                m_log->warn("{} worker failed to grab hardware "
                            "pool surface: {}. Will retry.",
                            worker.name, AVerr2str(ret));
            }
            this_thread::sleep_for(chrono::milliseconds(5));
        }

        FramePtr cpu_frame = make_frame();
        if (!cpu_frame)
        {
            m_log->warn("Failed to allocate local CPU frame wrapper.");
            f_image_avail(image.pImage, image.pEco);
            continue;
        }
        cpu_frame->format = m_sw_pix_fmt;
        cpu_frame->width  = hw_ctx->width;
        cpu_frame->height = hw_ctx->height;

        int size_bytes = av_image_fill_arrays(cpu_frame->data,
                                              cpu_frame->linesize,
                                              image.pImage, m_sw_pix_fmt,
                                              cpu_frame->width,
                                              cpu_frame->height, 1);

        if (size_bytes < 0)
        {
            m_log->error("{} av_image_fill_arrays failed: {}",
                         worker.name, AVerr2str(size_bytes));
            f_image_avail(image.pImage, image.pEco);
            Shutdown();
            break;
        }

        cpu_frame->extended_data = cpu_frame->data;

        ret = av_hwframe_transfer_data(hw.get(), cpu_frame.get(), 0);
        f_image_avail(image.pImage, image.pEco);

        if (ret < 0)
        {
            m_log->warn("DAMAGED: {} av_hwframe_transfer_data failed: {}",
                        worker.name, AVerr2str(ret));

            m_log->warn("{} transfer failed:"
                        " hw=%dx%d fmt=%s"
                        " cpu=%dx%d fmt=%s"
                        " cpu_ls=%d/%d hw_ls=%d/%d",
                        worker.name,
                        hw->width, hw->height,
        av_get_pix_fmt_name(static_cast<AVPixelFormat>(hw->format)),
                        cpu_frame->width, cpu_frame->height,
        av_get_pix_fmt_name(static_cast<AVPixelFormat>(cpu_frame->format)),
                        cpu_frame->linesize[0], cpu_frame->linesize[1],
                        hw->linesize[0], hw->linesize[1]);

            this_thread::sleep_for(chrono::milliseconds(2));
            continue;
        }

        hw->colorspace      = m_params.color.space;
        hw->color_primaries = m_params.color.primaries;
        hw->color_trc       = m_params.color.trc;
        hw->color_range     = m_params.color.range;
        hw->pts = av_rescale_q(image.timestamp, TimeBase::Magewell,
                               m_encoder->time_base);

        // Handle HDR metadata attachments
        if (m_params.color.is_HDR)
        {
            if (m_display_primaries)
            {
                av_frame_remove_side_data(hw.get(),
                                     AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
                AVMasteringDisplayMetadata* primaries =
                    av_mastering_display_metadata_create_side_data(hw.get());
                if (primaries)
                    *primaries = *m_display_primaries;
            }
            if (m_content_light)
            {
                av_frame_remove_side_data(hw.get(),
                                          AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
                AVContentLightMetadata* light =
                    av_content_light_metadata_create_side_data(hw.get());
                if (light)
                    *light = *m_content_light;
            }
        }

        {
            std::scoped_lock lock(worker.mtx);
            worker.frames.push_back(std::move(hw));
        }
        worker.frame_avail.notify_all();
    }

    if (m_verbose > 1)
        m_log->info("Stopped {} worker thread.", worker.name);
}

void VideoStream::AddImage(Image&& image)
{
    CopyThread* worker_ptr = nullptr;
    const size_t num_buffers = m_args.buffers;
    {
        std::scoped_lock workers_lock(m_workers_mutex);
        worker_ptr = &m_workers[m_next_capture_worker];
        m_next_capture_worker = (m_next_capture_worker + 1) % m_workers.size();
    }

    CopyThread& worker = *worker_ptr;
    {
        std::unique_lock worker_lock(worker.mtx);
        worker.WaitForSpace(worker_lock, m_running, num_buffers);

        if (!m_running.load())
        {
            f_image_avail(image.pImage, image.pEco);
            return;
        }

        worker.images.push_back(std::move(image));
    }

    worker.image_avail.notify_one();
}
