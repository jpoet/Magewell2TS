#include <iostream>
#include <ranges>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

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

namespace {
struct DetectedGpu
{
    enum class Kind { Discrete, Integrated };
    enum class Backend { Nvidia, Drm }; // Drm = VAAPI / QSV (/dev/dri/renderD*)

    Kind        kind;
    Backend     backend;
    std::string device;      // e.g. "renderD128" or "0" (CUDA index)
    std::string pciAddr;     // e.g. "0000:03:00.0"
    std::string vendorId;    // e.g. "0x8086", "0x10de", "0x1002"
    std::string deviceId;    // e.g. "0x56a5"
    std::string driverName;  // e.g. "i915", "xe", "amdgpu", "nvidia"
    std::string label;       // Human-readable description for m_verbose > 2
    uint64_t    vramBytes {0};
};

std::string read_trimmed_line(const std::filesystem::path& p)
{
    std::ifstream f(p);
    std::string s;
    if (std::getline(f, s))
    {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' ||
                              s.back() == ' '  || s.back() == '\t'))
            s.pop_back();
    }
    return s;
}

uint64_t read_u64_file(const std::filesystem::path& p)
{
    std::ifstream f(p);
    uint64_t v = 0;
    if (f >> v)
        return v;
    return 0;
}

std::string vendor_pretty_name(const std::string& vendorHex)
{
    if (vendorHex == "0x8086") return "Intel";
    if (vendorHex == "0x10de") return "NVIDIA";
    if (vendorHex == "0x1002") return "AMD";
    return vendorHex;
}

