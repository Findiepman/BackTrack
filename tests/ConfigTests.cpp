#include "core/Config.h"

#include "Check.h"

#include <fstream>
#include <sstream>

using namespace bt;

namespace {

bool same_settings(const Settings& a, const Settings& b) {
    return a == b;
}

bool any_warning(const LoadedSettings& loaded, std::string_view needle) {
    for (const std::string& w : loaded.warnings) {
        if (bt_test::contains(w, needle)) return true;
    }
    return false;
}

void dump_warnings(const LoadedSettings& loaded) {
    for (const std::string& w : loaded.warnings) std::printf("    warning: %s\n", w.c_str());
}

void empty_and_missing() {
    const LoadedSettings empty = load_settings("");
    CHECK(same_settings(empty.settings, Settings{}));
    CHECK(empty.warnings.empty());

    const LoadedSettings only_comments = load_settings("; nothing here\n# or here\n\n   \n");
    CHECK(same_settings(only_comments.settings, Settings{}));
    CHECK(only_comments.warnings.empty());

    // A partial file keeps defaults for everything it does not mention.
    const LoadedSettings partial = load_settings("[video]\nfps = 30\n");
    CHECK_EQ(partial.settings.fps, 30);
    CHECK_EQ(partial.settings.clip_seconds, 30);
    CHECK_EQ(partial.settings.save_hotkey, std::string("F8"));
    CHECK(partial.warnings.empty());
}

void default_file_is_clean() {
    const LoadedSettings loaded = load_settings(default_ini_text());
    CHECK(same_settings(loaded.settings, Settings{}));
    CHECK(loaded.warnings.empty());
    dump_warnings(loaded);
}

void repository_ini_matches_built_in_default() {
    std::ifstream file(std::string(BT_SOURCE_DIR) + "/backtrack.ini", std::ios::binary);
    CHECK(file.good());
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string text = buffer.str();
    std::string normalised;
    for (char c : text) {
        if (c != '\r') normalised += c;  // git may check the file out with CRLF
    }
    CHECK(normalised == default_ini_text());
}

void every_key_is_read() {
    const LoadedSettings loaded = load_settings(R"(
[clip]
seconds = 120
output_dir = D:\Clips
filename = game_%H%M.mp4
[hotkey]
save = ctrl+shift+f12
[video]
monitor = 1
fps = 144
bitrate_kbps = 40000
encoder = software
[audio]
enabled = false
bitrate_kbps = 192
[feedback]
sound = no
notification = off
[logging]
level = debug
)");
    dump_warnings(loaded);
    CHECK(loaded.warnings.empty());
    const Settings& s = loaded.settings;
    CHECK_EQ(s.clip_seconds, 120);
    CHECK_EQ(s.output_dir, std::string("D:\\Clips"));
    CHECK_EQ(s.filename, std::string("game_%H%M.mp4"));
    CHECK_EQ(s.save_hotkey, std::string("Ctrl+Shift+F12"));
    CHECK_EQ(s.monitor, 1);
    CHECK_EQ(s.fps, 144);
    CHECK_EQ(s.video_bitrate_kbps, 40000);
    CHECK(s.encoder == EncoderPreference::Software);
    CHECK(!s.audio_enabled);
    CHECK_EQ(s.audio_bitrate_kbps, 192);
    CHECK(!s.sound);
    CHECK(!s.notification);
    CHECK(s.log_level == LogLevel::Debug);
}

void whitespace_comments_and_line_endings() {
    const LoadedSettings loaded = load_settings(
        "\xEF\xBB\xBF; BOM at the start\r\n"
        "  [ Clip ]  \r\n"
        "\tSECONDS\t=\t45   ; inline comment\r\n"
        "filename = clip#1.mp4\r\n"
        "output_dir=   \"C:\\My Clips ; with semicolon\"   ; real comment\r\n"
        "# hash comment\r\n"
        "[video]\r\n"
        "fps = 30 # hash inline comment");
    dump_warnings(loaded);
    CHECK(loaded.warnings.empty());
    CHECK_EQ(loaded.settings.clip_seconds, 45);
    CHECK_EQ(loaded.settings.filename, std::string("clip#1.mp4"));
    CHECK_EQ(loaded.settings.output_dir, std::string("C:\\My Clips ; with semicolon"));
    CHECK_EQ(loaded.settings.fps, 30);
}

void duplicate_keys_use_the_last_one() {
    const LoadedSettings loaded = load_settings("[clip]\nseconds = 20\nseconds = 40\n");
    CHECK_EQ(loaded.settings.clip_seconds, 40);
    CHECK(any_warning(loaded, "line 3"));
    CHECK(any_warning(loaded, "first on line 2"));

    // The same key in a repeated section is still a duplicate.
    const LoadedSettings sections = load_settings("[video]\nfps=30\n[audio]\n[video]\nfps=120\n");
    CHECK_EQ(sections.settings.fps, 120);
    CHECK(any_warning(sections, "first on line 2"));
}

void unknown_keys_and_sections() {
    const LoadedSettings loaded = load_settings("[clip]\nsecs = 10\n[gpu]\nturbo = yes\n[clip]\nseconds = 10\n");
    CHECK_EQ(loaded.settings.clip_seconds, 10);
    CHECK(any_warning(loaded, "line 2: unknown setting 'secs' in [clip]"));
    CHECK(any_warning(loaded, "line 4: unknown setting 'turbo' in [gpu]"));
    CHECK_EQ(loaded.warnings.size(), std::size_t{2});
}

void malformed_lines() {
    const LoadedSettings loaded = load_settings(
        "seconds = 10\n"  // before any section
        "[clip\n"  // broken header
        "seconds = 11\n"  // ignored: no valid section
        "[]\n"
        "[clip]\n"
        "just some words\n"
        "= 12\n"
        "seconds = 13\n"
        "filename = \"unterminated.mp4\n");
    CHECK_EQ(loaded.settings.clip_seconds, 13);
    CHECK(any_warning(loaded, "line 1: 'seconds' is not inside"));
    CHECK(any_warning(loaded, "line 2: '[clip' is not a valid"));
    CHECK(any_warning(loaded, "line 3: 'seconds' is not inside"));
    CHECK(any_warning(loaded, "line 4: '[]' is not a valid"));
    CHECK(any_warning(loaded, "line 6: expected 'key = value'"));
    CHECK(any_warning(loaded, "line 7: '= 12' has no key"));
    CHECK(any_warning(loaded, "line 9: filename has an opening quote"));
    CHECK_EQ(loaded.settings.filename, std::string("unterminated.mp4"));
}

void bad_values_keep_defaults() {
    const LoadedSettings loaded = load_settings(R"([clip]
seconds = thirty
output_dir =
filename =
[video]
fps = fast
monitor = -1
bitrate_kbps = 99999999999999999999
encoder = gpu
[audio]
enabled = maybe
bitrate_kbps = 1.5
[logging]
level = verbose
[hotkey]
save = Ctrl+
)");
    const Settings& s = loaded.settings;
    CHECK_EQ(s.clip_seconds, 30);
    CHECK(any_warning(loaded, "line 2: seconds = 'thirty' is not a whole number, keeping 30"));
    CHECK_EQ(s.output_dir, Settings{}.output_dir);
    CHECK(any_warning(loaded, "line 3: output_dir is empty"));
    CHECK_EQ(s.filename, Settings{}.filename);
    CHECK(any_warning(loaded, "line 4: filename is empty"));
    CHECK_EQ(s.fps, 60);
    CHECK(any_warning(loaded, "line 6:"));
    CHECK_EQ(s.monitor, 0);
    CHECK(any_warning(loaded, "line 7: monitor = -1 is below the minimum 0"));
    CHECK_EQ(s.video_bitrate_kbps, 20000);
    CHECK(any_warning(loaded, "line 8:"));
    CHECK(s.encoder == EncoderPreference::Auto);
    CHECK(any_warning(loaded, "line 9:"));
    CHECK(s.audio_enabled);
    CHECK(any_warning(loaded, "line 11:"));
    CHECK_EQ(s.audio_bitrate_kbps, 160);
    CHECK(any_warning(loaded, "line 12:"));
    CHECK(s.log_level == LogLevel::Info);
    CHECK(any_warning(loaded, "line 14:"));
    CHECK_EQ(s.save_hotkey, std::string("F8"));
    CHECK(any_warning(loaded, "line 16: save = 'Ctrl+'"));
    CHECK_EQ(loaded.warnings.size(), std::size_t{11});
}

void ranges_and_snapping() {
    auto seconds = [](const char* v) { return load_settings(std::format("[clip]\nseconds={}", v)); };
    CHECK_EQ(seconds("3").settings.clip_seconds, 5);
    CHECK(any_warning(seconds("3"), "below the minimum 5"));
    CHECK_EQ(seconds("1000").settings.clip_seconds, 300);
    CHECK(any_warning(seconds("1000"), "above the maximum 300"));
    CHECK_EQ(seconds("5").settings.clip_seconds, 5);
    CHECK(seconds("5").warnings.empty());
    CHECK_EQ(seconds("300").settings.clip_seconds, 300);
    CHECK_EQ(seconds("+60").settings.clip_seconds, 60);

    auto fps = [](const char* v) { return load_settings(std::format("[video]\nfps={}", v)).settings.fps; };
    CHECK_EQ(fps("30"), 30);
    CHECK_EQ(fps("60"), 60);
    CHECK_EQ(fps("120"), 120);
    CHECK_EQ(fps("144"), 144);
    CHECK_EQ(fps("50"), 60);
    CHECK_EQ(fps("25"), 30);
    CHECK_EQ(fps("45"), 30);  // tie goes to the lower rate
    CHECK_EQ(fps("165"), 144);
    CHECK_EQ(fps("0"), 30);
    CHECK_EQ(fps("-60"), 30);
    CHECK(any_warning(load_settings("[video]\nfps=50"), "fps = 50 is not supported (30, 60, 120 or 144), using 60"));

    auto audio = [](const char* v) { return load_settings(std::format("[audio]\nbitrate_kbps={}", v)).settings.audio_bitrate_kbps; };
    CHECK_EQ(audio("96"), 96);
    CHECK_EQ(audio("100"), 96);
    CHECK_EQ(audio("150"), 160);
    CHECK_EQ(audio("144"), 128);  // tie
    CHECK_EQ(audio("320"), 192);

    auto video = [](const char* v) { return load_settings(std::format("[video]\nbitrate_kbps={}", v)).settings.video_bitrate_kbps; };
    CHECK_EQ(video("500"), 1000);
    CHECK_EQ(video("1000"), 1000);
    CHECK_EQ(video("100000"), 100000);
    CHECK_EQ(video("200000"), 100000);
}

void booleans_and_enums() {
    for (const char* yes : {"true", "TRUE", "yes", "on", "1"}) {
        CHECK(load_settings(std::format("[feedback]\nsound={}", yes)).settings.sound);
    }
    for (const char* no : {"false", "False", "no", "off", "0"}) {
        CHECK(!load_settings(std::format("[feedback]\nsound={}", no)).settings.sound);
    }
    CHECK(load_settings("[video]\nencoder=HARDWARE").settings.encoder == EncoderPreference::Hardware);
    CHECK(load_settings("[video]\nencoder=Auto").settings.encoder == EncoderPreference::Auto);
    CHECK(load_settings("[logging]\nlevel=ERROR").settings.log_level == LogLevel::Error);
}

void file_names() {
    const LoadedSettings no_ext = load_settings("[clip]\nfilename = clip_%H");
    CHECK_EQ(no_ext.settings.filename, std::string("clip_%H.mp4"));
    CHECK(any_warning(no_ext, "does not end in .mp4"));

    CHECK_EQ(load_settings("[clip]\nfilename = Clip.MP4").settings.filename, std::string("Clip.MP4"));

    const LoadedSettings with_folder = load_settings("[clip]\nfilename = sub\\clip.mp4");
    CHECK_EQ(with_folder.settings.filename, Settings{}.filename);
    CHECK(any_warning(with_folder, "folder separator"));
}

void hotkeys() {
    CHECK_EQ(load_settings("[hotkey]\nsave = ctrl + alt + f8").settings.save_hotkey, std::string("Ctrl+Alt+F8"));
    const LoadedSettings bad = load_settings("[hotkey]\nsave = Ctrl+Banana");
    CHECK_EQ(bad.settings.save_hotkey, std::string("F8"));
    CHECK(any_warning(bad, "line 2: save = 'Ctrl+Banana': 'Banana' is not a key"));
    CHECK(any_warning(bad, "Using F8"));
}

void review_findings() {
    // A comment after a section header keeps the section.
    const LoadedSettings header = load_settings("[video] ; my video settings\nfps = 30\n[clip]# x\nseconds = 9\n");
    CHECK(header.warnings.empty());
    CHECK_EQ(header.settings.fps, 30);
    CHECK_EQ(header.settings.clip_seconds, 9);

    // Extreme integers must not overflow while snapping or clamping.
    CHECK_EQ(load_settings("[video]\nfps = -9223372036854775808").settings.fps, 30);
    CHECK_EQ(load_settings("[video]\nfps = 9223372036854775807").settings.fps, 144);
    CHECK_EQ(load_settings("[audio]\nbitrate_kbps = -9223372036854775808").settings.audio_bitrate_kbps, 96);
    CHECK_EQ(load_settings("[clip]\nseconds = -9223372036854775808").settings.clip_seconds, 5);

    // A path cut short by the inline comment rule says so; quoting avoids it.
    const LoadedSettings cut = load_settings("[clip]\noutput_dir = D:\\Clips #2\n");
    CHECK_EQ(cut.settings.output_dir, std::string("D:\\Clips"));
    CHECK(any_warning(cut, "line 2: output_dir: the text after ';' or '#' was read as a comment"));
    const LoadedSettings quoted = load_settings("[clip]\noutput_dir = \"D:\\Clips #2\" ; comment\n");
    CHECK_EQ(quoted.settings.output_dir, std::string("D:\\Clips #2"));
    CHECK(quoted.warnings.empty());
    CHECK(load_settings("[clip]\noutput_dir = D:\\Clips#2\n").warnings.empty());
    const LoadedSettings unclosed = load_settings("[clip]\nfilename = \"clip#1.mp4\n");
    CHECK_EQ(unclosed.settings.filename, std::string("clip#1.mp4"));
    CHECK(!any_warning(unclosed, "read as a comment"));
    CHECK(any_warning(unclosed, "no closing quote"));
    const LoadedSettings unclosed_cut = load_settings("[clip]\nfilename = \"clip #1.mp4\n");
    CHECK_EQ(unclosed_cut.settings.filename, std::string("clip.mp4"));
    CHECK(any_warning(unclosed_cut, "no closing quote"));
    CHECK(any_warning(unclosed_cut, "read as a comment"));
    const LoadedSettings only_comment = load_settings("[clip]\noutput_dir = ; leave default\n");
    CHECK(any_warning(only_comment, "output_dir is empty"));
    CHECK(!any_warning(only_comment, "read as a comment"));

    // File names Windows or strftime cannot handle.
    for (const char* bad : {"clip_%H:%M.mp4", "clip?.mp4", "clip*.mp4", "a<b.mp4", "a|b.mp4", "a\"b.mp4"}) {
        const LoadedSettings loaded = load_settings(std::format("[clip]\nfilename = {}", bad));
        CHECK_EQ(loaded.settings.filename, Settings{}.filename);
        CHECK(any_warning(loaded, "does not allow"));
    }
    for (const char* bad : {"clip_%c.mp4", "clip_%T.mp4", "clip_%D.mp4", "clip_%Q.mp4", "clip_%.mp4", "clip_%"}) {
        const LoadedSettings loaded = load_settings(std::format("[clip]\nfilename = {}", bad));
        CHECK_EQ(loaded.settings.filename, Settings{}.filename);
        CHECK(any_warning(loaded, "not allowed in a file name"));
    }
    const LoadedSettings safe = load_settings("[clip]\nfilename = %Y%m%d_%H%M%S_%j_%a_%A_%b_%B_%p_%I_%U_%W_%w_%y_100%%.mp4");
    dump_warnings(safe);
    CHECK(safe.warnings.empty());

    // A valid hotkey that Windows often takes produces a warning but is kept.
    const LoadedSettings print = load_settings("[hotkey]\nsave = PrintScreen");
    CHECK_EQ(print.settings.save_hotkey, std::string("PrintScreen"));
    CHECK(any_warning(print, "Snipping"));
    CHECK_EQ(load_settings("[hotkey]\nsave = F12").settings.save_hotkey, std::string("F8"));
}


Settings non_default_settings() {
    Settings s;
    s.start_minimized = true;
    s.clip_seconds = 120;
    s.output_dir = "D:\\Games\\Clips";
    s.filename = "game_%H%M.mp4";
    s.max_gb = 250;
    s.delete_after_days = 30;
    s.save_hotkey = "Ctrl+Shift+F12";
    s.monitor = 2;
    s.fps = 144;
    s.video_bitrate_kbps = 40000;
    s.encoder = EncoderPreference::Software;
    s.audio_enabled = false;
    s.audio_bitrate_kbps = 192;
    s.system_volume = 35;
    s.mic_enabled = false;
    s.mic_device = "{0.0.1.00000000}.{6c1a3b0e-1111-4c44-9d9d-0123456789ab}";
    s.mic_volume = 180;
    s.noise_suppression = false;
    s.push_to_talk = true;
    s.mic_mute_hotkey = "";
    s.push_to_talk_key = "Mouse4";
    s.sound = false;
    s.notification = false;
    s.saved_bar = false;
    s.log_level = LogLevel::Debug;
    s.auto_update = false;
    s.save_seconds = 60;
    s.overlay_hotkey = "";
    s.bookmark_hotkey = "Ctrl+Alt+B";
    s.resolution = 720;
    s.quality = QualityPreset::Quality;
    s.capture_cursor = false;
    s.system_device = "{0.0.0.00000000}.{aa1a3b0e-1111-4c44-9d9d-0123456789ab}";
    s.separate_tracks = true;
    s.overlay = false;
    s.overlay_corner = OverlayCorner::BottomLeft;
    s.overlay_opacity = 40;
    s.theme = ThemeId::Daylight;
    s.density = Density::Compact;
    s.thumbnail_size = ThumbnailSize::Large;
    s.reduce_motion = true;
    s.share_host = ShareHost::Litterbox;
    s.share_copy_link = false;
    return s;
}

void round_trips(std::string_view base, const Settings& s) {
    const std::string written = update_ini(base, s);
    const LoadedSettings loaded = load_settings(written);
    CHECK(loaded.settings == s);
    if (!(loaded.settings == s)) std::printf("    written:\n%s\n", written.c_str());
}

void new_keys_parse() {
    const LoadedSettings loaded = load_settings(
        "[general]\nstart_minimized = yes\n[storage]\nmax_gb = 500\ndelete_after_days = 14\n"
        "[feedback]\nsaved_bar = off\n");
    dump_warnings(loaded);
    CHECK(loaded.warnings.empty());
    CHECK(loaded.settings.start_minimized);
    CHECK_EQ(loaded.settings.max_gb, 500);
    CHECK_EQ(loaded.settings.delete_after_days, 14);
    CHECK(!loaded.settings.saved_bar);

    const LoadedSettings bad = load_settings(
        "[general]\nstart_minimized = maybe\n[storage]\nmax_gb = 100001\ndelete_after_days = -1\n"
        "[feedback]\nsaved_bar = 2\n");
    CHECK(!bad.settings.start_minimized);
    CHECK(any_warning(bad, "line 2: start_minimized = 'maybe' should be true or false"));
    CHECK_EQ(bad.settings.max_gb, 100000);
    CHECK(any_warning(bad, "line 4: max_gb = 100001 is above the maximum 100000"));
    CHECK_EQ(bad.settings.delete_after_days, 0);
    CHECK(any_warning(bad, "line 5: delete_after_days = -1 is below the minimum 0"));
    CHECK(bad.settings.saved_bar);
    CHECK(any_warning(bad, "line 7: saved_bar"));
    CHECK_EQ(load_settings("[storage]\ndelete_after_days = 3651").settings.delete_after_days, 3650);
    CHECK_EQ(load_settings("[storage]\nmax_gb = lots").settings.max_gb, 0);
    CHECK(load_settings("[logging]\nlevel = warning").settings.log_level == LogLevel::Warning);
}

void writer_round_trips_every_field() {
    const Settings s = non_default_settings();
    round_trips(default_ini_text(), s);
    round_trips("", s);
    round_trips("[clip]\nseconds = 5\n", s);
    round_trips(update_ini("", s), Settings{});
    // Every enum value.
    for (const auto e : {EncoderPreference::Auto, EncoderPreference::Hardware, EncoderPreference::Software}) {
        Settings t;
        t.encoder = e;
        round_trips(default_ini_text(), t);
    }
    for (const auto l : {LogLevel::Error, LogLevel::Warning, LogLevel::Info, LogLevel::Debug}) {
        Settings t;
        t.log_level = l;
        round_trips(default_ini_text(), t);
    }
    // Writing the same settings twice changes nothing.
    const std::string once = update_ini(default_ini_text(), s);
    CHECK_EQ(update_ini(once, s), once);
    // Default settings into the default file give the default file back.
    CHECK(update_ini(default_ini_text(), Settings{}) == default_ini_text());
    CHECK(load_settings(update_ini(default_ini_text(), s)).warnings.empty());
}

void writer_keeps_everything_else() {
    const std::string before =
        "; my file\r\n"
        "\r\n"
        "[Video]   ; header comment\r\n"
        "  FPS\t=   30 ; my note\r\n"
        "turbo = yes\r\n"
        "encoder=hardware\r\n"
        "# hash\r\n"
        "[gpu]\r\n"
        "boost = 1\r\n";
    Settings s;
    s.fps = 144;
    s.encoder = EncoderPreference::Auto;
    const std::string after = update_ini(before, s);
    CHECK(bt_test::contains(after, "; my file\r\n\r\n[Video]   ; header comment\r\n  FPS\t=   144 ; my note\r\n"
                                   "turbo = yes\r\nencoder=auto\r\n"));
    CHECK(bt_test::contains(after, "# hash\r\n[gpu]\r\nboost = 1\r\n"));
    // Missing [video] keys go after the last key line of the section, before the comment.
    CHECK(bt_test::contains(after, "encoder=auto\r\nmonitor = 0\r\nbitrate_kbps = 20000\r\nresolution = native\r\n"
                                   "quality = balanced\r\ncursor = true\r\n# hash"));
    // No bare LF anywhere.
    for (std::size_t i = 0; i < after.size(); ++i) {
        if (after[i] == '\n') CHECK(i > 0 && after[i - 1] == '\r');
    }
    round_trips(before, s);

    // LF files stay LF.
    const std::string lf = update_ini("[video]\nfps = 30\n", s);
    CHECK(!bt_test::contains(lf, "\r"));
    CHECK(lf.starts_with("[video]\nfps = 144\n"));

    // A comment right after '=' with no value gets a space so it stays a comment.
    CHECK(update_ini("[video]\nfps =; note\n", s).starts_with("[video]\nfps =144 ; note\n"));
    // An old quoted value is replaced including its quotes and the text after them.
    CHECK(update_ini("[video]\nencoder = \"hardware\" ; x\n", s).starts_with("[video]\nencoder = auto ; x\n"));
    // A last line without a line ending.
    round_trips("[video]\nfps = 30", s);
    CHECK(update_ini("[video]\nfps = 30", s).starts_with("[video]\nfps = 144\n"));
    // A BOM stays.
    CHECK(update_ini("\xEF\xBB\xBF[video]\nfps = 30\n", s).starts_with("\xEF\xBB\xBF[video]\nfps = 144\n"));
}

void writer_duplicates_and_missing() {
    Settings s;
    s.clip_seconds = 60;
    const std::string dup = update_ini("[clip]\nseconds = 20\nseconds = 40\n", s);
    CHECK(dup.starts_with("[clip]\nseconds = 20\nseconds = 60\n"));
    CHECK_EQ(load_settings(dup).settings.clip_seconds, 60);
    // Repeated section: the later block holds the key load_settings uses.
    const std::string sections = update_ini("[clip]\nseconds = 20\n[audio]\n[clip]\nseconds = 40\n", s);
    CHECK(sections.starts_with("[clip]\nseconds = 20\n[audio]\nenabled = true\nbitrate_kbps = 160\nsystem_volume = 100\n"
                               "system_device = \nseparate_tracks = false\nmic = true\nmic_device = \nmic_volume = 100\nnoise_suppression = true\n"
                               "push_to_talk = false\n[clip]\nseconds = 60\n"));
    if (!sections.starts_with("[clip]\nseconds = 20\n[audio]\nenabled = true\nbitrate_kbps = 160\nsystem_volume = 100\n")) {
        std::printf("    written:\n%s\n", sections.c_str());
    }

    // Missing section appended at the end with its keys; empty header gets keys below it.
    const std::string missing = update_ini("[storage]\n\n[logging]\nlevel = debug\n", s);
    CHECK(missing.starts_with("[storage]\nmax_gb = 0\ndelete_after_days = 0\n\n[logging]\nlevel = info\n"));
    CHECK(bt_test::contains(missing, "\n\n[general]\nstart_minimized = false\n"));
    CHECK(bt_test::contains(missing, "\n\n[feedback]\nsound = true\nnotification = true\nsaved_bar = true\n"));
    CHECK(load_settings(missing).warnings.empty());

    // An old file without the new keys gets them in the right sections.
    const std::string old = update_ini("[feedback]\nsound = true\nnotification = true\n\n[logging]\nlevel = info\n",
                                       Settings{});
    CHECK(old.starts_with("[feedback]\nsound = true\nnotification = true\nsaved_bar = true\n\n[logging]\n"));

    // Empty text becomes the default file in CRLF.
    std::string crlf_default;
    for (const char c : default_ini_text()) {
        if (c == '\n') crlf_default += '\r';
        crlf_default += c;
    }
    CHECK(update_ini("", Settings{}) == crlf_default);
    CHECK(update_ini("  \r\n", Settings{}) == crlf_default);
}

void writer_quotes_strings() {
    Settings s;
    s.output_dir = "D:\\Clips #2";
    CHECK(bt_test::contains(update_ini(default_ini_text(), s), "output_dir = \"D:\\Clips #2\"\n"));
    round_trips(default_ini_text(), s);
    s.output_dir = "C:\\a;b";
    CHECK(bt_test::contains(update_ini(default_ini_text(), s), "output_dir = \"C:\\a;b\"\n"));
    round_trips(default_ini_text(), s);
    s.output_dir = "  spaced  ";
    round_trips(default_ini_text(), s);
    s.filename = "clip#1.mp4";
    round_trips(default_ini_text(), s);
    // A quote inside a value that needs no quoting is fine unquoted.
    s.output_dir = "D:\\say \"hi\"";
    round_trips(default_ini_text(), s);
    // A value that starts with '"' cannot be represented: it is cut, and the file still parses.
    s.output_dir = "\"D:\\x\" ; y";
    const LoadedSettings cut = load_settings(update_ini(default_ini_text(), s));
    CHECK(cut.settings.output_dir == std::string("%USERPROFILE%\\Videos\\Backtrack"));  // empty, kept default
    CHECK(any_warning(cut, "output_dir is empty"));
    // Line breaks are dropped instead of breaking the file.
    s.output_dir = "D:\\a\r\nb";
    CHECK_EQ(load_settings(update_ini(default_ini_text(), s)).settings.output_dir, std::string("D:\\ab"));
}

}  // namespace

