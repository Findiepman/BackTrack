#pragma once

#include "core/Config.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <string_view>

namespace bt::log {

// Opens (or rolls) the log file. Lines logged before init() are kept and written then, so
// startup problems that happen before the config is read are not lost.
void init(const std::filesystem::path& file, LogLevel level);
void set_level(LogLevel level);
void set_console_echo(bool echo);
void shutdown();

bool enabled(LogLevel level);
void write(LogLevel level, std::string_view message);

template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(LogLevel::Error)) write(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(LogLevel::Warning)) write(LogLevel::Warning, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(LogLevel::Info)) write(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(LogLevel::Debug)) write(LogLevel::Debug, std::format(fmt, std::forward<Args>(args)...));
}

// Lets hot-path code log at most once per interval (§9). One instance per call site, used
// from a single thread.
class RateLimit {
public:
    explicit RateLimit(std::chrono::milliseconds interval) : interval_(interval) {}
    bool allow() {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_) {
            return false;
        }
        next_ = now + interval_;
        return true;
    }

private:
    std::chrono::milliseconds interval_;
    std::chrono::steady_clock::time_point next_{};
};

}  // namespace bt::log
