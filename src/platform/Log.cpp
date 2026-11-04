#include "platform/Log.h"

#include "platform/Com.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace bt::log {
namespace {

// Big enough for days of info-level logging or a full debug session, small enough to open in
// Notepad. Three files (backtrack.log, .1.log, .2.log) cap the disk use at 15 MB.
constexpr std::uint64_t kMaxFileBytes = 5ull * 1024 * 1024;
constexpr int kKeptFiles = 3;

// Lines logged before init() are held in memory; a runaway early loop must not eat RAM.
constexpr std::size_t kMaxEarlyLines = 1000;

// No single line may be large enough to push the whole history out of the rolled files.
constexpr std::size_t kMaxLineBytes = 64 * 1024;

// When rolling fails (another program holds the file without delete sharing), try again only
// after this much more has been written instead of on every line.
constexpr std::uint64_t kRollRetryBytes = 1024 * 1024;

struct State {
    std::mutex mutex;
    UniqueHandle file;
    std::filesystem::path path;
    std::uint64_t size = 0;
    std::uint64_t next_roll_at = 0;  // do not try to roll below this size (after a failed roll)
    bool initialised = false;
    bool echo_console = false;
    std::vector<std::string> early_lines;
    std::atomic<LogLevel> level{LogLevel::Info};
};

State& state() {
    static State s;
    return s;
}

std::filesystem::path numbered(const std::filesystem::path& path, int n) {
    std::filesystem::path out = path;
    out.replace_filename(path.stem().wstring() + L"." + std::to_wstring(n) + path.extension().wstring());
    return out;
}

void open_file(State& s) {
    s.file.reset(CreateFileW(s.path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER size{};
    s.size = s.file && GetFileSizeEx(s.file.get(), &size) ? static_cast<std::uint64_t>(size.QuadPart) : 0;
}

// Failures here are ignored on purpose: the log is the only place we could report them.
void roll(State& s) {
    s.file.reset();
    DeleteFileW(numbered(s.path, kKeptFiles - 1).c_str());
    for (int n = kKeptFiles - 2; n >= 1; --n) {
        MoveFileExW(numbered(s.path, n).c_str(), numbered(s.path, n + 1).c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    MoveFileExW(s.path.c_str(), numbered(s.path, 1).c_str(), MOVEFILE_REPLACE_EXISTING);
    open_file(s);
}

void append(State& s, const std::string& line) {
    if (!s.file) {
        return;
    }
    if (s.size > 0 && s.size + line.size() > kMaxFileBytes && s.size >= s.next_roll_at) {
        roll(s);
        if (!s.file) {
            return;
        }
        // A failed rename leaves the big file in place: retry later, not on every line. A
        // successful roll starts the new file with the normal cap again.
        s.next_roll_at = s.size > 0 ? s.size + kRollRetryBytes : 0;
    }
    DWORD written = 0;
    if (WriteFile(s.file.get(), line.data(), static_cast<DWORD>(line.size()), &written, nullptr)) {
        s.size += written;
    }
}

std::string_view level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "ERROR";
        case LogLevel::Warning: return "WARN ";
        case LogLevel::Info: return "INFO ";
        case LogLevel::Debug: return "DEBUG";
    }
    return "?    ";
}

}  // namespace

void init(const std::filesystem::path& file, LogLevel level) {
    State& s = state();
    s.level = level;
    std::lock_guard lock(s.mutex);
    s.path = file;
    open_file(s);
    if (s.size > kMaxFileBytes) {
        roll(s);
    }
    s.initialised = true;
    for (const std::string& line : s.early_lines) {
        append(s, line);
    }
    s.early_lines.clear();
    s.early_lines.shrink_to_fit();
}

void set_level(LogLevel level) {
    state().level = level;
}

void set_console_echo(bool echo) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    s.echo_console = echo;
}

void shutdown() {
    State& s = state();
    std::lock_guard lock(s.mutex);
    s.file.reset();
    s.initialised = false;
}

bool enabled(LogLevel level) {
    // "error" still shows warnings: they are what tells the player why a hotkey or a save did not work.
    const LogLevel threshold = state().level.load(std::memory_order_relaxed);
    return level <= (threshold == LogLevel::Error ? LogLevel::Warning : threshold);
}

void write(LogLevel level, std::string_view message) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    const std::string_view text = message.size() > kMaxLineBytes ? message.substr(0, kMaxLineBytes) : message;
    const std::string line =
        std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} {} [{}] {}\r\n", t.wYear, t.wMonth, t.wDay, t.wHour,
                    t.wMinute, t.wSecond, t.wMilliseconds, level_name(level), GetCurrentThreadId(), text);

    State& s = state();
    bool echo = false;
    {
        std::lock_guard lock(s.mutex);
        echo = s.echo_console;
        if (s.initialised) {
            append(s, line);
        } else if (s.early_lines.size() < kMaxEarlyLines) {
            s.early_lines.push_back(line);
        }
    }
    // Outside the lock: a console paused by the user (selecting text, Scroll Lock) blocks this
    // write, and that must not block every other thread that logs.
    if (echo) {
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fflush(stdout);
    }
}

}  // namespace bt::log