void mic_settings() {
    // Defaults: the mic is on, mixed in, with noise suppression, no push-to-talk.
    const Settings d{};
    CHECK(d.mic_enabled && d.noise_suppression && !d.push_to_talk);
    CHECK(d.mic_device.empty() && d.mic_volume == 100 && d.system_volume == 100);
    CHECK(d.mic_mute_hotkey == "Alt+F7" && d.push_to_talk_key == "V");

    const LoadedSettings loaded = load_settings(
        "[audio]\nmic = false\nmic_volume = 250\nsystem_volume = -5\nmic_device =\nnoise_suppression = maybe\n"
        "[hotkey]\nmic_mute =\npush_to_talk = v\n");
    CHECK(!loaded.settings.mic_enabled);
    CHECK(loaded.settings.mic_volume == 200);  // clamped
    CHECK(loaded.settings.system_volume == 0);
    CHECK(loaded.settings.mic_device.empty());
    CHECK(loaded.settings.noise_suppression);  // bad value keeps the default
    CHECK(loaded.settings.mic_mute_hotkey.empty());  // empty: no mute key
    CHECK(loaded.settings.push_to_talk_key == "V");  // a plain letter is fine for a held key
    CHECK(loaded.warnings.size() == 3);

    // A mute key must still be a valid global hotkey: a plain letter is refused, the default kept.
    const LoadedSettings bad = load_settings("[hotkey]\nmic_mute = M\npush_to_talk = Ctrl+Ctrl\n");
    CHECK(bad.settings.mic_mute_hotkey == "Alt+F7");
    CHECK(bad.settings.push_to_talk_key == "V");
    CHECK(bad.warnings.size() == 2);
}

