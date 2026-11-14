#include "linux/Log.h"

#include <cstdio>
#include <ctime>
#include <mutex>

namespace bt::linux_app {

namespace {

std::mutex g_mutex;
std::FILE* g_file = nullptr;

// Above this the log starts again, so a stuck error cannot fill the disk.
constexpr long kMaxLogBytes = 5L * 1024 * 1024;

}  // namespace

void log_open(const std::string& path) {
    std::lock_guard lock(g_mutex);
    if (g_file != nullptr) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_file = std::fopen(path.c_str(), "a");
    if (g_file != nullptr && std::fseek(g_file, 0, SEEK_END) == 0 && std::ftell(g_file) > kMaxLogBytes) {
        g_file = std::freopen(path.c_str(), "w", g_file);
    }
}

void log(LogLevel level, std::string_view message) {
    const char* tag = level == LogLevel::Error ? "ERROR" : level == LogLevel::Warning ? "WARN " : "INFO ";
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &local);

    std::lock_guard lock(g_mutex);
    std::fprintf(stderr, "%s %s %.*s\n", stamp, tag, static_cast<int>(message.size()), message.data());
    if (g_file != nullptr) {
        std::fprintf(g_file, "%s %s %.*s\n", stamp, tag, static_cast<int>(message.size()), message.data());
        std::fflush(g_file);
    }
}

}  // namespace bt::linux_app
