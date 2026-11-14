#pragma once

// The Linux log: one line per event to stderr and to $XDG_STATE_HOME/Backtrack/backtrack.log
// (~/.local/state/Backtrack). Thread safe.

#include <string>
#include <string_view>

namespace bt::linux_app {

enum class LogLevel { Error, Warning, Info };

void log_open(const std::string& path);
void log(LogLevel level, std::string_view message);

inline void log_error(std::string_view m) { log(LogLevel::Error, m); }
inline void log_warning(std::string_view m) { log(LogLevel::Warning, m); }
inline void log_info(std::string_view m) { log(LogLevel::Info, m); }

}  // namespace bt::linux_app
