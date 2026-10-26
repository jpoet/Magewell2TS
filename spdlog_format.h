#pragma once

#include <array>
#include <cstring>
#include <memory>
#include <string_view>
#include <pthread.h>

#include "spdlog/spdlog.h"
#include "spdlog/pattern_formatter.h"

class linux_thread_name_flag : public spdlog::custom_flag_formatter
{
  private:
    static constexpr size_t WIDTH = 6;

    static void append_centered(std::string_view str,
                                spdlog::memory_buf_t& dest)
    {
        if (str.length() >= WIDTH)
        {
            dest.append(str.data(), str.data() + WIDTH);
            return;
        }

        size_t total_padding = WIDTH - str.length();
        size_t left_padding  = total_padding / 2;
        size_t right_padding = total_padding - left_padding;

        std::array<char, WIDTH> buf{};
        buf.fill(' ');
        std::memcpy(buf.data() + left_padding, str.data(), str.length());
        (void)right_padding; // already filled with spaces

        dest.append(buf.data(), buf.data() + WIDTH);
    }

  public:
    void format(const spdlog::details::log_msg&, const std::tm&,
                spdlog::memory_buf_t& dest) override
    {
        std::array<char, 16> name_buffer{};
        std::string_view raw_name = "unnamed";

        // Pull the native OS thread name
        if (pthread_getname_np(pthread_self(), name_buffer.data(),
                               name_buffer.size()) == 0 &&
            name_buffer.front() != '\0')
        {
            raw_name = name_buffer.data();
        }

        append_centered(raw_name, dest);
    }

    std::unique_ptr<custom_flag_formatter> clone() const override
    {
        return std::make_unique<linux_thread_name_flag>();
    }
};
