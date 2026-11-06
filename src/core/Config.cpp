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

// The words of each choice in the ini, in the enum's order.
constexpr std::array<std::string_view, 3> kQualityNames = {"performance", "balanced", "quality"};
constexpr std::array<std::string_view, 4> kCornerNames = {"top_left", "top_right", "bottom_left", "bottom_right"};
constexpr std::array<std::string_view, 5> kThemeNames = {"night", "harbor", "plum", "moss", "daylight"};
constexpr std::array<std::string_view, 2> kDensityNames = {"comfortable", "compact"};
constexpr std::array<std::string_view, 3> kThumbnailNames = {"small", "medium", "large"};

// §5.2: shorter than 5 s is barely more than one GOP, longer than 300 s at 20 Mbit/s keeps
// 750 MB in memory.
constexpr long long kMinClipSeconds = 5;
constexpr long long kMaxClipSeconds = 300;

constexpr std::array<int, 4> kAllowedFps = {30, 60, 120, 144};

// Output heights besides "native" (0).
constexpr std::array<int, 3> kAllowedResolutions = {720, 1080, 1440};

constexpr long long kMinOverlayOpacity = 20;
constexpr long long kMaxOverlayOpacity = 100;

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

constexpr long long kMaxStorageGb = 100000;
constexpr long long kMaxDeleteAfterDays = 3650;
constexpr long long kMaxVolumePercent = 200;

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

// "[video] ; my settings" is a header followed by a comment. A section name cannot contain ']',
// so everything after the first one must be empty or a comment. Takes a trimmed line. Returns
// nullopt for a line that is not a header, an empty string for a broken header and the lower case
// name otherwise.
std::optional<std::string> section_header(std::string_view line) {
    if (line.empty() || line.front() != '[') {
        return std::nullopt;
    }
    const std::size_t close = line.find(']');
    const std::string_view rest = close == std::string_view::npos ? std::string_view{} : trim(line.substr(close + 1));
    const bool closed = close != std::string_view::npos && (rest.empty() || rest.front() == ';' || rest.front() == '#');
    return closed ? to_lower(trim(line.substr(1, close - 1))) : std::string();
}

class Applier {
public:
    Applier(Settings& settings, std::vector<std::string>& warnings, const IniEntry& entry)
        : s_(settings), warnings_(warnings), e_(entry) {}

