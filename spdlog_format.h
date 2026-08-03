#pragma once

#include "spdlog/spdlog.h"
#include "spdlog/pattern_formatter.h"
#include <pthread.h>
#include <thread>
#include <array>

#include "spdlog/spdlog.h"
#include "spdlog/pattern_formatter.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/stdout_sinks.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include <pthread.h>
#include <array>
#include <iostream>
#include <filesystem>
#include <vector>

#include "spdlog/spdlog.h"
#include "spdlog/pattern_formatter.h"
#include <pthread.h>
#include <array>
#include <string>
#include <cstring>

class linux_thread_name_flag : public spdlog::custom_flag_formatter
{
  private:
    std::string format_centered(const std::string& str, size_t width) const
    {
        if (str.length() >= width)
        {
            // Truncate if the thread name exceeds the maximum allowed
            // characters
            return str.substr(0, width);
        }

        size_t total_padding = width - str.length();
        size_t left_padding = total_padding / 2;
        size_t right_padding = total_padding - left_padding;

        return std::string(left_padding, ' ') + str +
            std::string(right_padding, ' ');
    }

  public:
    void format(const spdlog::details::log_msg&, const std::tm&,
                spdlog::memory_buf_t& dest) override
    {
        std::array<char, 16> name_buffer{};
        std::string raw_name = "unnamed";

        // Pull the native OS thread name
        if (pthread_getname_np(pthread_self(), name_buffer.data(),
                               name_buffer.size()) == 0 && name_buffer[0] != '\0')
        {
            raw_name = name_buffer.data();
        }

        // Center-align or truncate the string to 6 characters
        std::string aligned_name = format_centered(raw_name, 6);
        dest.append(aligned_name.data(), aligned_name.data() +
                    aligned_name.size());
    }

    std::unique_ptr<custom_flag_formatter> clone() const override
    {
        return std::make_unique<linux_thread_name_flag>();
    }
};
