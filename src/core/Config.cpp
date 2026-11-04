#include "core/Config.h"

#include "core/Hotkey.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <format>
#include <map>
#include <optional>
#include <span>

namespace bt {
namespace {

// §5.2: shorter than 5 s is barely more than one GOP, longer than 300 s at 20 Mbit/s keeps
// 750 MB in memory.
constexpr long long kMinClipSeconds = 5;
constexpr long long kMaxClipSeconds = 300;

constexpr std::array<int, 4> kAllowedFps = {30, 60, 120, 144};

// The Microsoft AAC encoder accepts exactly these bitrates (12000, 16000, 20000 and 24000 bytes
// per second) and rejects the media type for anything else.
constexpr std::array<int, 4> kAllowedAudioKbps = {96, 128, 160, 192};

// Below 1 Mbit/s a desktop frame is unreadable. The upper bound is a sanity cap so a typo
// (an extra zero) cannot ask the encoder for gigabits per second.
constexpr long long kMinVideoKbps = 1000;
constexpr long long kMaxVideoKbps = 100000;

// Only the lower bound is checked here; ScreenCapture knows how many monitors exist and falls back
// to monitor 0 with one warning (§5.2).
constexpr long long kMaxMonitorIndex = 1000;

constexpr const char* kForbiddenFileNameChars = "<>:\"|?*";
constexpr const char* kSafeStrftimeSpecifiers = "aAbBdHIjmMpSUwWyY%";

constexpr std::string_view kWhitespace = " \t\r\n\f\v";
constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(kWhitespace);
    return s.substr(first, last - first + 1);
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool is_blank(char c) {
    return c == ' ' || c == '\t';
}

// An inline comment only starts at a ';' or '#' that follows whitespace (or starts the value),
// so "seconds = 30 ; half a minute" reads 30 while a file name such as "clip#1.mp4" survives.
std::string_view strip_inline_comment(std::string_view value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        const bool comment_char = value[i] == ';' || value[i] == '#';
        if (comment_char && (i == 0 || is_blank(value[i - 1]))) {
            return value.substr(0, i);
        }
    }
    return value;
}

std::optional<long long> parse_integer(std::string_view text) {
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    long long value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<bool> parse_bool(std::string_view text) {
    const std::string v = to_lower(text);
    if (v == "true" || v == "yes" || v == "on" || v == "1") {
        return true;
    }
    if (v == "false" || v == "no" || v == "off" || v == "0") {
        return false;
    }
    return std::nullopt;
}

// Picks the closest allowed value; on a tie the earlier (lower) one wins.
int nearest_allowed(long long value, std::span<const int> allowed) {
    // Clamped first so value - candidate cannot overflow for inputs near LLONG_MIN/MAX.
    constexpr long long kSnapRange = 1'000'000'000;
    value = std::clamp(value, -kSnapRange, kSnapRange);
    int best = allowed.front();
    for (const int candidate : allowed) {
        if (std::llabs(value - candidate) < std::llabs(value - best)) {
            best = candidate;
        }
    }
    return best;
}

std::string join(std::span<const int> values) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        out += std::to_string(values[i]);
        if (i + 2 < values.size()) {
            out += ", ";
        } else if (i + 2 == values.size()) {
            out += " or ";
        }
    }
    return out;
}

bool ends_with_mp4(std::string_view name) {
    return name.size() >= 4 && to_lower(name.substr(name.size() - 4)) == ".mp4";
}

class Applier {
public:
    Applier(Settings& settings, std::vector<std::string>& warnings, const IniEntry& entry)
        : s_(settings), warnings_(warnings), e_(entry) {}

    void apply() {
        const std::string id = e_.section + "." + e_.key;
        if (id == "clip.seconds") {
            clamped_int(s_.clip_seconds, kMinClipSeconds, kMaxClipSeconds);
        } else if (id == "clip.output_dir") {
            non_empty_string(s_.output_dir);
        } else if (id == "clip.filename") {
            file_name();
        } else if (id == "hotkey.save") {
            hotkey();
        } else if (id == "video.monitor") {
            clamped_int(s_.monitor, 0, kMaxMonitorIndex);
        } else if (id == "video.fps") {
            snapped_int(s_.fps, kAllowedFps);
        } else if (id == "video.bitrate_kbps") {
            clamped_int(s_.video_bitrate_kbps, kMinVideoKbps, kMaxVideoKbps);
        } else if (id == "video.encoder") {
            encoder();
        } else if (id == "audio.enabled") {
            boolean(s_.audio_enabled);
        } else if (id == "audio.bitrate_kbps") {
            snapped_int(s_.audio_bitrate_kbps, kAllowedAudioKbps);
        } else if (id == "feedback.sound") {
            boolean(s_.sound);
        } else if (id == "feedback.notification") {
            boolean(s_.notification);
        } else if (id == "logging.level") {
            log_level();
        } else {
            warn(std::format("unknown setting '{}' in [{}], ignored", e_.key, e_.section));
        }
    }

private:
    void warn(const std::string& message) {
        warnings_.push_back(std::format("line {}: {}", e_.line, message));
    }