std::vector<DetectedGpu> enumerate_system_gpus()
{
    namespace fs = std::filesystem;
    std::vector<DetectedGpu> gpus;

    // 1) Enumerate NVIDIA GPUs via /proc/driver/nvidia/gpus/* (works
    //    without linking libcuda)
    const fs::path nvRoot("/proc/driver/nvidia/gpus");
    std::error_code ec;
    if (fs::exists(nvRoot, ec))
    {
        std::vector<fs::path> nvDirs;
        for (const auto& entry : fs::directory_iterator(nvRoot, ec))
        {
            if (entry.is_directory(ec))
                nvDirs.push_back(entry.path());
        }
        std::sort(nvDirs.begin(), nvDirs.end());

        int cudaIdx = 0;
        for (const auto& dir : nvDirs)
        {
            DetectedGpu g;
            g.kind       = DetectedGpu::Kind::Discrete;
            g.backend    = DetectedGpu::Backend::Nvidia;
            g.device     = std::to_string(cudaIdx++);
            g.pciAddr    = dir.filename().string();
            g.vendorId   = "0x10de";
            g.driverName = "nvidia";

            std::string modelName = "NVIDIA GPU";
            std::ifstream info(dir / "information");
            std::string line;
            while (std::getline(info, line))
            {
                if (line.starts_with("Model:"))
                {
                    auto pos = line.find_first_not_of(" \t", 6);
                    if (pos != std::string::npos)
                        modelName = line.substr(pos);
                }
            }
            g.label = fmt::format("{} (CUDA #{}, PCI {}, driver=nvidia)",
                                  modelName, g.device, g.pciAddr);
            gpus.push_back(std::move(g));
        }
    }

    // 2) Enumerate DRM render nodes (/sys/class/drm/renderD*) for
    //    Intel / AMD VAAPI & QSV
    const fs::path drmRoot("/sys/class/drm");
    if (fs::exists(drmRoot, ec))
    {
        std::vector<fs::path> nodes;
        for (const auto& entry : fs::directory_iterator(drmRoot, ec))
        {
            const std::string name = entry.path().filename().string();
            if (name.starts_with("renderD"))
                nodes.push_back(entry.path());
        }
        std::sort(nodes.begin(), nodes.end());

        for (const auto& nodePath : nodes)
        {
            const fs::path devLink = nodePath / "device";
            const fs::path canonicalDev = fs::weakly_canonical(devLink, ec);
            const std::string pciAddr   = canonicalDev.filename().string();
            const std::string vendor    = read_trimmed_line(devLink / "vendor");
            const std::string devId     = read_trimmed_line(devLink / "device");

            // Skip NVIDIA DRM nodes here if already enumerated via CUDA above
            if (vendor == "0x10de")
                continue;

            std::string driverName = "unknown";
            const fs::path drvCanon = fs::weakly_canonical(devLink /
                                                           "driver", ec);
            if (!drvCanon.empty())
                driverName = drvCanon.filename().string();

            // Intel iGPUs always reside on root complex PCI
            // 0000:00:02.0.  AMD APUs sit on bus 00 without dedicated
            // VRAM (mem_info_vram_total).
            bool isIntegrated = pciAddr.ends_with(":00:02.0");
            if (!isIntegrated && pciAddr.starts_with("0000:00:"))
                isIntegrated = !fs::exists(devLink / "mem_info_vram_total", ec);

            uint64_t vram = read_u64_file(devLink / "mem_info_vram_total");
            if (vram == 0)
                vram = read_u64_file(devLink / "lmem_total_bytes");
            if (vram == 0)
                vram = read_u64_file(devLink /
                             "tile0/gt0/mem_regions/vram0/physical_size");

            DetectedGpu g;
            g.kind       = isIntegrated ? DetectedGpu::Kind::Integrated
                           : DetectedGpu::Kind::Discrete;
            g.backend    = DetectedGpu::Backend::Drm;
            g.device     = nodePath.filename().string(); // e.g. "renderD128"
            g.pciAddr    = pciAddr;
            g.vendorId   = vendor;
            g.deviceId   = devId;
            g.driverName = driverName;
            g.vramBytes  = vram;

            const std::string vramStr = (vram > 0)
                                        ? fmt::format(", VRAM={} MiB",
                                                      vram / (1024 * 1024))
                                        : "";
            g.label = fmt::format("{} {} (/dev/dri/{}, PCI {}, dev={}, driver={}{})",
                                  vendor_pretty_name(vendor),
                                  isIntegrated ? "iGPU" : "dGPU",
                                  g.device, pciAddr, devId,
                                  driverName, vramStr);
            gpus.push_back(std::move(g));
        }
    }

    return gpus;
}
} // namespace

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
        m_log->error("Failed to open video encoder.");
        Shutdown();
        return;
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
    m_display_primaries->has_primaries = color.has_primaries ? 1 : 0;
    m_display_primaries->has_luminance = color.has_luminance ? 1 : 0;

    // FFmpeg order: 0 = Red, 1 = Green, 2 = Blue
    constexpr size_t FF_R = 0, FF_G = 1, FF_B = 2;
    constexpr size_t X = 0, Y = 1;

    // CTA-861.3 allows primaries in any order:
    // Red has largest x, Green has largest y, Blue is the remaining one.
    size_t idx_r = 0;
    size_t idx_g = 0;
    for (size_t i = 1; i < 3; ++i)
    {
        if (color.display_primaries[i][X].num >
            color.display_primaries[idx_r][X].num)
            idx_r = i;
        if (color.display_primaries[i][Y].num >
            color.display_primaries[idx_g][Y].num)
            idx_g = i;
    }

    if (idx_r == idx_g)
    {
        // Fallback to SMPTE ST 2086 order (G=0, B=1, R=2) if degenerate
        idx_g = 0;
        idx_r = 2;
    }
    size_t idx_b = 3 - idx_r - idx_g;

    m_display_primaries->display_primaries[FF_R][X] =
        color.display_primaries[idx_r][X];
    m_display_primaries->display_primaries[FF_R][Y] =
        color.display_primaries[idx_r][Y];

    m_display_primaries->display_primaries[FF_G][X] =
        color.display_primaries[idx_g][X];
    m_display_primaries->display_primaries[FF_G][Y] =
        color.display_primaries[idx_g][Y];

    m_display_primaries->display_primaries[FF_B][X] =
        color.display_primaries[idx_b][X];
    m_display_primaries->display_primaries[FF_B][Y] =
        color.display_primaries[idx_b][Y];

    std::copy(std::begin(color.white_point),
              std::end(color.white_point),
              m_display_primaries->white_point);

    m_display_primaries->max_luminance = color.max_luminance;
    m_display_primaries->min_luminance = color.min_luminance;

    m_content_light->MaxCLL  = color.MaxCLL;
    m_content_light->MaxFALL = color.MaxFALL;
}

