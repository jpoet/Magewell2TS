/*
 * magewell2ts
 * Copyright (c) 2022 John Patrick Poet
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
 *
 *
 * Based on AudioCapture.cpp,CaptureByInput.cpp and ReadWriteEDID.cpp
 * by magewell:
 *
 * THE SOFTWARE PROVIDED BY MAGEWELL “AS IS” AND ANY EXPRESS, INCLUDING
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL
 * MAGEWELL BE LIABLE
 *
 * FOR ANY CLAIM, DIRECT OR INDIRECT DAMAGES OR OTHER LIABILITY, WHETHER
 * IN CONTRACT, TORT OR OTHERWISE, ARISING IN ANY WAY OF USING THE
 * SOFTWARE.
 *
 * CONTACT INFORMATION:
 * SDK@magewell.net
 * http://www.magewell.com/
 */

#include <string_view>
#include <iostream>
#include <charconv>
#include <csignal>

#include <vector>

#include "Magewell.h"
#include "lock_ios.h"
#include "version.h"

using namespace std;
using namespace s6_lock_ios;

Magewell g_mw;

void Shutdown(void)
{
    g_mw.Shutdown();
}

void signal_handler(int signum)
{
    if (signum == SIGHUP || signum == SIGUSR1)
    {
        cerr << "\n\nResetting audio.\n" << endl;
        g_mw.Reset();
    }
    else if (signum == SIGINT || signum == SIGTERM)
    {
        cerr << "Received SIGINT/SIGTERM." << endl;
        g_mw.Shutdown();
    }
    else
        cerr << "Unhandled interrupt." << endl;
}

void show_help(string_view app)
{
    cerr << app << endl;

    cerr << "\n"
         << "Defaults in []:\n"
         << "\n";

    cerr << "--board (-b)       : board id, if you have more than one [0]\n"
         << "--device (-d)      : vaapi/qsv device (e.g. renderD129) [renderD128]\n"
         << "--get-volume (-g)  : Display volume settings for each channel of input\n"
         << "--input (-i)       : input idx, *required*. Starts at 1\n"
         << "--list (-l)        : List capture card inputs\n"
         << "--mux (-m)         : capture audio and video and mux into TS [false]\n"
         << "--no-audio (-n)    : Only capture video. [false]\n"
         << "--read-edid (-r)   : Read EDID info for input to file\n"
         << "--set-volume (-s)  : Set volume for all channels of the input\n"
         << "--verbose (-v)     : message verbose level. 0=completely quiet [1]\n"
         << "--video-codec (-c) : Video codec name (e.g. hevc_qsv, h264_nvenc) [hevc_nvenc]\n"
         << "--lookahead (-a)   : How many frames to 'look ahead' [35]\n"
         << "--quality (-q)     : quality setting [25]\n"
         << "--preset (-p)      : encoder preset\n"
         << "--write-edid (-w)  : Write EDID info from file to input\n"
         << "--wait-for         : Wait for given number of inputs to be initialized. 10 second timeout\n";

    cerr << "\n"
         << "Examples:\n"
         << "\tCapture from input 2 and write Transport Stream to stdout:\n"
         << "\t" << app << " -i 2 -m\n"
         << "\n"
         << "\tWrite EDID to input 3 and capture audio and video:\n"
         << "\t" << app << " -i 3 -w ProCaptureHDMI-EAC3.bin -m\n"
         << "\n"
         << "\tSet Volume of input 1 to max and capture to TS:\n"
         << "\t" << app << " -i 1 -s 100 -m\n"
         << "\n"
         << "\tUse the iHD vaapi driver to encode h264 video and pipe it to mpv:\n"
         << "\t" << app << " ./magewell2ts -i 1 -m -n -c h264_qsv | mpv -\n";

    cerr << "\nNOTE: setting EDID does not survive a reboot.\n";
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

int main(int argc, char* argv[])
{
//    parse_cmd(argc, argv);

    int    ret = 0;
    int    boardId  = -1;
    int    devIndex = -1;

    string_view app_name = argv[0];
    string      edid_file;
    string      video_codec = "hevc_nvenc";
    string      device      = "renderD128";

    bool        get_volume  = false;
    int         set_volume  = -1;

    bool        list_inputs = false;
    bool        do_capture  = false;
    bool        read_edid   = false;
    bool        write_edid  = false;

    string      preset;
    int         quality     = 25;
    int         look_ahead  = -1;
    bool        no_audio    = false;

    std::cerr << mutex_init_own;

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
        else if (*iter == "-l" || *iter == "--list")
        {
            list_inputs = true;
        }
        else if (*iter == "-p" || *iter == "--preset")
        {
            preset = *(++iter);
        }
        else if (*iter == "-q" || *iter == "--quality")
        {
            if (!string_to_int(*(++iter), quality, "quality"))
                exit(1);
        }
        else if (*iter == "-a" || *iter == "--lookahead")
        {
            if (!string_to_int(*(++iter), look_ahead, "lookahead"))
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
            video_codec = *(++iter);
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
        else if (*iter == "-g" || *iter == "--get-volume")
        {
            get_volume = true;
        }
        else if (*iter == "-s" || *iter == "--set-volume")
        {
            if (!string_to_int(*(++iter), set_volume, "volume"))
                exit(1);
        }
        else if (*iter == "-n" || *iter == "--no-audio")
        {
            no_audio = true;
        }
        else if (*iter == "-d" || *iter == "--device")
        {
            device = *(++iter);
        }
        else if (*iter == "--wait-for")
        {
            int input_count;
            if (!string_to_int(*(++iter), input_count, "input count"))
                exit(1);
            g_mw.WaitForInputs(input_count);
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
            g_mw.Verbose(v);
        }
        else
        {
            cerr << "Unrecognized option '" << *iter << "'\n";
            exit(1);
        }
    }

    if (!g_mw)
        return -1;

    if (list_inputs)
        g_mw.ListInputs();

    if (devIndex < 1)
        return 0;

    if (!g_mw.OpenChannel(devIndex - 1, boardId))
        return -1;

    if (get_volume)
        g_mw.DisplayVolume();
    if (set_volume >= 0)
        if (!g_mw.SetVolume(set_volume))
            return -1;

    if (!edid_file.empty())
    {
        if (read_edid)
        {
            if (!g_mw.ReadEDID(edid_file))
                return -1;
        }
        else if (write_edid)
        {
            if (!g_mw.WriteEDID(edid_file))
                return -1;
        }
    }

    if (do_capture)
    {
        if (!g_mw.Capture(video_codec, preset, quality, look_ahead,
                          no_audio, device))
            return -2;
    }

    return ret;
}