    std::optional<long long> integer(int current) {
        const auto value = parse_integer(e_.value);
        if (!value) {
            warn(std::format("{} = '{}' is not a whole number, keeping {}", e_.key, e_.value, current));
        }
        return value;
    }

    void clamped_int(int& target, long long min, long long max) {
        const auto value = integer(target);
        if (!value) {
            return;
        }
        if (*value < min) {
            warn(std::format("{} = {} is below the minimum {}, using {}", e_.key, *value, min, min));
            target = static_cast<int>(min);
        } else if (*value > max) {
            warn(std::format("{} = {} is above the maximum {}, using {}", e_.key, *value, max, max));
            target = static_cast<int>(max);
        } else {
            target = static_cast<int>(*value);
        }
    }

    void snapped_int(int& target, std::span<const int> allowed) {
        const auto value = integer(target);
        if (!value) {
            return;
        }
        const int snapped = nearest_allowed(*value, allowed);
        if (snapped != *value) {
            warn(std::format("{} = {} is not supported ({}), using {}", e_.key, *value, join(allowed), snapped));
        }
        target = snapped;
    }

    void boolean(bool& target) {
        if (const auto value = parse_bool(e_.value)) {
            target = *value;
        } else {
            warn(std::format("{} = '{}' should be true or false, keeping {}", e_.key, e_.value, target));
        }
    }

    // A path such as "D:\Clips #2" loses its tail to the inline comment rule; say so instead of
    // silently saving somewhere else.
    void warn_if_comment_cut() {
        if (e_.comment_removed) {
            warn(std::format("{}: the text after ';' or '#' was read as a comment. If it was meant as part of the "
                             "name, put the whole value in double quotes",
                             e_.key));
        }
    }

    void non_empty_string(std::string& target) {
        warn_if_comment_cut();
        if (e_.value.empty()) {
            warn(std::format("{} is empty, keeping {}", e_.key, target));
        } else {
            target = e_.value;
        }
    }

    void file_name() {
        warn_if_comment_cut();
        if (e_.value.empty()) {
            warn(std::format("filename is empty, keeping {}", s_.filename));
            return;
        }
        if (e_.value.find_first_of("\\/") != std::string::npos) {
            warn(std::format("filename = '{}' contains a folder separator, put folders in output_dir. Keeping {}",
                             e_.value, s_.filename));
            return;
        }
        // ':' in particular does not fail on NTFS: it silently writes an alternate data stream
        // and the clip shows up as an empty file.
        for (const char c : e_.value) {
            if (std::string_view(kForbiddenFileNameChars).find(c) != std::string_view::npos ||
                static_cast<unsigned char>(c) < 0x20) {
                warn(std::format("filename = '{}' contains a character Windows does not allow in file names "
                                 "(< > : \" | ? *). Keeping {}",
                                 e_.value, s_.filename));
                return;
            }
        }
        // Unknown strftime placeholders make the MSVC runtime abort the program, and %c, %T, %D
        // and friends produce ':' or '/'. Only placeholders that are safe in a file name pass.
        for (std::size_t i = 0; i < e_.value.size(); ++i) {
            if (e_.value[i] != '%') {
                continue;
            }
            const char spec = i + 1 < e_.value.size() ? e_.value[i + 1] : '\0';
            if (std::string_view(kSafeStrftimeSpecifiers).find(spec) == std::string_view::npos || spec == '\0') {
                warn(std::format("filename = '{}' uses '%{}', which is not allowed in a file name. Use %Y %m %d %H "
                                 "%M %S %y %j %a %A %b %B %p %I %U %W %w or %%. Keeping {}",
                                 e_.value, spec == '\0' ? std::string() : std::string(1, spec), s_.filename));
                return;
            }
            ++i;  // skip the specifier so "%%" is not read twice
        }
        s_.filename = e_.value;
        if (!ends_with_mp4(s_.filename)) {
            s_.filename += ".mp4";
            warn(std::format("filename = '{}' does not end in .mp4, using {}", e_.value, s_.filename));
        }
    }

    void hotkey() {
        const HotkeyParseResult parsed = parse_hotkey(e_.value);
        if (!parsed.hotkey) {
            warn(std::format("save = '{}': {}. Using {}", e_.value, parsed.error, s_.save_hotkey));
            return;
        }
        s_.save_hotkey = hotkey_to_string(*parsed.hotkey);
        if (!parsed.warning.empty()) {
            warn(std::format("save = {}: {}", s_.save_hotkey, parsed.warning));
        }
    }

    void encoder() {
        const std::string v = to_lower(e_.value);
        if (v == "auto") {
            s_.encoder = EncoderPreference::Auto;
        } else if (v == "hardware") {
            s_.encoder = EncoderPreference::Hardware;
        } else if (v == "software") {
            s_.encoder = EncoderPreference::Software;
        } else {
            warn(std::format("encoder = '{}' should be auto, hardware or software, keeping the previous value",
                             e_.value));
        }
    }

