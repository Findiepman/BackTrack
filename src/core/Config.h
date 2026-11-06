#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace bt {

enum class EncoderPreference { Auto, Hardware, Software };
// The encoder's speed/quality trade-off at the same bitrate (CODECAPI_AVEncCommonQualityVsSpeed).
enum class QualityPreset { Performance, Balanced, Quality };
enum class OverlayCorner { TopLeft, TopRight, BottomLeft, BottomRight };
// The colour presets of the mockup (ST_PRESETS), in its order.
enum class ThemeId { Night, Harbor, Plum, Moss, Daylight };
enum class Density { Comfortable, Compact };
enum class ThumbnailSize { Small, Medium, Large };

// Ordered by severity so "level <= threshold" means "should be logged".
enum class LogLevel { Error, Warning, Info, Debug };

// Typed settings with the defaults used when a key is missing (§5.1).
struct Settings {
    // [general]
    bool start_minimized = false;  // when started with Windows, open in the tray
    bool auto_update = true;  // download new versions and install them when Backtrack closes
    // [clip]
    int clip_seconds = 30;  // the replay buffer's length
    int save_seconds = 0;  // what the save hotkey keeps; 0 or more than clip_seconds: the whole buffer
    std::string output_dir = "%USERPROFILE%\\Videos\\Backtrack";
    std::string filename = "clip_%Y-%m-%d_%H-%M-%S.mp4";
    // [storage]
    int max_gb = 0;  // 0 = no limit
    int delete_after_days = 0;  // 0 = never
    // [hotkey]
    std::string save_hotkey = "F8";  // always a string parse_hotkey accepts
    std::string mic_mute_hotkey = "Alt+F7";  // empty: no mute key; else a Global hotkey
    std::string push_to_talk_key = "V";  // a Held key (parse_hotkey with HotkeyUse::Held)
    std::string overlay_hotkey = "Alt+Shift+O";  // shows or hides the overlay; empty: no key
    std::string bookmark_hotkey = "Alt+Shift+B";  // marks a moment in the replay buffer; empty: no key
    // [video]
    int monitor = 0;
    int fps = 60;
    int video_bitrate_kbps = 20000;
    EncoderPreference encoder = EncoderPreference::Auto;
    int resolution = 0;  // output height: 0 = the display's own size, else 720, 1080 or 1440 (never scaled up)
    QualityPreset quality = QualityPreset::Balanced;
    bool capture_cursor = true;  // draw the mouse pointer into the video
    // [audio]
    bool audio_enabled = true;  // what you hear (game, voice chat, music)
    int audio_bitrate_kbps = 160;
    int system_volume = 100;  // percent, 0..200, of what you hear in the clip
    std::string system_device;  // playback endpoint id to record; empty: the Windows default (followed when it changes)
    bool separate_tracks = false;  // besides the mixed track: one with only the system sound, one with only the mic
    bool mic_enabled = true;
    std::string mic_device;  // Windows endpoint id; empty: the Windows default microphone (followed when it changes)
    int mic_volume = 100;  // percent, 0..200
    bool noise_suppression = true;
    bool push_to_talk = false;  // the mic is only recorded while push_to_talk_key is held
    // [feedback]
    bool sound = true;
    bool notification = true;
    bool saved_bar = true;  // the "Saved" bar in the window
    // [overlay]: a small status chip on top of the screen, never part of recordings
    bool overlay = true;
    OverlayCorner overlay_corner = OverlayCorner::TopRight;
    int overlay_opacity = 85;  // percent, 20..100
    // [appearance]
    ThemeId theme = ThemeId::Night;
    Density density = Density::Comfortable;
    ThumbnailSize thumbnail_size = ThumbnailSize::Medium;
    bool reduce_motion = false;
    // [logging]
    LogLevel log_level = LogLevel::Info;

    friend bool operator==(const Settings&, const Settings&) = default;
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

// What the save hotkey keeps: save_seconds, or the whole buffer when that is 0 or longer.
inline int saved_seconds(const Settings& s) {
    return s.save_seconds > 0 && s.save_seconds < s.clip_seconds ? s.save_seconds : s.clip_seconds;
}

IniDocument parse_ini(std::string_view text);

struct LoadedSettings {
    Settings settings;
    std::vector<std::string> warnings;  // one per problem, each starts with "line N: "
};

// Never fails: every bad or unknown value produces a warning and keeps the default (§5.1).
LoadedSettings load_settings(std::string_view ini_text);

// The commented file written on first run. Identical to backtrack.ini in the repository.
std::string_view default_ini_text();

// Returns existing_text with the value of every known key set from settings. Everything else is
// kept byte for byte: comments, blank lines, unknown keys and sections, key order, spacing around
// '=', inline comments after the value and the line ending style. When a key appears twice the
// last one (the one load_settings uses) is changed. A missing key is added after the last line of
// its section, a missing section at the end of the file. Empty text gives the default file (CRLF)
// with these values. Strings that need it are written in double quotes. The INI format has no
// escape for '"', so a string that needs quotes and also contains '"' is cut at that quote, and
// line breaks inside a string are dropped.
std::string update_ini(std::string_view existing_text, const Settings& settings);

}  // namespace bt