void round_eleven_settings() {
    // Defaults keep the old behaviour where there was one: whole buffer saved, one sound track.
    const Settings d{};
    CHECK(d.save_seconds == 0 && !d.separate_tracks && d.system_device.empty() && d.resolution == 0);
    CHECK(d.quality == QualityPreset::Balanced && d.capture_cursor && d.overlay && d.auto_update);
    CHECK(d.overlay_corner == OverlayCorner::TopRight && d.overlay_opacity == 85);
    CHECK(d.theme == ThemeId::Night && d.density == Density::Comfortable && d.thumbnail_size == ThumbnailSize::Medium);

    const LoadedSettings loaded = load_settings(
        "[general]\nauto_update = no\n[clip]\nsave_seconds = 15\n[hotkey]\noverlay =\nbookmark = ctrl+b\n"
        "[video]\nresolution = 1080\nquality = Performance\ncursor = false\n"
        "[audio]\nsystem_device = {x}\nseparate_tracks = yes\n"
        "[overlay]\nshow = off\nposition = BOTTOM_RIGHT\nopacity = 55\n"
        "[appearance]\ntheme = plum\ndensity = compact\nthumbnail_size = small\nreduce_motion = on\n");
    dump_warnings(loaded);
    CHECK(loaded.warnings.empty());
    const Settings& s = loaded.settings;
    CHECK(!s.auto_update && s.save_seconds == 15 && s.overlay_hotkey.empty() && s.bookmark_hotkey == "Ctrl+B");
    CHECK(s.resolution == 1080 && s.quality == QualityPreset::Performance && !s.capture_cursor);
    CHECK(s.system_device == "{x}" && s.separate_tracks);
    CHECK(!s.overlay && s.overlay_corner == OverlayCorner::BottomRight && s.overlay_opacity == 55);
    CHECK(s.theme == ThemeId::Plum && s.density == Density::Compact && s.thumbnail_size == ThumbnailSize::Small);
    CHECK(s.reduce_motion);

    // Bad values keep the defaults with one warning each; numbers are clamped or snapped.
    const LoadedSettings bad = load_settings(
        "[clip]\nsave_seconds = 900\n[video]\nresolution = 1000\nquality = ultra\n"
        "[overlay]\nposition = middle\nopacity = 5\n[appearance]\ntheme = neon\nthumbnail_size = 3\n"
        "[hotkey]\nbookmark = B\n");
    CHECK_EQ(bad.settings.save_seconds, 300);
    CHECK_EQ(bad.settings.resolution, 1080);
    CHECK(bad.settings.quality == QualityPreset::Balanced);
    CHECK(bad.settings.overlay_corner == OverlayCorner::TopRight);
    CHECK_EQ(bad.settings.overlay_opacity, 20);
    CHECK(bad.settings.theme == ThemeId::Night && bad.settings.thumbnail_size == ThumbnailSize::Medium);
    CHECK(bad.settings.bookmark_hotkey == "Alt+Shift+B");  // a plain letter is no global hotkey
    CHECK_EQ(bad.warnings.size(), std::size_t{8});
    CHECK(any_warning(bad, "theme = 'neon' should be night, harbor, plum, moss or daylight, keeping night"));
    CHECK_EQ(load_settings("[video]\nresolution = NATIVE\n").settings.resolution, 0);

    for (const auto q : {QualityPreset::Performance, QualityPreset::Balanced, QualityPreset::Quality}) {
        Settings t;
        t.quality = q;
        round_trips(default_ini_text(), t);
    }
    for (const auto c : {OverlayCorner::TopLeft, OverlayCorner::TopRight, OverlayCorner::BottomLeft,
                         OverlayCorner::BottomRight}) {
        Settings t;
        t.overlay_corner = c;
        round_trips(default_ini_text(), t);
    }
    for (const auto th : {ThemeId::Night, ThemeId::Harbor, ThemeId::Plum, ThemeId::Moss, ThemeId::Daylight}) {
        Settings t;
        t.theme = th;
        round_trips(default_ini_text(), t);
    }
    for (const int r : {0, 720, 1080, 1440}) {
        Settings t;
        t.resolution = r;
        round_trips(default_ini_text(), t);
    }
    // A file from 0.3.0 gets the new sections appended, and reads back without warnings.
    const std::string old = update_ini("[feedback]\nsound = true\n\n[logging]\nlevel = info\n", Settings{});
    CHECK(bt_test::contains(old, "[overlay]\nshow = true\nposition = top_right\nopacity = 85\n"));
    CHECK(load_settings(old).warnings.empty());
}

int main() {
    round_eleven_settings();
    mic_settings();
    empty_and_missing();
    default_file_is_clean();
    repository_ini_matches_built_in_default();
    every_key_is_read();
    whitespace_comments_and_line_endings();
    duplicate_keys_use_the_last_one();
    unknown_keys_and_sections();
    malformed_lines();
    bad_values_keep_defaults();
    ranges_and_snapping();
    booleans_and_enums();
    file_names();
    hotkeys();
    review_findings();
    new_keys_parse();
    writer_round_trips_every_field();
    writer_keeps_everything_else();
    writer_duplicates_and_missing();
    writer_quotes_strings();
    return bt_test::finish("ConfigTests");
}