    void apply() {
        const std::string id = e_.section + "." + e_.key;
        if (id == "general.start_minimized") {
            boolean(s_.start_minimized);
        } else if (id == "general.auto_update") {
            boolean(s_.auto_update);
        } else if (id == "storage.max_gb") {
            clamped_int(s_.max_gb, 0, kMaxStorageGb);
        } else if (id == "storage.delete_after_days") {
            clamped_int(s_.delete_after_days, 0, kMaxDeleteAfterDays);
        } else if (id == "clip.seconds") {
            clamped_int(s_.clip_seconds, kMinClipSeconds, kMaxClipSeconds);
        } else if (id == "clip.save_seconds") {
            clamped_int(s_.save_seconds, 0, kMaxClipSeconds);
        } else if (id == "clip.output_dir") {
            non_empty_string(s_.output_dir);
        } else if (id == "clip.filename") {
            file_name();
        } else if (id == "hotkey.save") {
            hotkey();
        } else if (id == "hotkey.mic_mute") {
            other_hotkey(s_.mic_mute_hotkey, HotkeyUse::Global, true);
        } else if (id == "hotkey.push_to_talk") {
            other_hotkey(s_.push_to_talk_key, HotkeyUse::Held, false);
        } else if (id == "hotkey.overlay") {
            other_hotkey(s_.overlay_hotkey, HotkeyUse::Global, true);
        } else if (id == "hotkey.bookmark") {
            other_hotkey(s_.bookmark_hotkey, HotkeyUse::Global, true);
        } else if (id == "video.monitor") {
            clamped_int(s_.monitor, 0, kMaxMonitorIndex);
        } else if (id == "video.fps") {
            snapped_int(s_.fps, kAllowedFps);
        } else if (id == "video.bitrate_kbps") {
            clamped_int(s_.video_bitrate_kbps, kMinVideoKbps, kMaxVideoKbps);
        } else if (id == "video.encoder") {
            encoder();
        } else if (id == "video.resolution") {
            resolution();
        } else if (id == "video.quality") {
            choice(s_.quality, kQualityNames);
        } else if (id == "video.cursor") {
            boolean(s_.capture_cursor);
        } else if (id == "audio.enabled") {
            boolean(s_.audio_enabled);
        } else if (id == "audio.bitrate_kbps") {
            snapped_int(s_.audio_bitrate_kbps, kAllowedAudioKbps);
        } else if (id == "audio.system_volume") {
            clamped_int(s_.system_volume, 0, kMaxVolumePercent);
        } else if (id == "audio.system_device") {
            warn_if_comment_cut();
            s_.system_device = e_.value;  // empty is allowed: the Windows default
        } else if (id == "audio.separate_tracks") {
            boolean(s_.separate_tracks);
        } else if (id == "audio.mic") {
            boolean(s_.mic_enabled);
        } else if (id == "audio.mic_device") {
            warn_if_comment_cut();
            s_.mic_device = e_.value;  // empty is allowed: the Windows default
        } else if (id == "audio.mic_volume") {
            clamped_int(s_.mic_volume, 0, kMaxVolumePercent);
        } else if (id == "audio.noise_suppression") {
            boolean(s_.noise_suppression);
        } else if (id == "audio.push_to_talk") {
            boolean(s_.push_to_talk);
        } else if (id == "feedback.sound") {
            boolean(s_.sound);
        } else if (id == "feedback.notification") {
            boolean(s_.notification);
        } else if (id == "feedback.saved_bar") {
            boolean(s_.saved_bar);
        } else if (id == "overlay.show") {
            boolean(s_.overlay);
        } else if (id == "overlay.position") {
            choice(s_.overlay_corner, kCornerNames);
        } else if (id == "overlay.opacity") {
            clamped_int(s_.overlay_opacity, kMinOverlayOpacity, kMaxOverlayOpacity);
        } else if (id == "appearance.theme") {
            choice(s_.theme, kThemeNames);
        } else if (id == "appearance.density") {
            choice(s_.density, kDensityNames);
        } else if (id == "appearance.thumbnail_size") {
            choice(s_.thumbnail_size, kThumbnailNames);
        } else if (id == "appearance.reduce_motion") {
            boolean(s_.reduce_motion);
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

    // A second hotkey: empty may mean "none" (allow_empty), anything else must parse.
    void other_hotkey(std::string& target, HotkeyUse use, bool allow_empty) {
        if (allow_empty && trim(e_.value).empty()) {
            target.clear();
            return;
        }
        const HotkeyParseResult parsed = parse_hotkey(e_.value, use);
        if (!parsed.hotkey) {
            warn(std::format("{} = '{}': {}. Using {}", e_.key, e_.value, parsed.error,
                             target.empty() ? std::string("none") : target));
            return;
        }
        target = hotkey_to_string(*parsed.hotkey);
    }

    // One of a fixed list of words, by the enum's order.
    template <typename Enum, std::size_t N>
    void choice(Enum& target, const std::array<std::string_view, N>& names) {
        const std::string v = to_lower(e_.value);
        for (std::size_t i = 0; i < N; ++i) {
            if (v == names[i]) {
                target = static_cast<Enum>(i);
                return;
            }
        }
        std::string list;
        for (std::size_t i = 0; i < N; ++i) {
            list += std::string(names[i]) + (i + 2 < N ? ", " : i + 2 == N ? " or " : "");
        }
        warn(std::format("{} = '{}' should be {}, keeping {}", e_.key, e_.value, list,
                         names[static_cast<std::size_t>(target)]));
    }

    // "native" (or 0) or a height; other heights snap to the nearest offered one.
    void resolution() {
        if (to_lower(e_.value) == "native" || trim(e_.value) == "0") {
            s_.resolution = 0;
            return;
        }
        snapped_int(s_.resolution, kAllowedResolutions);
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
        } else if (v == "warning") {
            s_.log_level = LogLevel::Warning;
        } else if (v == "info") {
            s_.log_level = LogLevel::Info;
        } else if (v == "debug") {
            s_.log_level = LogLevel::Debug;
        } else {
            warn(std::format("level = '{}' should be error, warning, info or debug, keeping the previous value", e_.value));
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
        if (const auto header = section_header(line)) {
            if (header->empty()) {
                warn(std::format("'{}' is not a valid [section] header, the keys below it are ignored", line));
            }
            section = *header;
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

[general]
; When Backtrack starts with Windows, open in the tray.
start_minimized = false
; Download new versions in the background and install them when Backtrack closes.
auto_update = true

[clip]
; How many seconds to keep in memory (the replay buffer).
seconds = 30
; How many of those the save hotkey keeps. 0 is all of them.
save_seconds = 0
; Where clips are saved. Environment variables are expanded.
output_dir = %USERPROFILE%\Videos\Backtrack
; File name pattern. Supports strftime placeholders.
filename = clip_%Y-%m-%d_%H-%M-%S.mp4

[storage]
; Delete the oldest clips when they take more than this many GB, 0 is no limit.
max_gb = 0
; Delete clips older than this many days, 0 is never.
delete_after_days = 0

[hotkey]
; Modifiers: Ctrl, Alt, Shift, Win. Keys: A-Z, 0-9, F1-F24, Numpad0-9, Home, End,
; Insert, Delete, PageUp, PageDown, Space, Pause, PrintScreen.
; Letters, digits and Space need at least one modifier, for example Ctrl+Alt+C.
save = F8
; Mutes and unmutes the microphone in recordings. Empty: no key.
mic_mute = Alt+F7
; Held down to record the microphone when push_to_talk is on. Plain letters work here too,
; and CapsLock, Tab, Backquote, Up/Down/Left/Right, Mouse3/Mouse4/Mouse5: other programs still get it.
push_to_talk = V
; Shows or hides the overlay. Empty: no key.
overlay = Alt+O
; Marks a moment; it shows on the timeline of the clip it ends up in. Empty: no key.
bookmark = Alt+B

[video]
; Which monitor to record, 0 is the primary monitor.
monitor = 0
fps = 60
bitrate_kbps = 20000
; auto, hardware or software
encoder = auto
; native, 1440, 1080 or 720 (the height of the video; never larger than the display)
resolution = native
; performance, balanced or quality: how hard the encoder works for the same bitrate
quality = balanced
; Draw the mouse pointer into the video.
cursor = true

[audio]
; Record what you hear (the default playback device).
enabled = true
bitrate_kbps = 160
; Volume of what you hear in the clip, in percent (0 to 200).
system_volume = 100
; Which playback device: empty is the Windows default (followed when it changes).
system_device =
; Besides the mixed track, store the system sound and the microphone on tracks of their own.
separate_tracks = false
; Record the microphone, mixed into the same sound track.
mic = true
; Which microphone: empty is the Windows default (followed when it changes).
mic_device =
; Microphone volume in the clip, in percent (0 to 200).
mic_volume = 100
; Take out steady background noise (fans, hum) and quiet keyboard clicks from the microphone.
noise_suppression = true
; Only record the microphone while the push_to_talk key is held.
push_to_talk = false

[feedback]
; Play a sound when a clip is saved.
sound = true
; Show a tray notification with the file name.
notification = true
; Show the "Saved" bar in the window.
saved_bar = true

[overlay]
; A small status chip on top of the screen. It is never part of recordings.
show = true
; top_left, top_right, bottom_left or bottom_right
position = top_right
; In percent, 20 to 100.
opacity = 85

[appearance]
; night, harbor, plum, moss or daylight
theme = night
; comfortable or compact
density = comfortable
; small, medium or large
thumbnail_size = medium
; Turn off animations.
reduce_motion = false

[logging]
; error, warning, info or debug
level = info
)";
}

namespace {

const char* encoder_name(EncoderPreference e) {
    switch (e) {
        case EncoderPreference::Hardware: return "hardware";
        case EncoderPreference::Software: return "software";
        case EncoderPreference::Auto: break;
    }
    return "auto";
}

const char* log_level_name(LogLevel l) {
    switch (l) {
        case LogLevel::Error: return "error";
        case LogLevel::Warning: return "warning";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: break;
    }
    return "info";
}

// Writes a string so parse_ini reads it back unchanged wherever the format allows (see update_ini).
std::string encode_string(std::string_view raw) {
    std::string v;
    for (const char c : raw) {
        if (c != '\r' && c != '\n') {
            v += c;
        }
    }
    const bool needs_quotes = v.find_first_of(";#") != std::string::npos || v.starts_with('"') ||
                              (!v.empty() && (kWhitespace.find(v.front()) != std::string_view::npos ||
                                              kWhitespace.find(v.back()) != std::string_view::npos));
    if (!needs_quotes) {
        return v;
    }
    // The parser ends a quoted value at the next '"' and has no escape, so cut there.
    return "\"" + v.substr(0, v.find('"')) + "\"";
}

template <typename Enum, std::size_t N>
std::string name_of(Enum value, const std::array<std::string_view, N>& names) {
    const auto i = static_cast<std::size_t>(value);
    return std::string(i < N ? names[i] : names[0]);
}

std::string bool_text(bool b) {
    return b ? "true" : "false";
}

struct KnownKey {
    std::string_view section;
    std::string_view key;
    std::string (*value)(const Settings&);
};

// In the order of the default file, so appended keys and sections follow it.
constexpr std::array<KnownKey, 41> kKnownKeys = {{
    {"general", "start_minimized", [](const Settings& s) { return bool_text(s.start_minimized); }},
    {"general", "auto_update", [](const Settings& s) { return bool_text(s.auto_update); }},
    {"clip", "seconds", [](const Settings& s) { return std::to_string(s.clip_seconds); }},
    {"clip", "save_seconds", [](const Settings& s) { return std::to_string(s.save_seconds); }},
    {"clip", "output_dir", [](const Settings& s) { return encode_string(s.output_dir); }},
    {"clip", "filename", [](const Settings& s) { return encode_string(s.filename); }},
    {"storage", "max_gb", [](const Settings& s) { return std::to_string(s.max_gb); }},
    {"storage", "delete_after_days", [](const Settings& s) { return std::to_string(s.delete_after_days); }},
    {"hotkey", "save", [](const Settings& s) { return encode_string(s.save_hotkey); }},
    {"hotkey", "mic_mute", [](const Settings& s) { return encode_string(s.mic_mute_hotkey); }},
    {"hotkey", "push_to_talk", [](const Settings& s) { return encode_string(s.push_to_talk_key); }},
    {"hotkey", "overlay", [](const Settings& s) { return encode_string(s.overlay_hotkey); }},
    {"hotkey", "bookmark", [](const Settings& s) { return encode_string(s.bookmark_hotkey); }},
    {"video", "monitor", [](const Settings& s) { return std::to_string(s.monitor); }},
    {"video", "fps", [](const Settings& s) { return std::to_string(s.fps); }},
    {"video", "bitrate_kbps", [](const Settings& s) { return std::to_string(s.video_bitrate_kbps); }},
    {"video", "encoder", [](const Settings& s) { return std::string(encoder_name(s.encoder)); }},
    {"video", "resolution",
     [](const Settings& s) { return s.resolution == 0 ? std::string("native") : std::to_string(s.resolution); }},
    {"video", "quality", [](const Settings& s) { return name_of(s.quality, kQualityNames); }},
    {"video", "cursor", [](const Settings& s) { return bool_text(s.capture_cursor); }},
    {"audio", "enabled", [](const Settings& s) { return bool_text(s.audio_enabled); }},
    {"audio", "bitrate_kbps", [](const Settings& s) { return std::to_string(s.audio_bitrate_kbps); }},
    {"audio", "system_volume", [](const Settings& s) { return std::to_string(s.system_volume); }},
    {"audio", "system_device", [](const Settings& s) { return encode_string(s.system_device); }},
    {"audio", "separate_tracks", [](const Settings& s) { return bool_text(s.separate_tracks); }},
    {"audio", "mic", [](const Settings& s) { return bool_text(s.mic_enabled); }},
    {"audio", "mic_device", [](const Settings& s) { return encode_string(s.mic_device); }},
    {"audio", "mic_volume", [](const Settings& s) { return std::to_string(s.mic_volume); }},
    {"audio", "noise_suppression", [](const Settings& s) { return bool_text(s.noise_suppression); }},
    {"audio", "push_to_talk", [](const Settings& s) { return bool_text(s.push_to_talk); }},
    {"feedback", "sound", [](const Settings& s) { return bool_text(s.sound); }},
    {"feedback", "notification", [](const Settings& s) { return bool_text(s.notification); }},
    {"feedback", "saved_bar", [](const Settings& s) { return bool_text(s.saved_bar); }},
    {"overlay", "show", [](const Settings& s) { return bool_text(s.overlay); }},
    {"overlay", "position", [](const Settings& s) { return name_of(s.overlay_corner, kCornerNames); }},
    {"overlay", "opacity", [](const Settings& s) { return std::to_string(s.overlay_opacity); }},
    {"appearance", "theme", [](const Settings& s) { return name_of(s.theme, kThemeNames); }},
    {"appearance", "density", [](const Settings& s) { return name_of(s.density, kDensityNames); }},
    {"appearance", "thumbnail_size", [](const Settings& s) { return name_of(s.thumbnail_size, kThumbnailNames); }},
    {"appearance", "reduce_motion", [](const Settings& s) { return bool_text(s.reduce_motion); }},
    {"logging", "level", [](const Settings& s) { return std::string(log_level_name(s.log_level)); }},
}};

bool is_space(char c) {
    return kWhitespace.find(c) != std::string_view::npos;
}

// Replaces the value part of one "key = value ; comment" line, keeping everything around it.
std::string replace_value(std::string_view line, std::string_view new_value) {
    std::size_t start = line.find('=') + 1;
    while (start < line.size() && is_blank(line[start])) {
        ++start;
    }
    std::size_t content_end = line.size();
    while (content_end > start && (line[content_end - 1] == '\r' || line[content_end - 1] == '\n')) {
        --content_end;
    }
    const std::string_view value = line.substr(start, content_end - start);
    std::size_t value_len = 0;
    const std::size_t close = value.starts_with('"') ? value.find('"', 1) : std::string_view::npos;
    if (close != std::string_view::npos) {
        value_len = close + 1;
    } else {
        value_len = strip_inline_comment(value).size();
        while (value_len > 0 && is_space(value[value_len - 1])) {
            --value_len;
        }
    }
    std::string out(line.substr(0, start));
    out += new_value;
    const std::string_view rest = line.substr(start + value_len);
    // "fps =; note" has no space before the comment; "60;" would then be read as the value.
    if (!rest.empty() && !new_value.empty() && !is_space(rest.front())) {
        out += ' ';
    }
    out += rest;
    return out;
}

}  // namespace

std::string update_ini(std::string_view existing_text, const Settings& settings) {
    std::string base;
    if (trim(existing_text).empty()) {
        for (const char c : default_ini_text()) {
            if (c == '\n') {
                base += '\r';
            }
            base += c;
        }
        existing_text = base;
    }
    const bool crlf = existing_text.find("\r\n") != std::string_view::npos ||
                      existing_text.find('\n') == std::string_view::npos;
    const std::string_view eol = crlf ? "\r\n" : "\n";

    // Lines with their endings; lines[i] is line number i + 1 as parse_ini counts them.
    std::vector<std::string> lines;
    for (std::size_t pos = 0; pos < existing_text.size();) {
        std::size_t end = existing_text.find('\n', pos);
        end = end == std::string_view::npos ? existing_text.size() : end + 1;
        lines.emplace_back(existing_text.substr(pos, end - pos));
        pos = end;
    }

    // parse_ini decides which lines are entries, so the writer cannot disagree with the reader.
    // Duplicates: the last line wins, exactly like load_settings.
    const IniDocument doc = parse_ini(existing_text);
    std::map<std::string, int> last_line_of_key;
    std::map<std::string, int> last_line_of_section;
    for (const IniEntry& e : doc.entries) {
        last_line_of_key[e.section + "." + e.key] = e.line;
        int& last = last_line_of_section[e.section];
        last = std::max(last, e.line);
    }
    // A section header with no keys below it still gets its missing keys right under it.
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string_view raw = lines[i];
        if (i == 0 && raw.starts_with(kUtf8Bom)) {
            raw.remove_prefix(kUtf8Bom.size());
        }
        if (const auto header = section_header(trim(raw)); header && !header->empty()) {
            int& last = last_line_of_section[*header];
            last = std::max(last, static_cast<int>(i) + 1);
        }
    }

    std::map<int, std::vector<std::string>> inserted_after;  // line number -> new key lines
    std::vector<std::string> new_sections;  // in kKnownKeys order
    std::map<std::string, std::vector<std::string>, std::less<>> new_section_lines;
    for (const KnownKey& k : kKnownKeys) {
        const std::string section(k.section);
        const std::string value = k.value(settings);
        if (const auto it = last_line_of_key.find(section + "." + std::string(k.key)); it != last_line_of_key.end()) {
            std::string& line = lines[static_cast<std::size_t>(it->second - 1)];
            line = replace_value(line, value);
        } else if (const auto sec = last_line_of_section.find(section); sec != last_line_of_section.end()) {
            inserted_after[sec->second].push_back(std::format("{} = {}", k.key, value));
        } else {
            if (!new_section_lines.contains(section)) {
                new_sections.push_back(section);
            }
            new_section_lines[section].push_back(std::format("{} = {}", k.key, value));
        }
    }

    std::string out;
    auto end_line = [&] {
        if (!out.empty() && out.back() != '\n') {
            out += eol;
        }
    };
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (const auto it = inserted_after.find(static_cast<int>(i) + 1); it != inserted_after.end()) {
            end_line();
            for (const std::string& l : it->second) {
                out += l;
                out += eol;
            }
        }
    }
    for (const std::string& section : new_sections) {
        end_line();
        if (!trim(out).empty()) {
            out += eol;
        }
        out += std::format("[{}]", section);
        out += eol;
        for (const std::string& l : new_section_lines[section]) {
            out += l;
            out += eol;
        }
    }
    return out;
}

}  // namespace bt
