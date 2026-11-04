#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace bt {

enum class EncoderPreference { Auto, Hardware, Software };

// Ordered by severity so "level <= threshold" means "should be logged".
enum class LogLevel { Error, Warning, Info, Debug };

// Typed settings with the defaults used when a key is missing (§5.1).
struct Settings {
    // [clip]
    int clip_seconds = 30;
    std::string output_dir = "%USERPROFILE%\\Videos\\Backtrack";
    std::string filename = "clip_%Y-%m-%d_%H-%M-%S.mp4";
    // [hotkey]
    std::string save_hotkey = "F8";  // always a string parse_hotkey accepts
    // [video]
    int monitor = 0;
    int fps = 60;
    int video_bitrate_kbps = 20000;
    EncoderPreference encoder = EncoderPreference::Auto;
    // [audio]
    bool audio_enabled = true;
    int audio_bitrate_kbps = 160;
    // [feedback]
    bool sound = true;
    bool notification = true;
    // [logging]
    LogLevel log_level = LogLevel::Info;
};

struct IniEntry {
    std::string section;  // lower case
    std::string key;  // lower case
    std::string value;  // trimmed, quotes and inline comment removed
    int line = 0;
    bool comment_removed = false;  // an unquoted " ;..." or " #..." tail was cut off as a comment
};

struct IniDocument {
    std::vector<IniEntry> entries;
    std::vector<std::string> warnings;  // "line N: ..." for lines that are not valid INI
};

IniDocument parse_ini(std::string_view text);

struct LoadedSettings {
    Settings settings;
    std::vector<std::string> warnings;  // one per problem, each starts with "line N: "
};

// Never fails: every bad or unknown value produces a warning and keeps the default (§5.1).
LoadedSettings load_settings(std::string_view ini_text);

// The commented file written on first run. Identical to backtrack.ini in the repository.
std::string_view default_ini_text();

}  // namespace bt