    void log_level() {
        const std::string v = to_lower(e_.value);
        if (v == "error") {
            s_.log_level = LogLevel::Error;
        } else if (v == "info") {
            s_.log_level = LogLevel::Info;
        } else if (v == "debug") {
            s_.log_level = LogLevel::Debug;
        } else {
            warn(std::format("level = '{}' should be error, info or debug, keeping the previous value", e_.value));
        }
    }

    Settings& s_;
    std::vector<std::string>& warnings_;
    const IniEntry& e_;
};

}  // namespace

IniDocument parse_ini(std::string_view text) {
    IniDocument doc;
    if (text.starts_with(kUtf8Bom)) {
        text.remove_prefix(kUtf8Bom.size());
    }

    std::string section;
    int line_number = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        ++line_number;
        auto warn = [&](const std::string& message) {
            doc.warnings.push_back(std::format("line {}: {}", line_number, message));
        };

        if (line.empty() || line.front() == ';' || line.front() == '#') {
            continue;
        }
        if (line.front() == '[') {
            // "[video] ; my settings" is a header followed by a comment. A section name cannot
            // contain ']', so everything after the first one must be empty or a comment.
            const std::size_t close = line.find(']');
            const std::string_view rest = close == std::string_view::npos ? std::string_view{} : trim(line.substr(close + 1));
            const bool closed = close != std::string_view::npos && (rest.empty() || rest.front() == ';' || rest.front() == '#');
            const std::string_view name = closed ? trim(line.substr(1, close - 1)) : std::string_view{};
            if (!closed || name.empty()) {
                warn(std::format("'{}' is not a valid [section] header, the keys below it are ignored", line));
                section.clear();
            } else {
                section = to_lower(name);
            }
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            warn(std::format("expected 'key = value', ignoring '{}'", line));
            continue;
        }
        const std::string key = to_lower(trim(line.substr(0, equals)));
        if (key.empty()) {
            warn(std::format("'{}' has no key before '=', ignored", line));
            continue;
        }
        if (section.empty()) {
            warn(std::format("'{}' is not inside a valid [section], ignored", key));
            continue;
        }

        std::string_view value = trim(line.substr(equals + 1));
        bool comment_removed = false;
        auto without_comment = [&](std::string_view text) {
            const std::string_view kept = trim(strip_inline_comment(text));
            // A value that is nothing but a comment is reported as empty, not as a cut-off name.
            comment_removed = !kept.empty() && kept.size() < trim(text).size();
            return kept;
        };
        if (!value.empty() && value.front() == '"') {
            const std::size_t close = value.find('"', 1);
            if (close == std::string_view::npos) {
                warn(std::format("{} has an opening quote but no closing quote", key));
                value = without_comment(value.substr(1));
            } else {
                value = value.substr(1, close - 1);
            }
        } else {
            value = without_comment(value);
        }
        doc.entries.push_back(IniEntry{section, key, std::string(value), line_number, comment_removed});
    }
    return doc;
}

LoadedSettings load_settings(std::string_view ini_text) {
    LoadedSettings out;
    IniDocument doc = parse_ini(ini_text);
    out.warnings = std::move(doc.warnings);

    std::map<std::string, int> first_line_of;
    for (const IniEntry& entry : doc.entries) {
        const std::string id = entry.section + "." + entry.key;
        const auto [it, inserted] = first_line_of.emplace(id, entry.line);
        if (!inserted) {
            out.warnings.push_back(std::format("line {}: [{}] {} is set again (first on line {}), using this later value",
                                               entry.line, entry.section, entry.key, it->second));
        }
        Applier(out.settings, out.warnings, entry).apply();
    }
    return out;
}

std::string_view default_ini_text() {
    return R"(; Backtrack settings. Restart the program after editing.

[clip]
; How many seconds to keep in memory and save when the hotkey is pressed.
seconds = 30
; Where clips are saved. Environment variables are expanded.
output_dir = %USERPROFILE%\Videos\Backtrack
; File name pattern. Supports strftime placeholders.
filename = clip_%Y-%m-%d_%H-%M-%S.mp4

[hotkey]
; Modifiers: Ctrl, Alt, Shift, Win. Keys: A-Z, 0-9, F1-F24, Numpad0-9, Home, End,
; Insert, Delete, PageUp, PageDown, Space, Pause, PrintScreen.
; Letters, digits and Space need at least one modifier, for example Ctrl+Alt+C.
save = F8

[video]
; Which monitor to record, 0 is the primary monitor.
monitor = 0
fps = 60
bitrate_kbps = 20000
; auto, hardware or software
encoder = auto

[audio]
; Record what you hear (the default playback device).
enabled = true
bitrate_kbps = 160

[feedback]
; Play a sound when a clip is saved.
sound = true
; Show a tray notification with the file name.
notification = true

[logging]
; error, info or debug
level = info
)";
}

}  // namespace bt
