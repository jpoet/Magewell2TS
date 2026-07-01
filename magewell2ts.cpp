/*
 * Copyright (c) 2022-2026 John Patrick Poet
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <string_view>
#include <iostream>
#include <charconv>
#include <csignal>
#include <memory>
#include <format>

#include <fcntl.h>
#include <unistd.h>
#include <cstdio>

#include <vector>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>
#include "spdlog/sinks/rotating_file_sink.h"
#include <spdlog/sinks/stdout_color_sinks.h>

#include "Magewell.h"
#include "version.h"

using namespace std;

std::shared_ptr<spdlog::logger> logger;
Magewell* g_mw;

void Shutdown(void)
{
    g_mw->Shutdown();
}

void signal_handler(int signum)
{
    if (signum == SIGHUP || signum == SIGUSR1)
    {
#if 0
        g_mw->Reset();
#endif
    }
    else if (signum == SIGINT || signum == SIGTERM)
    {
        const char* msg = "Received SIGINT/SIGTERM.\n";
        write(STDERR_FILENO, msg, strlen(msg));
        g_mw->Shutdown();
    }
    else
    {
        const char* msg = "Unhandled interrupt.\n";
        write(STDERR_FILENO, msg, strlen(msg));
    }
}

void show_help(string_view app)
{
    clog << format("{} Version: {}\n", app, project::version::full_version);

    clog << "\n"
         << "Defaults in []:\n"
         << "\n";

    clog << "--board (-b)       : board id, if you have more than one [0]\n"
         << "--device (-d)      : vaapi/qsv device (e.g. renderD129) [renderD128]\n"
         << "--input (-i)       : input idx, *required*. Starts at 1\n"
         << "--settle-time (-s) : How long to wait for signal changes to 'settle' [99(ms)]\n"
         << "--list (-l)        : List capture card inputs\n"
         << "--mux (-m)         : capture audio and video and mux into TS [false]\n"
         << "--no-audio (-n)    : Only capture video. [false]\n"
         << "--read-edid (-r)   : Read EDID info for input to file\n"
         << "--logfile          : Also log messages to the given file\n"
         << "--color (-o)       : Use color when logging to the console\n"
         << "--verbose (-v)     : message verbose level. 0=completely quiet [1]\n"
         << "--video-codec (-c) : Video codec name (e.g. hevc_qsv, h264_nvenc) [hevc_qsv]\n"
         << "--lookahead (-a)   : How many frames to 'look ahead' [35]\n"
         << "--quality (-q)     : quality setting [25]\n"
         << "--preset (-p)      : encoder preset\n"
         << "--p010             : Force p010 (10bit) video format.\n"
         << "--gop_secs (-g)    : GOP size in seconds [1.5] (0 to disable)\n"
         << "--idr-interval     : Frequency that keyframe will be IDR [0]\n"
         << "--gpu-buffers      : GPU video buffers count [4]\n"
         << "--video-buffers    : Video buffers count (RAM) [12]\n"
         << "--extra-hw-frames  : Extra HW frames used for encoding [32]\n"
         << "--write-edid (-w)  : Write EDID info from file to input\n"
         << "--wait-for         : Wait for given number of inputs to be initialized. 10 second timeout\n"
         << "--realtime         : Enable real-time priority threads.\n";

    clog << "\n"
         << "Examples:\n"
         << "\tCapture from input 2 and write Transport Stream to stdout:\n"
         << "\t" << app << " -i 2 -m\n"
         << "\n"
         << "\tWrite EDID to input 3 and capture audio and video:\n"
         << "\t" << app << " -i 3 -w ProCaptureHDMI-EAC3.bin -m\n"
         << "\n"
         << "\tUse the iHD vaapi driver to encode h.264 video and pipe it to mpv:\n"
         << "\t" << app << " -i 1 -m -n -c h264_vaapi | mpv -\n"
         << "\n"
         << "\tUse Intel quick-sync to encode h.265 video and pipe it to mpv:\n"
         << "\t" << app << " -b 1 -i 1 -m -n -c hevc_qsv | mpv -\n";

    clog << "\nIntel notes:\n"
         << "  --extra-hw-frames is equivalent to passing that argument\n"
         << "    to ffmpeg. If the value is too low the resulting video can\n"
         << "    have artifacts and it will usually result in dropped frames.\n"
         << "    It is strongly recommended to set this to least 32.\n";

    clog << "\nNOTE: setting EDID does not survive a reboot.\n";
}

bool string_to_int(string_view st, int &value, string_view var)
{
    auto result = from_chars(st.data(), st.data() + st.size(),
                             value);
    if (result.ec == errc::invalid_argument)
    {
        cerr << "Invalid " << var << ": " << st << endl;
        value = -1;
        return false;
    }

    return true;
}

bool string_to_float(string_view st, float &value, string_view var)
{
    auto result = from_chars(st.data(), st.data() + st.size(),
                             value);
    if (result.ec == errc::invalid_argument)
    {
        cerr << "Invalid " << var << ": " << st << endl;
        value = -1;
        return false;
    }

    return true;
}

void setup_logging(int verbose_level, bool color, const string& logpath)
{
    // Create console sink
    std::shared_ptr<spdlog::sinks::sink> console_sink;

    if (color)
    {
        console_sink =
            std::make_shared<spdlog::sinks::stderr_color_sink_mt>();

        console_sink->set_pattern("%H:%M:%S.%e %^[%l]%$ : %v");
    }
    else
    {
        console_sink =
            std::make_shared<spdlog::sinks::stderr_sink_mt>();

        console_sink->set_pattern("%l: %v");
    }

    // Set console level based on verbose level
    if (verbose_level < 1)
        console_sink->set_level(spdlog::level::off);
    else if (verbose_level < 4)
        console_sink->set_level(spdlog::level::info);
    else if (verbose_level == 4)
        console_sink->set_level(spdlog::level::debug);
    else
        console_sink->set_level(spdlog::level::trace);

    // Create file sink if logpath is specified
    std::shared_ptr<spdlog::sinks::sink> file_sink;
    if (!logpath.empty())
    {
        try
        {
            filesystem::path path(logpath);
            if (!path.parent_path().empty())
                filesystem::create_directories(path.parent_path());

            file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>
                        (logpath, // filename
                         1024 * 1024 * 3,  // max size (4 MB)
                         3,                // max files
                         false     // rotate on open (optional, default false)
                         );
            file_sink->set_level(spdlog::level::trace);
            file_sink->set_pattern("%Y-%m-%d %H:%M:%S.%e [%l] %v");

            // Combine sinks into a vector
            std::vector<spdlog::sink_ptr> sinks {console_sink, file_sink};
            logger = std::make_shared<spdlog::logger>("app_logger",
                                                      begin(sinks), end(sinks));
        }
        catch (const filesystem::filesystem_error& e)
        {
            cerr << "Error creating log file: " << e.what() << '\n';
        }
    }
    else
    {
        logger = std::make_shared<spdlog::logger>("app_logger",
                      spdlog::sinks_init_list{console_sink});
    }

    // Set logger level based on verbose level
    if (verbose_level < 1)
        logger->set_level(spdlog::level::off);
    else if (verbose_level < 5)
        logger->set_level(spdlog::level::info);
    else if (verbose_level == 5)
        logger->set_level(spdlog::level::debug);
    else
        logger->set_level(spdlog::level::trace);

    spdlog::register_logger(logger);
    spdlog::flush_on(spdlog::level::warn);
    spdlog::flush_every(std::chrono::seconds(11)); // Flush every 5 seconds.

    // Set default logger
    spdlog::set_default_logger(logger);
}

int main(int argc, char* argv[])
{
    int    ret = 0;
    int    boardId  = -1;
    int    devIndex = -1;
    chrono::milliseconds settle_time {99};

    string      logpath;
    int         verbose_level = 1;
    bool        color = false;
    bool        realtime = false;

    string_view app_name = argv[0];
    string      edid_file;

    bool        list_inputs = false;
    bool        do_capture  = false;
    bool        read_edid   = false;
    bool        write_edid  = false;
    bool        no_audio      = false;

    int         video_buffers = 12;
    VideoStream::Args  video_args;


    // Attempt to set output PIPE to 1 Megabyte
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int max_pipe_size = 1048576;
    // Force the STDOUT file descriptor pipe buffer to expand
    if (fcntl(STDOUT_FILENO, F_SETPIPE_SZ, max_pipe_size) < 0)
    {
        // Could not set pipe size, so must be file mode.
        constexpr size_t BUFFER_SIZE = 100 * 1024 * 1024;
        std::vector<char> buffer(BUFFER_SIZE);

        // Set stdout to use this buffer (fully buffered)
        if (std::setvbuf(stdout, buffer.data(), _IOFBF, BUFFER_SIZE) != 0)
        {
            cerr << "Failed to allocate large buffer for stdout\n";
            return 1;
        }
    }

    vector<string_view> args(argv + 1, argv + argc);

    {
        struct sigaction action;
        action.sa_handler = signal_handler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        sigaction(SIGINT, &action, NULL);
        sigaction(SIGTERM, &action, NULL);
        sigaction(SIGHUP, &action, NULL);
        sigaction(SIGUSR1, &action, NULL);
    }

    for (auto iter = args.begin(); iter != args.end(); ++iter)
    {
        if (*iter == "-h" || *iter == "--help")
        {
            show_help(app_name);
            return 0;
        }
        else if (*iter == "--logfile")
        {
            logpath = *(++iter);
        }
        else if (*iter == "-l" || *iter == "--list")
        {
            list_inputs = true;
        }
        else if (*iter == "-p" || *iter == "--preset")
        {
            video_args.preset = *(++iter);
        }
        else if (*iter == "-q" || *iter == "--quality")
        {
            if (!string_to_int(*(++iter), video_args.quality, "quality"))
                exit(1);
        }
        else if (*iter == "-a" || *iter == "--lookahead")
        {
            if (!string_to_int(*(++iter), video_args.lookahead, "lookahead"))
                exit(1);
        }
        else if (*iter == "-g" || *iter == "--gop-secs")
        {
            if (!string_to_float(*(++iter), video_args.gopSecs, "gop-secs"))
                exit(1);
        }
        else if (*iter == "--idr-interval")
        {
            if (!string_to_int(*(++iter), video_args.idrInterval,
                               "idr-interval"))
                exit(1);
        }
        else if (*iter == "-m" || *iter == "--mux")
        {
            do_capture = true;
        }
        else if (*iter == "-i" || *iter == "--input")
        {
            if (!string_to_int(*(++iter), devIndex, "device index"))
                exit(1);
        }
        else if (*iter == "-b" || *iter == "--board")
        {
            if (!string_to_int(*(++iter), boardId, "board id"))
                exit(1);
        }
        else if (*iter == "-c" || *iter == "--video-codec")
        {
            video_args.codecName = *(++iter);
        }
        else if (*iter == "-r" || *iter == "--read-edid")
        {
            read_edid = true;
            edid_file = *(++iter);
        }
        else if (*iter == "-w" || *iter == "--write-edid")
        {
            write_edid = true;
            edid_file = *(++iter);
        }
        else if (*iter == "-n" || *iter == "--no-audio")
        {
            no_audio = true;
        }
        else if (*iter == "--p010")
        {
            video_args.p010 = true;
        }
        else if (*iter == "-d" || *iter == "--device")
        {
            video_args.device = *(++iter);
        }
        else if (*iter == "-s" || *iter == "--settle-time")
        {
            int ms;
            if (!string_to_int(*(++iter), ms, "Settle time"))
                exit(1);
            settle_time = chrono::milliseconds(ms);
        }
        else if (*iter == "--gpu-buffers")
        {
            if (!string_to_int(*(++iter), video_args.buffers,
                               "GPU buffers"))
                exit(1);
        }
        else if (*iter == "--video-buffers")
        {
            if (!string_to_int(*(++iter), video_buffers,
                               "Video buffers"))
                exit(1);
        }
        else if (*iter == "--extra-hw-frames")
        {
            if (!string_to_int(*(++iter), video_args.extraHWframes,
                               "Extra HW frames"))
                exit(1);
        }
        else if (*iter == "--wait-for")
        {
            int input_count;
            if (!string_to_int(*(++iter), input_count, "input count"))
                exit(1);
            g_mw->WaitForInputs(input_count);
        }
        else if (*iter == "-o" || *iter == "--color")
        {
            color = true;
        }
        else if (*iter == "-v" || *iter == "--verbose")
        {
            int v;
            if (iter + 1 == args.end())
                v = 1;
            else
            {
                if (!string_to_int(*(++iter), v, "verbose"))
                    exit(1);
            }
            verbose_level = v;
        }
        else if (*iter == "--version")
        {
            clog << format("Version: {}\n", project::version::full_version);
            exit(0);
        }
        else if (*iter == "--realtime")
        {
            realtime = true;
        }
        else
        {
            cerr << "Unrecognized option " << *iter << endl;
            exit(1);
        }
    }

    // Initialize logging
    setup_logging(verbose_level, color, logpath);

    string argstr;
    for (int idx = 0; idx < argc; ++idx)
        argstr += format("{} ", argv[idx]);
    argstr += format("[version {}]", project::version::full_version);
    logger->critical(argstr);

    g_mw = new Magewell;
    if (!g_mw)
        return -1;
    g_mw->Verbose(verbose_level);

    if (list_inputs)
        g_mw->ListInputs();

    if (devIndex < 1)
        return 0;

    if (!g_mw->OpenChannel(devIndex - 1, boardId))
        return -1;

    if (!edid_file.empty())
    {
        if (read_edid)
        {
            if (!g_mw->ReadEDID(edid_file))
                return -1;
        }
        else if (write_edid)
        {
            if (!g_mw->WriteEDID(edid_file))
                return -1;
        }
    }

    if (do_capture)
    {
        if (!g_mw->Capture(std::move(video_args), no_audio,
                           settle_time, video_buffers, realtime))
            return -2;
    }

    std::fflush(stdout);

    delete g_mw;
    spdlog::shutdown();
    return ret;
}
