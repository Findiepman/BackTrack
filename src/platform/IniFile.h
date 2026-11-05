#pragma once

// Reading and writing backtrack.ini as Notepad users leave it: UTF-8, "Unicode" (UTF-16 LE/BE)
// or ANSI on the way in, always UTF-8 on the way out.

#include <filesystem>
#include <optional>
#include <string>

namespace bt {

// The file as UTF-8, or empty when it could not be read (missing, locked by a sync tool).
std::optional<std::string> read_ini_file(const std::filesystem::path& path);

// Written to "<path>.tmp", flushed, then renamed over the old file, so a power cut leaves either
// the old or the new settings. False (and a log line) when it could not be written.
bool write_ini_file(const std::filesystem::path& path, const std::string& utf8_text);

}  // namespace bt