bool VideoStream::ResolveGpu(Args& args, int verbose_level)
{
    auto log = spdlog::get("app_logger");
    const auto gpus = enumerate_system_gpus();

    if (verbose_level > 2 && log)
    {
        log->info("Detected {} GPU device(s):", gpus.size());
        for (const auto& g : gpus)
            log->info("  - {}", g.label);
    }

    auto inferCodecForGpu = [&](const DetectedGpu& g)
    {
        if (args.codecExplicit)
            return;

        if (g.backend == DetectedGpu::Backend::Nvidia)
            args.codecName = "hevc_nvenc";
        else if (g.vendorId == "0x1002") // AMD
            args.codecName = "hevc_vaapi";
        else // Intel
            args.codecName = "hevc_qsv";
    };

    // Rule 1 for Auto: If the user explicitly passed -d / --device,
    // honor it directly.
    if (args.gpuPref == GpuPreference::Auto && args.deviceExplicit)
    {
        std::error_code ec;
        std::string resolvedNode = args.device;
        if (args.device.starts_with('/'))
        {
            const auto canon = std::filesystem::weakly_canonical(args.device,
                                                                 ec);
            if (!ec && canon.filename().string().starts_with("renderD"))
                resolvedNode = canon.filename().string();
        }

        auto it = std::find_if(gpus.begin(), gpus.end(),
                               [&](const DetectedGpu& g) {
            return g.device == resolvedNode ||
                   ("/dev/dri/" + g.device) == args.device;
        });

        if (it != gpus.end())
        {
            args.device = it->device;
            inferCodecForGpu(*it);
            if (verbose_level > 2 && log)
                log->info("Selected user-specified GPU: {} [codec={}]",
                          it->label, args.codecName);
        }
        else
        {
            if (!args.codecExplicit)
            {
                const bool isNumeric = !args.device.empty() &&
                                       std::all_of(args.device.begin(),
                                                   args.device.end(),
                                                   [](unsigned char c)
                                           { return std::isdigit(c) != 0; });

                args.codecName = isNumeric ? "hevc_nvenc" : "hevc_qsv";
            }
            if (verbose_level > 2 && log)
                log->info("Selected user-specified device: '{}' [codec={}]",
                          args.device, args.codecName);
        }
        return true;
    }

    auto matchesExplicitCodec = [&](const DetectedGpu& g) -> bool
    {
        if (!args.codecExplicit)
            return true;

        if (args.codecName.find("nvenc") != std::string::npos)
            return g.backend == DetectedGpu::Backend::Nvidia;
        if (args.codecName.find("qsv") != std::string::npos)
            return g.backend == DetectedGpu::Backend::Drm &&
                g.vendorId == "0x8086";
        if (args.codecName.find("vaapi") != std::string::npos)
            return g.backend == DetectedGpu::Backend::Drm;
        return true;
    };

    auto findFirst = [&](DetectedGpu::Kind targetKind) -> const DetectedGpu*
    {
        for (const auto& g : gpus)
        {
            if (g.kind == targetKind && matchesExplicitCodec(g))
                return &g;
        }
        return nullptr;
    };

    const DetectedGpu* chosen = nullptr;
    if (args.gpuPref == GpuPreference::IGPU)
    {
        chosen = findFirst(DetectedGpu::Kind::Integrated);
        if (!chosen)
            chosen = findFirst(DetectedGpu::Kind::Discrete);
    }
    else // GpuPreference::Auto or GpuPreference::DGPU
    {
        chosen = findFirst(DetectedGpu::Kind::Discrete);
        if (!chosen)
            chosen = findFirst(DetectedGpu::Kind::Integrated);
    }

    if (!chosen)
    {
        if (log)
        {
            log->error("No compatible GPU found for --gpu {} (codec='{}').",
                       args.gpuPref == GpuPreference::IGPU ? "iGPU" :
                       args.gpuPref == GpuPreference::DGPU ? "dGPU" : "auto",
                       args.codecName);
        }
        return false;
    }

    args.device = chosen->device;
    inferCodecForGpu(*chosen);

    if (verbose_level > 2 && log)
    {
        const char* modeStr = (args.gpuPref == GpuPreference::IGPU)
                              ? "iGPU"
                              : (args.gpuPref == GpuPreference::DGPU)
                                ? "dGPU"
                                : "auto";
        log->info("Selected GPU (--gpu {}): {} [codec={}]",
                  modeStr, chosen->label, args.codecName);
    }

    return true;
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

    m_encoder = make_shared_codec_context(video_codec);
    if (!m_encoder)
    {
        m_log->error("Failed to allocate unique video encoding context.");
        return false;
    }

    // Assign hardware metrics to the active encoder context
    m_encoder->codec_id = video_codec->id;
    m_encoder->width = m_params.width;
    m_encoder->height = m_params.height;

    if (m_params.pix_fmt != AV_PIX_FMT_NV12 &&
        m_params.pix_fmt != AV_PIX_FMT_P010LE)
    {
        m_log->error("Unsupported input pixel format: {}",
                     av_get_pix_fmt_name(m_params.pix_fmt));
        return false;
    }

    if (m_params.pix_fmt == AV_PIX_FMT_P010LE &&
        m_encoder->codec_id == AV_CODEC_ID_HEVC)
    {
        m_encoder->profile = AV_PROFILE_HEVC_MAIN_10;
    }

    // Do NOT set AV_CODEC_FLAG_GLOBAL_HEADER for MPEG-TS so Annex-B
    // parameter sets (VUI) and HDR SEI messages are emitted in-band.
    AVRational encoder_fps = AVRational {
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

    if (m_verbose > 4)
    {
        m_log->info("Encoder VUI Init: primaries={} ({}), trc={} ({}), "
                    "space={} ({}), range={} ({})",
                    static_cast<int>(m_encoder->color_primaries),
                    av_color_primaries_name(m_encoder->color_primaries),
                    static_cast<int>(m_encoder->color_trc),
                    av_color_transfer_name(m_encoder->color_trc),
                    static_cast<int>(m_encoder->colorspace),
                    av_color_space_name(m_encoder->colorspace),
                    static_cast<int>(m_encoder->color_range),
                    av_color_range_name(m_encoder->color_range));
    }

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

    if (!success)
    {
        if (local_opt != nullptr)
            av_dict_free(&local_opt);

        return false;
    }

    start_work();

    if (local_opt != nullptr)
        av_dict_free(&local_opt);

    return true;
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

        std::string device;
        if (m_args.device.empty() || m_args.device.starts_with("renderD"))
            device = "0";
        else
            device = m_args.device;

        ret = av_hwdevice_ctx_create(&raw_hw_ctx,
                                     AV_HWDEVICE_TYPE_CUDA,
                                     device.c_str(),
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
     * m_params.pix_fmt will be:
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
    frames_ctx->sw_format = m_params.pix_fmt;
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
                    av_get_pix_fmt_name(m_params.pix_fmt));
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

    string child_device = m_args.device.starts_with('/')
                              ? m_args.device
                              : ("/dev/dri/" + m_args.device);

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
    frames_ctx->sw_format = m_params.pix_fmt;

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
    string child_device = m_args.device.starts_with('/')
                          ? m_args.device
                          : ("/dev/dri/" + m_args.device);

    if (m_hw_device_ctx == nullptr)
    {
        setenv("LIBVA_DRIVER_NAME", "iHD", 0);
        setenv("LIBVA_MESSAGING_LEVEL", "0", 1);

        AVBufferRef* raw_hw_ctx = nullptr;
        AVDictionary* dev_opt = nullptr;
        av_dict_set(&dev_opt, "child_device", child_device.c_str(), 0);
        ret = av_hwdevice_ctx_create(&raw_hw_ctx,
                                     AV_HWDEVICE_TYPE_QSV,
                                     "auto", dev_opt, 0);
        av_dict_free(&dev_opt);

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
    frames_ctx->sw_format = m_params.pix_fmt;
    frames_ctx->initial_pool_size = (m_args.num_threads * m_args.buffers)
                                    + m_args.extraHWframes
                                    + m_args.lookahead + 4;

#if defined(HAS_MAGEWELL_QSV_SUPPORT) && (LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 0, 0))
    // This code only compiles if the hardware/library dependencies exist system-wide
    if (frames_ctx->hwctx)
    {
        AVQSVFramesContext* qsv_hwctx =
            reinterpret_cast<AVQSVFramesContext*>(frames_ctx->hwctx);
        qsv_hwctx->frame_type = MFX_MEMTYPE_VIDEO_MEMORY_PROCESSOR_TARGET;
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
    ret = avcodec_open2(m_encoder.get(), codec, &opt);
    if (opt)
        av_dict_free(&opt);

    if (ret < 0)
    {
        m_log->error("Fatal Error: Intel QSV codec activation failed: {}",
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
            if (hw_frame == nullptr)
                continue;
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
    BufferRefPtr hw_frames_ctx(av_buffer_ref(m_hw_frames_ctx.get()));

    if (!hw_frames_ctx)
    {
        m_log->error("{} failed to reference QSV frame context.",
                     worker.name);
        Shutdown();
        return;
    }

    auto* hw_ctx = reinterpret_cast<AVHWFramesContext*>(hw_frames_ctx->data);
    bool  first = true;

    m_log->info("Started {} worker thread", worker.name);

    while (worker.running.load() && m_running.load())
    {
        Image image;

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
        }

        // Helper lambda: Runs when called inside an error 'if' block below
        auto drop_image = [&]() {
            f_image_avail(image.pImage, image.pEco);
            {
                std::scoped_lock lock(worker.mtx);
                worker.frames.push_back(nullptr);
            }
            worker.frame_avail.notify_all();
        };

        FramePtr hw = make_frame();
        if (!hw)
        {
            m_log->error("{} failed to allocate hardware frame wrapper.",
                         worker.name);
            drop_image();
            continue;
        }

        int ret = AVERROR(ENOMEM);

        while (worker.running.load() && m_running.load())
        {
            ret = av_hwframe_get_buffer(hw_frames_ctx.get(), hw.get(), 0);
            if (ret == 0)
                break;

            if (ret != AVERROR(ENOMEM))
            {
                m_log->error("{} worker failed to grab hardware pool surface: {}",
                             worker.name, AVerr2str(ret));
                Shutdown();
                break;
            }

            m_log->warn("{} worker failed to grab hardware pool surface: {}. "
                        "Will retry.",
                        worker.name, AVerr2str(ret));

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        if (ret < 0)
        {
            drop_image();
            continue;
        }

        FramePtr cpu_frame = make_frame();
        if (!cpu_frame)
        {
            m_log->warn("Failed to allocate local CPU frame wrapper.");
            drop_image();
            continue;
        }

        cpu_frame->format = m_params.pix_fmt;
        cpu_frame->width  = hw_ctx->width;
        cpu_frame->height = hw_ctx->height;

        ret = av_image_fill_arrays(cpu_frame->data,
                                   cpu_frame->linesize,
                                   image.pImage,
                                   m_params.pix_fmt,
                                   cpu_frame->width,
                                   cpu_frame->height,
                                   1);

        if (ret < 0)
        {
            m_log->error("{} failed to setup CPU frame: {}",
                         worker.name, AVerr2str(ret));
            drop_image();
            continue;
        }

        if (cpu_frame->format == AV_PIX_FMT_P010LE ||
            cpu_frame->format == AV_PIX_FMT_NV12)
        {
            cpu_frame->linesize[0] = m_params.stride;
            cpu_frame->linesize[1] = m_params.stride;

            cpu_frame->data[1] = cpu_frame->data[0] +
                                 cpu_frame->linesize[0] * cpu_frame->height;
        }
        else
        {
            m_log->error("{} unsupported capture pixel format: {}",
                         worker.name,
                         av_get_pix_fmt_name(static_cast<AVPixelFormat>
                                             (cpu_frame->format)));
            drop_image();
            continue;
        }

        cpu_frame->extended_data = cpu_frame->data;

        if (first) [[unlikely]]
        {
            // QSV's first frame transfer performs shared initialization which
            // must not be performed concurrently by multiple copy workers.
            std::lock_guard lock(m_transfer_mutex);
            ret = av_hwframe_transfer_data(hw.get(), cpu_frame.get(), 0);
            if (ret == 0)
                first = false;
        }
        else
        {
            ret = av_hwframe_transfer_data(hw.get(), cpu_frame.get(), 0);
        }

        if (ret < 0) [[unlikely]]
        {
            m_log->warn("{}{} av_hwframe_transfer_data failed: "
                        "{}", m_args.damagedPrefix,
                        worker.name, AVerr2str(ret));

            m_log->warn("{} transfer failed: "
                        "ctx={}x{} sw={} | "
                        "hw={}x{} | "
                        "cpu={}x{} fmt={} ls={}/{} | "
                        "params={}x{} m_sw={}",
                        worker.name,
                        hw_ctx->width, hw_ctx->height,
                        av_get_pix_fmt_name(static_cast<AVPixelFormat>(hw_ctx->sw_format)),
                        hw->width, hw->height,
                        cpu_frame->width, cpu_frame->height,
                        av_get_pix_fmt_name(static_cast<AVPixelFormat>(cpu_frame->format)),
                        cpu_frame->linesize[0], cpu_frame->linesize[1],
                        m_params.width, m_params.height,
                        av_get_pix_fmt_name(m_params.pix_fmt));

            this_thread::sleep_for(chrono::milliseconds(3));
            drop_image();
            continue;
        }

        f_image_avail(image.pImage, image.pEco);

        hw->colorspace      = m_params.color.space;
        hw->color_primaries = m_params.color.primaries;
        hw->color_trc       = m_params.color.trc;
        hw->color_range     = m_params.color.range;
        hw->pts = av_rescale_q(image.timestamp, TimeBase::Magewell,
                               m_encoder->time_base);

        // Handle HDR metadata attachments
        if (m_params.color.is_HDR)
        {
            // Attach mastering display metadata if the color space
            // natively provides it (HDR10/PQ)
            if (m_display_primaries && m_params.color.has_primaries &&
                m_params.color.has_luminance)
            {
                av_frame_remove_side_data(hw.get(),
                                    AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
                AVMasteringDisplayMetadata* primaries =
                    av_mastering_display_metadata_create_side_data(hw.get());
                if (primaries)
                    *primaries = *m_display_primaries;
            }

            // Attach max content light levels if explicitly declared
            // by the source
            if (m_content_light && (m_params.color.MaxCLL > 0 ||
                                    m_params.color.MaxFALL > 0))
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
    const size_t num_buffers = m_args.buffers;

    std::unique_lock workers_lock(m_workers_mutex);

    if (!m_running.load() || m_workers.empty()) [[unlikely]]
    {
        workers_lock.unlock();
        f_image_avail(image.pImage, image.pEco);
        return;
    }

    CopyThread& worker = m_workers[m_next_capture_worker];

    m_next_capture_worker =
        (m_next_capture_worker + 1) % m_workers.size();

    // Acquire this before releasing workers_lock so stop_work()
    // cannot clear m_workers while we are using this worker.
    std::unique_lock worker_lock(worker.mtx);

    workers_lock.unlock();

    worker.WaitForSpace(worker_lock, m_running, num_buffers);

    if (!m_running.load())
    {
        f_image_avail(image.pImage, image.pEco);
        return;
    }

    worker.images.push_back(std::move(image));
    worker_lock.unlock();

    worker.image_avail.notify_one();
}
