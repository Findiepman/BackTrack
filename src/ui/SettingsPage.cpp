// The Settings page (backtrack.html #/settings) and everything that changes settings while
// Backtrack runs: writing backtrack.ini back, restarting Instant Replay when a setting needs a new
// pipeline, the "launch with Windows" entry, the save hotkey and the clips folder.
//
// The rows are data (SettingRow): one list drives the section pages, the search, the "changed"
// dots and the reset buttons. Rows for things the engine cannot do yet are shown greyed with a
// "Soon" tag, as in the mockup, so the page keeps its shape.

#include "core/Hotkey.h"
#include "platform/IniFile.h"
#include "platform/Log.h"
#include "platform/Shell.h"
#include "platform/Text.h"
#include "ui/App.h"
#include "ui/AppIds.h"
#include "ui/Theme.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <tuple>

#ifndef BT_VERSION
#define BT_VERSION "0.0.0"
#endif

namespace bt::ui {

using namespace theme;

struct App::SettingRow {
    enum class Kind { Toggle, Select, Segmented, Slider, Custom };
    std::string id;
    std::string section;
    Kind kind = Kind::Toggle;
    std::wstring label;
    std::wstring description;
    std::wstring keywords;  // extra words the search finds it by
    bool soon = false;  // not available yet: greyed with a "Soon" tag
    bool full = false;  // the control goes below the label, full width
    bool changed = false;  // differs from the default (a dot after the label)
    bool on = false;  // Toggle
    std::vector<std::wstring> options;  // Select, Segmented
    int selected = 0;
    float value = 0;  // Slider, 0..1
    std::wstring value_text;  // Slider: "20 Mbps"
    float height = 0;  // Custom: the control's height
    std::function<void(int choice)> choose;  // Toggle (0/1), Select and Segmented (index)
    std::function<void(float fraction, bool done)> drag;  // Slider
    std::function<std::wstring(float fraction)> preview;  // Slider: the text while dragging
    std::function<void(Settings& defaults)> reset;  // puts the default back (into the settings being edited)
    std::function<void(const Rect& area)> paint;  // Custom
};

namespace {

namespace fs = std::filesystem;

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Backtrack";
constexpr auto kRestartDelay = std::chrono::milliseconds(1500);

struct Section {
    const char* id;
    const wchar_t* title;
    wchar_t glyph;
    const wchar_t* description;
};
constexpr Section kSections[] = {
    {"general", L"General", glyph::kSettings, L"How Backtrack starts and stays up to date."},
    {"capture", L"Capture & Video", glyph::kFilm, L"Backtrack always captures an entire display, never a single window."},
    {"replay", L"Instant Replay", glyph::kBolt, L"A rolling buffer that lets you save what just happened."},
    {"audio", L"Audio", glyph::kHeadphones, L"Devices and levels for your recordings."},
    {"hotkeys", L"Hotkeys", glyph::kKeyboard, L"Click a shortcut, then press the keys you want. Esc cancels."},
    {"storage", L"Storage", glyph::kDrive, L"Where clips live and how much room they can use."},
    {"sharing", L"Sharing & Accounts", glyph::kShare, L"Connect the places you send clips to."},
    {"notify", L"Notifications & Overlay", glyph::kBell, L"How Backtrack tells you something was saved."},
    {"appearance", L"Appearance", glyph::kPalette, L"Make Backtrack look the way you like."},
    {"about", L"About", glyph::kInfo, L""},
};

const Section* find_section(std::string_view id) {
    for (const Section& s : kSections) {
        if (id == s.id) return &s;
    }
    return nullptr;
}

// "45 s", "1 min", "1 min 30 s", "5 min".
std::wstring length_text(int seconds) {
    if (seconds < 60) return std::format(L"{} s", seconds);
    if (seconds % 60 == 0) return std::format(L"{} min", seconds / 60);
    return std::format(L"{} min {} s", seconds / 60, seconds % 60);
}

// The settings a recorder is made with, copied from `from` into `to`.
void copy_pipeline(const Settings& from, Settings& to) {
    to.clip_seconds = from.clip_seconds;
    to.output_dir = from.output_dir;
    to.filename = from.filename;
    to.monitor = from.monitor;
    to.fps = from.fps;
    to.video_bitrate_kbps = from.video_bitrate_kbps;
    to.encoder = from.encoder;
    to.audio_enabled = from.audio_enabled;
    to.audio_bitrate_kbps = from.audio_bitrate_kbps;
    to.mic_enabled = from.mic_enabled;
    to.mic_device = from.mic_device;
    to.resolution = from.resolution;
    to.quality = from.quality;
    to.capture_cursor = from.capture_cursor;
    to.system_device = from.system_device;
    to.separate_tracks = from.separate_tracks;
}

bool needs_new_pipeline(const Settings& a, const Settings& b) {
    return a.clip_seconds != b.clip_seconds || a.output_dir != b.output_dir || a.filename != b.filename ||
           a.monitor != b.monitor || a.fps != b.fps || a.video_bitrate_kbps != b.video_bitrate_kbps ||
           a.encoder != b.encoder || a.audio_enabled != b.audio_enabled || a.audio_bitrate_kbps != b.audio_bitrate_kbps ||
           a.mic_enabled != b.mic_enabled || a.mic_device != b.mic_device || a.resolution != b.resolution ||
           a.quality != b.quality || a.capture_cursor != b.capture_cursor || a.system_device != b.system_device ||
           a.separate_tracks != b.separate_tracks;
}

std::wstring exe_path() {
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size()) {
            buffer.resize(length);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::int64_t folder_bytes(const fs::path& folder) {
    std::int64_t total = 0;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code size_ec;
        const auto size = it->file_size(size_ec);
        if (!size_ec) total += static_cast<std::int64_t>(size);
    }
    return total;
}

// The Hotkeys page rows that are settings, in the order they are shown.
constexpr HotkeyTarget kHotkeyRows[] = {HotkeyTarget::Save, HotkeyTarget::MicMute, HotkeyTarget::PushToTalk,
                                         HotkeyTarget::Overlay, HotkeyTarget::Bookmark};

// "save", "mute", ...: the Hotkeys page's hit names are "hotkey-<this>" and "hotkey-<this>-reset".
const char* hotkey_arg(HotkeyTarget target) {
    switch (target) {
        case HotkeyTarget::MicMute: return "mute";
        case HotkeyTarget::Overlay: return "overlay";
        case HotkeyTarget::Bookmark: return "bookmark";
        case HotkeyTarget::PushToTalk: return "ptt";
        case HotkeyTarget::Save: break;
    }
    return "save";
}

std::optional<HotkeyTarget> hotkey_target_of(std::string_view what, std::string_view suffix) {
    for (const HotkeyTarget t : kHotkeyRows) {
        if (what == std::string("hotkey-") + hotkey_arg(t) + std::string(suffix)) return t;
    }
    return std::nullopt;
}

// "saves clips": completes "That key already ...".
const wchar_t* hotkey_does(HotkeyTarget target) {
    switch (target) {
        case HotkeyTarget::MicMute: return L"mutes the mic";
        case HotkeyTarget::Overlay: return L"shows the overlay";
        case HotkeyTarget::Bookmark: return L"adds bookmarks";
        case HotkeyTarget::PushToTalk: return L"is the push-to-talk key";
        case HotkeyTarget::Save: break;
    }
    return L"saves clips";
}

std::wstring lower(std::wstring text) {
    if (!text.empty()) {
        CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    }
    return text;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Changing settings

void App::change_settings(const std::function<void(Settings&)>& edit) {
    Settings next = settings_;
    edit(next);
    if (next == settings_) return;
    const Settings before = settings_;
    settings_ = next;
    write_ini();
    inbox_.set_sound(settings_.sound);
    if (before.log_level != settings_.log_level) log::set_level(settings_.log_level);
    if (before.save_hotkey != settings_.save_hotkey || before.mic_mute_hotkey != settings_.mic_mute_hotkey ||
        before.overlay_hotkey != settings_.overlay_hotkey || before.bookmark_hotkey != settings_.bookmark_hotkey) {
        apply_hotkey();
    }
    if (before.system_volume != settings_.system_volume || before.mic_volume != settings_.mic_volume ||
        before.noise_suppression != settings_.noise_suppression || before.push_to_talk != settings_.push_to_talk ||
        before.push_to_talk_key != settings_.push_to_talk_key) {
        apply_audio_live();
    }
    if (before.start_minimized != settings_.start_minimized && launch_at_startup()) set_launch_at_startup(true);
    if (!settings_.saved_bar) saved_bar_.reset();
    if (before.theme != settings_.theme) apply_look();
    if (before.auto_update != settings_.auto_update && updater_ != nullptr) updater_->set_automatic(settings_.auto_update);
    if (before.overlay != settings_.overlay || before.overlay_corner != settings_.overlay_corner ||
        before.overlay_opacity != settings_.overlay_opacity || before.reduce_motion != settings_.reduce_motion ||
        before.theme != settings_.theme || before.monitor != settings_.monitor) {
        refresh_overlay();
    }
    if (needs_new_pipeline(before, settings_)) {
        // Waits a moment so several quick changes restart Instant Replay once.
        restart_due_ = std::chrono::steady_clock::now() + kRestartDelay;
    }
    if (before.max_gb != settings_.max_gb || before.delete_after_days != settings_.delete_after_days) {
        next_cleanup_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);  // not while dragging
    }
    refresh_status();
    invalidate();
}

void App::write_ini() {
    // Keeps the user's comments and unknown keys: only the values change (core/Config update_ini).
    std::error_code ec;
    std::string existing;
    if (fs::exists(paths_.ini, ec)) {
        const auto text = read_ini_file(paths_.ini);
        if (!text) {
            // Exists but cannot be read (locked): writing would throw away whatever is in it.
            ini_problem_ = "backtrack.ini is locked by another program; changes apply now but are not saved";
            log::warn("{}", ini_problem_);
            return;
        }
        existing = *text;
    }
    if (write_ini_file(paths_.ini, update_ini(existing, settings_))) {
        ini_problem_.clear();
    } else {
        ini_problem_ = "Could not save backtrack.ini (folder not writable?); changes apply until Backtrack closes";
    }
}

void App::restart_recorder() {
    restart_due_.reset();
    if (finished_) return;  // shutting down: the recorder stays stopped
    const bool was_paused = recorder_->paused();
    const bool was_muted = recorder_->mic_muted();
    log::info("Restarting Instant Replay with the new settings");
    const HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    recorder_->stop();  // §6: a clip being written is finished first
    take_saved();
    bookmarks_.clear();  // the new recorder starts with an empty buffer, on a clock of its own
    // The old recorder (stopped) stays until a new one runs, so there is never no recorder.
    // Tried in order: the new settings; the previous ones in the new clips folder (the clips may
    // just have been moved there); the previous ones exactly.
    Settings keep_folder = recorder_settings_;
    keep_folder.output_dir = settings_.output_dir;
    const Settings attempts[] = {settings_, keep_folder, recorder_settings_};
    bool started = false;
    for (std::size_t i = 0; i < std::size(attempts) && !started; ++i) {
        try {
            std::unique_ptr<Recorder> fresh = make_recorder_(attempts[i]);
            fresh->start();
            recorder_ = std::move(fresh);  // the stopped one goes now
            recorder_settings_ = attempts[i];
            started = true;
        } catch (const std::exception& e) {
            log::error("Instant Replay could not start ({}): {}", i == 0 ? "new settings" : "previous settings", e.what());
        }
    }
    if (!started) {
        show_notice(L"Instant Replay could not start again. Restart Backtrack; backtrack.log says why.");
    } else if (!(recorder_settings_ == settings_)) {
        // What is recording now is what the pages show and what the next start uses.
        show_notice(L"Instant Replay could not start with the new settings; it runs with the previous ones.");
        copy_pipeline(recorder_settings_, settings_);
        write_ini();
    }
    if (was_paused) recorder_->pause();
    recorder_->set_mic_muted(was_muted);  // not a setting: a mute lasts until unmuted
    apply_audio_live();  // a fallback recorder was made with older settings
    SetCursor(previous);
    // The clips folder may be another one now.
    probing_.clear();
    urgent_.clear();
    thumbs_missing_.clear();
    scan_folder();
    request_missing_info();
    refresh_status();
    invalidate();
}

void App::apply_hotkey() {
    unregister_hotkeys();
    register_hotkey();
    invalidate();
}

bool App::launch_at_startup() const {
    DWORD size = 0;
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, &size) == ERROR_SUCCESS;
}

void App::set_launch_at_startup(bool on) {
    LSTATUS status = ERROR_SUCCESS;
    if (on) {
        // Quoted: the path may have spaces. --hidden: straight to the tray when that is wanted.
        const std::wstring command = L"\"" + exe_path() + L"\"" + (settings_.start_minimized ? L" --hidden" : L"");
        status = RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, command.c_str(),
                                 static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    if (status != ERROR_SUCCESS) {
        log::warn("Could not change starting with Windows: {}", hresult_text(HRESULT_FROM_WIN32(status)));
        show_notice(L"Could not change starting with Windows (blocked by a policy or security program?)");
    }
    invalidate();
}

CleanupPolicy App::cleanup_policy() const {
    CleanupPolicy policy;
    policy.delete_after_days = settings_.delete_after_days;
    policy.max_bytes = static_cast<std::int64_t>(settings_.max_gb) * 1'000'000'000LL;
    return policy;
}

void App::change_clips_folder() {
    // Also when the configured folder is gone for good (unplugged drive): this is the way out.
    if (shell_busy_ || finished_) return;
    if (trimming_ || recycling_ || repairing()) {
        show_notice(L"Wait a moment until the clip being saved, removed or repaired is done, then change the folder.");
        return;
    }
    const bool have_folder = !recorder_->output_dir_is_fallback();
    const fs::path current = recorder_->output_dir();
    shell_busy_ = true;  // the picker pumps messages: a second click must not open a second one
    const std::optional<fs::path> chosen = shell::pick_folder(window_, current, L"Choose where Backtrack saves clips");
    shell_busy_ = false;
    if (quit_pending_) {
        quit();
        return;
    }
    if (!chosen || window_ == nullptr) return;
    std::error_code ec;
    if (have_folder && fs::equivalent(*chosen, current, ec)) return;
    {
        // Can clips be written there at all?
        const fs::path probe = *chosen / L"backtrack.write-test";
        const HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            show_notice(L"Backtrack cannot save clips in that folder. Pick a folder you can write to.");
            return;
        }
        CloseHandle(file);
    }
    bool move = false;
    if (have_folder && !library_.clips().empty()) {
        // Move them along, or start the new folder empty? A real question: clips can be many GB.
        const std::size_t count = library_.clips().size();
        const std::wstring move_text = std::format(L"Move my {} {} there\nFavorites, collections, tags and notes come along.",
                                                   count, count == 1 ? L"clip" : L"clips");
        const TASKDIALOG_BUTTON buttons[] = {
            {1001, move_text.c_str()},
            {1002, L"Leave them where they are\nThe library shows the clips in the new folder only."},
        };
        TASKDIALOGCONFIG config{};
        config.cbSize = sizeof(config);
        config.hwndParent = window_;
        config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        config.pszWindowTitle = L"Backtrack";
        config.pszMainInstruction = L"What about the clips you already have?";
        const std::wstring content = L"New clips will be saved in " + chosen->wstring() + L".";
        config.pszContent = content.c_str();
        config.cButtons = static_cast<UINT>(std::size(buttons));
        config.pButtons = buttons;
        config.nDefaultButton = 1001;
        int pressed = IDCANCEL;
        shell_busy_ = true;  // the dialog pumps messages too
        const HRESULT shown = TaskDialogIndirect(&config, &pressed, nullptr, nullptr);
        shell_busy_ = false;
        if (quit_pending_) {
            quit();
            return;
        }
        if (FAILED(shown) || pressed == IDCANCEL || window_ == nullptr || trimming_) return;
        move = pressed == 1001;
    }
    close_clip();  // the player holds a clip open
    if (page_ == Page::Clip) set_page(Page::Library);
    // Whatever happens next, the library as it is now is kept.
    keep_copy_of_library(L"before-folder-change");
    // Instant Replay stops first, so no clip lands in the old folder while the others move; the
    // clip it may be writing is finished and filed, then moved with the rest.
    recorder_->stop();
    try {
        take_saved();
        if (move) {
            std::vector<fs::path> files;
            for (const ClipEntry& clip : library_.clips()) files.push_back(clip_path(clip.file));
            // No folder scan while files leave the old folder (it would drop their favorites and tags),
            // and no clicks on the window behind Windows' progress window.
            shell_busy_ = true;
            EnableWindow(window_, FALSE);
            const std::vector<fs::path> left = shell::move_files(files, *chosen, window_);
            EnableWindow(window_, TRUE);
            shell_busy_ = false;
            if (!left.empty()) {
                show_notice(std::format(L"{} {} could not be moved and stayed in the old folder.", left.size(),
                                        left.size() == 1 ? L"clip" : L"clips"));
            }
        }
        saved_bar_.reset();
        new_files_.clear();
        change_settings([&](Settings& s) { s.output_dir = to_utf8(chosen->wstring()); });
    } catch (...) {
        EnableWindow(window_, TRUE);
        shell_busy_ = false;
        restart_recorder();  // never left stopped
        if (quit_pending_) quit();
        throw;
    }
    restart_recorder();  // now, so the library shows the new folder at once
    if (quit_pending_) {
        quit();
        return;
    }
    if (repair_) repair_->cancel->store(true);
    repair_.reset();  // a check of the old folder says nothing about the new one
    scan_folder();
    start_repair_scan(true);  // clips from another program in it: say they can be repaired
}

void App::cancel_hotkey_capture() {
    if (!capturing_hotkey_) return;
    capturing_hotkey_ = false;
    hotkey_message_.clear();
    apply_hotkey();  // the old key works again
}

void App::capture_hotkey(WPARAM key) {
    if (key == VK_ESCAPE) {
        capturing_hotkey_ = false;
        hotkey_message_.clear();
        apply_hotkey();
        return;
    }
    if (key == VK_BACK && capture_target_ != HotkeyTarget::Save && capture_target_ != HotkeyTarget::PushToTalk) {
        // Backspace: no key for this one at all (saving always has one).
        capturing_hotkey_ = false;
        hotkey_message_.clear();
        const HotkeyTarget target = capture_target_;
        if (hotkey_text(settings_, target).empty()) apply_hotkey();
        else change_settings([target](Settings& s) { hotkey_text(s, target).clear(); });
        return;
    }
    if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU || key == VK_LWIN || key == VK_RWIN || key == VK_LSHIFT ||
        key == VK_RSHIFT || key == VK_LCONTROL || key == VK_RCONTROL || key == VK_LMENU || key == VK_RMENU) {
        return;  // waiting for the key that goes with them
    }
    Hotkey hotkey;
    hotkey.virtual_key = static_cast<std::uint32_t>(key);
    if (GetKeyState(VK_CONTROL) < 0) hotkey.modifiers |= hotkey_mod::kCtrl;
    if (GetKeyState(VK_MENU) < 0) hotkey.modifiers |= hotkey_mod::kAlt;
    if (GetKeyState(VK_SHIFT) < 0) hotkey.modifiers |= hotkey_mod::kShift;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) hotkey.modifiers |= hotkey_mod::kWin;
    const std::string text = hotkey_to_string(hotkey);
    const HotkeyTarget target = capture_target_;
    const HotkeyParseResult parsed =
        parse_hotkey(text, target == HotkeyTarget::PushToTalk ? HotkeyUse::Held : HotkeyUse::Global);
    message_target_ = target;
    if (!parsed.hotkey) {
        hotkey_message_ = parsed.error.empty() ? L"That key cannot be used for a hotkey." : to_wide(parsed.error);
        invalidate();
        return;  // still listening: try another one
    }
    if (target != HotkeyTarget::PushToTalk) {
        for (int i = 0; i < kGlobalHotkeys; ++i) {
            const auto other = static_cast<HotkeyTarget>(i);
            if (other != target && text == hotkey_text(settings_, other)) {
                hotkey_message_ = std::format(L"That key already {}. Pick another one.", hotkey_does(other));
                invalidate();
                return;
            }
        }
    }
    capturing_hotkey_ = false;
    hotkey_message_ = to_wide(parsed.warning);
    if (text == hotkey_text(settings_, target)) {
        apply_hotkey();
    } else {
        change_settings([&](Settings& s) { hotkey_text(s, target) = text; });
        apply_hotkey();  // the push-to-talk key does not register, but the others were let go while listening
    }
}

// ---------------------------------------------------------------------------------------------
// The rows

std::vector<App::SettingRow> App::setting_rows() {
    using Kind = SettingRow::Kind;
    const Settings defaults{};
    std::vector<SettingRow> rows;
    const auto add = [&rows](SettingRow row) -> SettingRow& {
        rows.push_back(std::move(row));
        return rows.back();
    };
    const auto toggle_row = [&](const char* section, const char* id, const wchar_t* label, const wchar_t* description,
                                bool on, bool def, std::function<void(Settings&, bool)> set) -> SettingRow& {
        SettingRow row;
        row.id = id;
        row.section = section;
        row.kind = Kind::Toggle;
        row.label = label;
        row.description = description;
        row.on = on;
        row.changed = on != def;
        row.choose = [this, set](int choice) { change_settings([&](Settings& s) { set(s, choice != 0); }); };
        row.reset = [set, def](Settings& s) { set(s, def); };
        return add(std::move(row));
    };
    // A segmented control over an enum stored in Settings (options in the enum's order).
    const auto enum_row = [&]<typename Enum>(const char* section, const char* id, const wchar_t* label,
                                             const wchar_t* description, std::vector<std::wstring> options,
                                             Enum Settings::*field) -> SettingRow& {
        SettingRow row;
        row.id = id;
        row.section = section;
        row.kind = Kind::Segmented;
        row.label = label;
        row.description = description;
        row.options = std::move(options);
        row.selected = static_cast<int>(settings_.*field);
        row.changed = settings_.*field != defaults.*field;
        const int count = static_cast<int>(row.options.size());
        row.choose = [this, field, count](int choice) {
            if (choice >= 0 && choice < count) change_settings([&](Settings& s) { s.*field = static_cast<Enum>(choice); });
        };
        row.reset = [defaults, field](Settings& s) { s.*field = defaults.*field; };
        return add(std::move(row));
    };
    const auto soon_row = [&](const char* section, const char* id, Kind kind, const wchar_t* label,
                              const wchar_t* description, std::vector<std::wstring> options = {}, int selected = 0,
                              bool on = false, float value = 0, std::wstring value_text = {}) -> SettingRow& {
        SettingRow row;
        row.id = id;
        row.section = section;
        row.kind = kind;
        row.label = label;
        row.description = description;
        row.soon = true;
        row.options = std::move(options);
        row.selected = selected;
        row.on = on;
        row.value = value;
        row.value_text = std::move(value_text);
        return add(std::move(row));
    };

    // General.
    {
        SettingRow row;
        row.id = "startup";
        row.section = "general";
        row.kind = Kind::Toggle;
        row.label = L"Launch when Windows starts";
        row.description = L"Backtrack is ready before you open anything else.";
        row.keywords = L"startup boot login autostart";
        row.on = launch_at_startup();
        row.changed = row.on;
        row.choose = [this](int choice) { set_launch_at_startup(choice != 0); };
        row.reset = [](Settings&) {};  // not in backtrack.ini: run_setting turns it off itself
        add(std::move(row));
    }
    toggle_row("general", "tray", L"Start minimized to the tray",
               L"When Windows starts it, Backtrack opens quietly in the background. Instant Replay still runs.",
               settings_.start_minimized, defaults.start_minimized,
               [](Settings& s, bool v) { s.start_minimized = v; });
    soon_row("general", "lang", Kind::Select, L"Language", L"More languages are coming.", {L"English"});
    toggle_row("general", "autoUpdate", L"Install updates automatically",
               L"Downloads new versions in the background and installs them when you quit Backtrack.",
               settings_.auto_update, defaults.auto_update, [](Settings& s, bool v) { s.auto_update = v; })
        .keywords = L"update automatic";
    {
        SettingRow row;
        row.id = "update";
        row.section = "general";
        row.kind = Kind::Custom;
        row.label = L"Check for updates";
        // What the updater is doing, and the one button that fits.
        const UpdateState update = updater_ != nullptr ? updater_->state() : UpdateState{};
        std::wstring what = std::format(L"You are on version {}.", to_wide(BT_VERSION));
        std::wstring label = L"Check for updates";
        std::string arg = "update";
        bool disabled = updater_ == nullptr;
        switch (update.stage) {
            case UpdateStage::Idle: break;
            case UpdateStage::Checking:
                what = L"Checking for a new version...";
                disabled = true;
                break;
            case UpdateStage::UpToDate: what += L" That is the newest."; break;
            case UpdateStage::Available:
                what = std::format(L"Version {} is out (you have {}).", to_wide(update.version), to_wide(BT_VERSION));
                if (update.can_install) {
                    label = L"Download";
                    arg = "update-download";
                } else {
                    label = L"Open the download page";  // not an installed copy: it cannot replace itself
                    arg = "update-page";
                }
                break;
            case UpdateStage::Downloading:
                what = std::format(L"Downloading version {}: {}%", to_wide(update.version),
                                   std::lround(std::clamp(update.fraction, 0.0, 1.0) * 100));
                disabled = true;
                break;
            case UpdateStage::Ready:
                // Installing on quit is part of automatic updates; without them only the button installs.
                what = settings_.auto_update
                           ? std::format(L"Version {} is ready. It installs when you quit Backtrack.", to_wide(update.version))
                           : std::format(L"Version {} is ready. Press Install now to update.", to_wide(update.version));
                label = L"Install now";
                arg = "update-install";
                break;
            case UpdateStage::Failed: what = to_wide(update.error); break;
        }
        row.description = what;
        row.keywords = L"update version";
        row.height = 30;
        row.paint = [this, label, arg, disabled](const Rect& area) {
            const float w = button_width(label, glyph::kRestoreDefaults, true);
            push_button(area.right() - w, area.y + (area.h - 30) / 2, label, glyph::kRestoreDefaults,
                        arg == "update-install" ? ButtonStyle::Primary : ButtonStyle::Line, true, Action::Setting, arg,
                        disabled);
        };
        add(std::move(row));
    }

    // Capture & Video.
    {
        SettingRow row;
        row.id = "display";
        row.section = "capture";
        row.kind = Kind::Select;
        row.label = L"Display to capture";
        row.description = L"Choose the display Instant Replay records.";
        row.keywords = L"monitor screen";
        for (std::size_t i = 0; i < monitors_.size(); ++i) {
            const MonitorInfo& m = monitors_[i];
            row.options.push_back(std::format(L"Display {}{}, {}×{}", i + 1, m.primary ? L" (main)" : L"",
                                              m.desktop_size.width, m.desktop_size.height));
        }
        if (row.options.empty()) row.options.push_back(L"Display 1");
        // Out of range (unplugged) records monitor 0, like the recorder does.
        row.selected = settings_.monitor >= 0 && settings_.monitor < static_cast<int>(row.options.size()) ? settings_.monitor : 0;
        row.changed = settings_.monitor != defaults.monitor;
        row.choose = [this](int choice) { change_settings([choice](Settings& s) { s.monitor = choice; }); };
        row.reset = [defaults](Settings& s) { s.monitor = defaults.monitor; };
        add(std::move(row));
    }
    {
        const MonitorInfo* m = settings_.monitor >= 0 && static_cast<std::size_t>(settings_.monitor) < monitors_.size()
                                   ? &monitors_[static_cast<std::size_t>(settings_.monitor)]
                                   : nullptr;
        // Native, then only the sizes below the display's own: the video is never scaled up.
        static constexpr int kHeights[] = {1440, 1080, 720};
        SettingRow row;
        row.id = "res";
        row.section = "capture";
        row.kind = Kind::Select;
        row.label = L"Resolution";
        row.description = L"Lower resolutions give smaller files.";
        row.keywords = L"resolution 1080p 720p 1440p size scale";
        row.options.push_back(m != nullptr ? std::format(L"Native ({}×{})", m->desktop_size.width, m->desktop_size.height)
                                           : std::wstring(L"Native"));
        std::vector<int> heights{0};
        for (const int h : kHeights) {
            if (m == nullptr || h < m->desktop_size.height) {
                row.options.push_back(std::format(L"{}p", h));
                heights.push_back(h);
            }
        }
        const auto at = std::find(heights.begin(), heights.end(), settings_.resolution);
        if (at != heights.end()) {
            row.selected = static_cast<int>(at - heights.begin());
        } else {
            // Set for a larger display than this one: recorded at native size here.
            row.options.push_back(std::format(L"{}p (native on this display)", settings_.resolution));
            heights.push_back(settings_.resolution);
            row.selected = static_cast<int>(heights.size()) - 1;
        }
        row.changed = settings_.resolution != defaults.resolution;
        row.choose = [this, heights](int choice) {
            if (choice >= 0 && static_cast<std::size_t>(choice) < heights.size()) {
                change_settings([&](Settings& s) { s.resolution = heights[static_cast<std::size_t>(choice)]; });
            }
        };
        row.reset = [defaults](Settings& s) { s.resolution = defaults.resolution; };
        add(std::move(row));
    }
    {
        static constexpr int kFps[] = {30, 60, 120, 144};
        SettingRow row;
        row.id = "fps";
        row.section = "capture";
        row.kind = Kind::Segmented;
        row.label = L"Frame rate";
        row.keywords = L"fps frames smooth";
        row.selected = -1;
        for (const int f : kFps) {
            row.options.push_back(std::to_wstring(f));
            if (f == settings_.fps) row.selected = static_cast<int>(row.options.size()) - 1;
        }
        if (row.selected < 0) {
            row.options.push_back(std::to_wstring(settings_.fps));  // typed into backtrack.ini
            row.selected = static_cast<int>(row.options.size()) - 1;
        }
        row.changed = settings_.fps != defaults.fps;
        row.choose = [this](int choice) {
            if (choice < static_cast<int>(std::size(kFps))) change_settings([choice](Settings& s) { s.fps = kFps[choice]; });
        };
        row.reset = [defaults](Settings& s) { s.fps = defaults.fps; };
        add(std::move(row));
    }
    {
        SettingRow row;
        row.id = "encoder";
        row.section = "capture";
        row.kind = Kind::Select;
        row.label = L"Encoder";
        const BufferStatus status = recorder_->buffer_status();
        row.description = status.encoder_running
                              ? (status.hardware_encoder ? L"Now using your graphics card, which keeps your CPU free."
                                                         : L"Now using the processor (CPU).")
                              : L"Hardware encoders keep your CPU free.";
        row.keywords = L"gpu nvenc amf quicksync hardware software cpu";
        row.options = {L"Automatic (graphics card)", L"Graphics card only", L"Processor (software)"};
        row.selected = static_cast<int>(settings_.encoder);
        row.changed = settings_.encoder != defaults.encoder;
        row.choose = [this](int choice) {
            change_settings([choice](Settings& s) { s.encoder = static_cast<EncoderPreference>(choice); });
        };
        row.reset = [defaults](Settings& s) { s.encoder = defaults.encoder; };
        add(std::move(row));
    }
    {
        // 1..100 Mbps in whole Mbps.
        SettingRow row;
        row.id = "bitrate";
        row.section = "capture";
        row.kind = Kind::Slider;
        row.label = L"Bitrate";
        row.description = L"Higher looks better and makes bigger files.";
        row.keywords = L"quality mbps size";
        const auto mbps = [](float f) { return 1 + static_cast<int>(std::lround(f * 99.0f)); };
        row.value = static_cast<float>(settings_.video_bitrate_kbps / 1000 - 1) / 99.0f;
        row.value_text = std::format(L"{} Mbps", std::lround(settings_.video_bitrate_kbps / 1000.0));
        row.preview = [mbps](float f) { return std::format(L"{} Mbps", mbps(f)); };
        row.changed = settings_.video_bitrate_kbps != defaults.video_bitrate_kbps;
        row.drag = [this, mbps](float f, bool done) {
            if (done) change_settings([&](Settings& s) { s.video_bitrate_kbps = mbps(f) * 1000; });
        };
        row.reset = [defaults](Settings& s) { s.video_bitrate_kbps = defaults.video_bitrate_kbps; };
        add(std::move(row));
    }
    enum_row("capture", "preset", L"Quality preset", L"How hard the encoder works for the same bitrate.",
             {L"Performance", L"Balanced", L"Quality"}, &Settings::quality)
        .keywords = L"quality speed encoder preset";
    toggle_row("capture", "cursor", L"Capture mouse cursor", L"Shows the pointer in your clips. Games that hide it stay clean.",
               settings_.capture_cursor, defaults.capture_cursor, [](Settings& s, bool v) { s.capture_cursor = v; })
        .keywords = L"mouse pointer cursor";
    soon_row("capture", "hdr", Kind::Toggle, L"HDR capture",
             L"Records in HDR when your display supports it. Files get larger.");

    // Instant Replay.
    {
        SettingRow row;
        row.id = "replayOn";
        row.section = "replay";
        row.kind = Kind::Toggle;
        row.label = L"Instant Replay";
        row.description = L"Keeps the last moments ready to save. Off pauses it until you turn it on again.";
        row.keywords = L"pause resume buffer";
        row.on = !recorder_->paused();
        row.choose = [this](int choice) {
            if ((choice != 0) == recorder_->paused()) toggle_pause();
        };
        add(std::move(row));
    }
    {
        // 5 s to 5 min in steps of 5 s; a saved clip is this long.
        SettingRow row;
        row.id = "replayLen";
        row.section = "replay";
        row.kind = Kind::Slider;
        row.label = L"Buffer length";
        row.description = L"Between 5 seconds and 5 minutes: how far back you can save.";
        row.keywords = L"seconds minutes clip length duration";
        const auto seconds = [](float f) { return 5 + 5 * static_cast<int>(std::lround(f * 59.0f)); };
        row.value = static_cast<float>(settings_.clip_seconds - 5) / 295.0f;
        row.value_text = length_text(settings_.clip_seconds);
        row.preview = [seconds](float f) { return length_text(seconds(f)); };
        row.changed = settings_.clip_seconds != defaults.clip_seconds;
        row.drag = [this, seconds](float f, bool done) {
            if (done) change_settings([&](Settings& s) { s.clip_seconds = seconds(f); });
        };
        row.reset = [defaults](Settings& s) { s.clip_seconds = defaults.clip_seconds; };
        add(std::move(row));
    }
    soon_row("replay", "replayStore", Kind::Segmented, L"Keep the buffer in",
             L"RAM is fastest. Disk uses less memory but writes constantly.", {L"RAM", L"Disk"});
    {
        // The whole buffer, or a shorter stretch of it; lengths that do not fit in the buffer are left out.
        static constexpr int kLengths[] = {15, 30, 60, 120};
        SettingRow row;
        row.id = "saveLen";
        row.section = "replay";
        row.kind = Kind::Select;
        row.label = L"Length of a saved clip";
        row.description = L"What the save hotkey keeps. Changes at once, the buffer is kept.";
        row.keywords = L"save length seconds clip duration";
        row.options.push_back(L"The whole buffer (" + length_text(settings_.clip_seconds) + L")");
        std::vector<int> lengths{0};
        for (const int l : kLengths) {
            if (l < settings_.clip_seconds) {
                row.options.push_back(L"Last " + length_text(l));
                lengths.push_back(l);
            }
        }
        const int effective = saved_seconds(settings_) == settings_.clip_seconds ? 0 : settings_.save_seconds;
        const auto at = std::find(lengths.begin(), lengths.end(), effective);
        if (at != lengths.end()) {
            row.selected = static_cast<int>(at - lengths.begin());
        } else {
            row.options.push_back(L"Last " + length_text(effective));  // typed into backtrack.ini
            lengths.push_back(effective);
            row.selected = static_cast<int>(lengths.size()) - 1;
        }
        row.changed = saved_seconds(settings_) != settings_.clip_seconds;  // anything but the whole buffer
        row.choose = [this, lengths](int choice) {
            if (choice >= 0 && static_cast<std::size_t>(choice) < lengths.size()) {
                change_settings([&](Settings& s) { s.save_seconds = lengths[static_cast<std::size_t>(choice)]; });
            }
        };
        row.reset = [defaults](Settings& s) { s.save_seconds = defaults.save_seconds; };
        add(std::move(row));
    }
    {
        SettingRow row;
        row.id = "ram";
        row.section = "replay";
        row.kind = Kind::Custom;
        row.label = L"Memory use";
        row.description = L"Estimated from your bitrate and buffer length.";
        row.keywords = L"ram memory estimate";
        row.height = 22;
        row.paint = [this](const Rect& area) {
            const bool sound = settings_.audio_enabled || settings_.mic_enabled;
            const double mb = (settings_.video_bitrate_kbps + (sound ? settings_.audio_bitrate_kbps : 0)) *
                              static_cast<double>(settings_.clip_seconds) / 8.0 / 1000.0;
            graphics_->text(std::format(L"about {} MB", std::lround(mb)), area, TextStyle{14, Weight::Bold, false, true},
                            kInk, Align::End);
        };
        add(std::move(row));
    }

    // Audio.
    {
        // The Windows default first (followed when it changes), then every playback device by name.
        SettingRow row;
        row.id = "outDev";
        row.section = "audio";
        row.kind = Kind::Select;
        row.label = L"System audio source";
        const AudioLevels levels = recorder_->audio_levels();
        row.description = levels.system_fallback
                              ? L"The chosen device is not connected: recording the Windows default until it is back."
                              : L"What you hear on this device is recorded.";
        row.keywords = L"speakers headphones output playback device sound";
        std::wstring default_name;
        for (const PlaybackDevice& d : playback_devices_) {
            if (d.is_default) default_name = to_wide(d.name);
        }
        row.options.push_back(default_name.empty() ? std::wstring(L"Windows default")
                                                   : L"Windows default (" + default_name + L")");
        std::vector<std::string> ids{std::string()};
        for (const PlaybackDevice& d : playback_devices_) {
            row.options.push_back(to_wide(d.name));
            ids.push_back(to_utf8(d.id));
            if (to_utf8(d.id) == settings_.system_device) row.selected = static_cast<int>(row.options.size()) - 1;
        }
        if (!settings_.system_device.empty() && row.selected == 0) {
            // Unplugged: it is used again when it comes back.
            row.options.push_back(L"Unplugged device");
            ids.push_back(settings_.system_device);
            row.selected = static_cast<int>(row.options.size()) - 1;
        }
        row.changed = !settings_.system_device.empty();
        row.choose = [this, ids](int choice) {
            if (choice >= 0 && static_cast<std::size_t>(choice) < ids.size()) {
                change_settings([&](Settings& s) { s.system_device = ids[static_cast<std::size_t>(choice)]; });
            }
        };
        row.reset = [](Settings& s) { s.system_device.clear(); };
        add(std::move(row));
    }
    {
        // The Windows default first (followed when it changes), then every microphone by name.
        SettingRow row;
        row.id = "micDev";
        row.section = "audio";
        row.kind = Kind::Select;
        row.label = L"Microphone";
        row.description = L"Bluetooth headset mics are only used when picked here.";
        row.keywords = L"mic microphone input device headset";
        std::wstring default_name;
        bool default_bluetooth = false;
        for (const MicDevice& m : microphones_) {
            if (m.is_default) {
                default_name = to_wide(m.name);
                default_bluetooth = m.bluetooth;
            }
        }
        row.options.push_back(default_name.empty() ? std::wstring(L"Windows default")
                              : default_bluetooth ? std::wstring(L"Windows default (skips Bluetooth headsets)")
                                                  : L"Windows default (" + default_name + L")");
        std::vector<std::string> ids{std::string()};
        for (const MicDevice& m : microphones_) {
            row.options.push_back(to_wide(m.name));
            ids.push_back(to_utf8(m.id));
            if (to_utf8(m.id) == settings_.mic_device) row.selected = static_cast<int>(row.options.size()) - 1;
        }
        if (!settings_.mic_device.empty() && row.selected == 0) {
            // The chosen one is unplugged: it is used again when it comes back.
            row.options.push_back(L"Unplugged microphone");
            ids.push_back(settings_.mic_device);
            row.selected = static_cast<int>(row.options.size()) - 1;
        }
        row.changed = !settings_.mic_device.empty();
        row.choose = [this, ids](int choice) {
            if (choice >= 0 && static_cast<std::size_t>(choice) < ids.size()) {
                change_settings([&](Settings& s) { s.mic_device = ids[static_cast<std::size_t>(choice)]; });
            }
        };
        row.reset = [](Settings& s) { s.mic_device.clear(); };
        add(std::move(row));
    }
    toggle_row("audio", "micOn", L"Record microphone", L"Your voice, mixed into the clip with the game sound.",
               settings_.mic_enabled, defaults.mic_enabled, [](Settings& s, bool v) { s.mic_enabled = v; })
        .keywords = L"mic microphone voice";
    toggle_row("audio", "sysOn", L"Record system audio", L"What you hear: games, voice chat, music.",
               settings_.audio_enabled, defaults.audio_enabled, [](Settings& s, bool v) { s.audio_enabled = v; })
        .keywords = L"sound game";
    {
        static constexpr int kRates[] = {96, 128, 160, 192};
        SettingRow row;
        row.id = "audioQuality";
        row.section = "audio";
        row.kind = Kind::Segmented;
        row.label = L"Sound quality";
        row.description = L"In kbit/s. 160 sounds the same as 192 for almost everything.";
        row.keywords = L"bitrate kbps aac";
        row.selected = -1;
        for (const int r : kRates) {
            row.options.push_back(std::to_wstring(r));
            if (r == settings_.audio_bitrate_kbps) row.selected = static_cast<int>(row.options.size()) - 1;
        }
        if (row.selected < 0) {
            row.options.push_back(std::to_wstring(settings_.audio_bitrate_kbps));
            row.selected = static_cast<int>(row.options.size()) - 1;
        }
        row.changed = settings_.audio_bitrate_kbps != defaults.audio_bitrate_kbps;
        row.choose = [this](int choice) {
            if (choice < static_cast<int>(std::size(kRates))) {
                change_settings([choice](Settings& s) { s.audio_bitrate_kbps = kRates[choice]; });
            }
        };
        row.reset = [defaults](Settings& s) { s.audio_bitrate_kbps = defaults.audio_bitrate_kbps; };
        add(std::move(row));
    }
    // Volumes: 0 to 200 % in steps of 5, applied at once (no restart).
    const auto volume_row = [&](const char* id, const wchar_t* label, const wchar_t* description, int Settings::*field) {
        SettingRow row;
        row.id = id;
        row.section = "audio";
        row.kind = Kind::Slider;
        row.label = label;
        row.description = description;
        row.keywords = L"volume loudness level gain";
        const auto percent = [](float f) { return 5 * static_cast<int>(std::lround(f * 40.0f)); };
        row.value = static_cast<float>(settings_.*field) / 200.0f;
        row.value_text = std::format(L"{}%", settings_.*field);
        row.preview = [percent](float f) { return std::format(L"{}%", percent(f)); };
        row.changed = settings_.*field != defaults.*field;
        row.drag = [this, percent, field](float f, bool done) {
            if (done) change_settings([&](Settings& s) { s.*field = percent(f); });
        };
        row.reset = [defaults, field](Settings& s) { s.*field = defaults.*field; };
        add(std::move(row));
    };
    volume_row("micVol", L"Microphone volume", L"How loud your voice is in the clip.", &Settings::mic_volume);
    volume_row("sysVol", L"System volume", L"How loud the game and everything else you hear is in the clip.",
               &Settings::system_volume);
    toggle_row("audio", "tracks", L"Separate audio tracks",
               L"Also keeps mic and game sound on tracks of their own, for editing. Players and Discord play the mix.",
               settings_.separate_tracks, defaults.separate_tracks, [](Settings& s, bool v) { s.separate_tracks = v; })
        .keywords = L"tracks separate edit mic game";
    toggle_row("audio", "noise", L"Noise suppression", L"Takes out fan noise and hum, and softens keyboard clicks on your mic.",
               settings_.noise_suppression, defaults.noise_suppression,
               [](Settings& s, bool v) { s.noise_suppression = v; })
        .keywords = L"noise suppression fan keyboard background";
    toggle_row("audio", "ptt", L"Push-to-talk",
               std::format(L"Records your mic only while you hold {} (change it in Hotkeys).", to_wide(settings_.push_to_talk_key))
                   .c_str(),
               settings_.push_to_talk, defaults.push_to_talk, [](Settings& s, bool v) { s.push_to_talk = v; })
        .keywords = L"push to talk ptt key hold";

    // Hotkeys.
    {
        SettingRow row;
        row.id = "hotkeys";
        row.section = "hotkeys";
        row.kind = Kind::Custom;
        row.full = true;
        row.keywords = L"keyboard shortcut keys save record screenshot";
        row.changed = false;
        for (const HotkeyTarget t : kHotkeyRows) row.changed = row.changed || hotkey_text(settings_, t) != hotkey_text(defaults, t);
        // Each row is 58; a row with a warning under it 26 more (the same rule as hotkey_row).
        int warnings = 0;
        if (!hotkey_message_.empty()) ++warnings;
        if (!capturing_hotkey_) {
            for (int i = 0; i < kGlobalHotkeys; ++i) {
                const auto t = static_cast<HotkeyTarget>(i);
                if (!hotkey_ok_[i] && !hotkey_text(settings_, t).empty() &&
                    !(hotkey_message_.size() && message_target_ == t)) {
                    ++warnings;
                }
            }
        }
        row.height = 7 * 58.0f + 26.0f * static_cast<float>(warnings);
        row.paint = [this](const Rect& area) { paint_hotkeys(area.x, area.y, area.w); };
        row.reset = [defaults](Settings& s) {
            for (const HotkeyTarget t : kHotkeyRows) hotkey_text(s, t) = hotkey_text(defaults, t);
        };
        add(std::move(row));
    }

    // Storage.
    {
        SettingRow row;
        row.id = "folder";
        row.section = "storage";
        row.kind = Kind::Custom;
        row.label = L"Clips folder";
        row.description = L"Clips and trimmed clips are saved here.";
        row.keywords = L"folder path location directory";
        row.height = 36;
        row.paint = [this](const Rect& area) {
            Graphics& g = *graphics_;
            const float open_w = button_width(L"Open", glyph::kFolder, true);
            const float change_w = button_width(L"Change...", 0, true);
            float x = area.right() - open_w;
            push_button(x, area.y + 3, L"Open", glyph::kFolder, ButtonStyle::Ghost, true, Action::Setting, "folder-open");
            x -= 8 + change_w;
            push_button(x, area.y + 3, L"Change...", 0, ButtonStyle::Line, true, Action::Setting, "folder-change");
            // .path: monospace on --bg.
            const std::wstring path = recorder_->output_dir().wstring();
            const float path_w = std::clamp(x - 12 - area.x, 60.0f, 340.0f);
            const Rect box{x - 12 - path_w, area.y, path_w, 36};
            g.fill_round(box, 9, kBg);
            g.stroke_round(box, 9, kLine);
            g.text(path, box.inset(12, 0), TextStyle{12.5f}, recorder_->output_dir_is_fallback() ? kWarn : kInk);
        };
        add(std::move(row));
    }
    {
        // 0 (no limit) to 500 GB in steps of 5.
        SettingRow row;
        row.id = "maxStore";
        row.section = "storage";
        row.kind = Kind::Slider;
        row.label = L"Maximum storage";
        row.description = L"Oldest clips go to the Recycle Bin past this limit. Favorites and collections are kept.";
        row.keywords = L"gb limit space disk";
        const auto gb = [](float f) { return 5 * static_cast<int>(std::lround(f * 100.0f)); };
        const auto text = [](int value) { return value == 0 ? std::wstring(L"No limit") : std::format(L"{} GB", value); };
        row.value = static_cast<float>(std::min(settings_.max_gb, 500)) / 500.0f;
        row.value_text = text(settings_.max_gb);
        row.preview = [gb, text](float f) { return text(gb(f)); };
        row.changed = settings_.max_gb != defaults.max_gb;
        row.drag = [this, gb](float f, bool done) {
            if (done) change_settings([&](Settings& s) { s.max_gb = gb(f); });
        };
        row.reset = [defaults](Settings& s) { s.max_gb = defaults.max_gb; };
        add(std::move(row));
    }
    {
        static constexpr int kDays[] = {0, 7, 14, 30, 60, 90};
        SettingRow row;
        row.id = "autoDel";
        row.section = "storage";
        row.kind = Kind::Select;
        row.label = L"Delete clips after";
        row.description = L"Old clips go to the Recycle Bin. Favorites and clips in collections are kept.";
        row.keywords = L"days old cleanup";
        bool found = false;
        for (const int d : kDays) {
            row.options.push_back(d == 0 ? std::wstring(L"Never") : std::format(L"{} days", d));
            if (d == settings_.delete_after_days) {
                row.selected = static_cast<int>(row.options.size()) - 1;
                found = true;
            }
        }
        if (!found) {
            // A number typed into backtrack.ini that is not in the list stays as it is.
            row.options.push_back(std::format(L"{} days", settings_.delete_after_days));
            row.selected = static_cast<int>(row.options.size()) - 1;
        }
        row.changed = settings_.delete_after_days != defaults.delete_after_days;
        row.choose = [this](int choice) {
            if (choice < static_cast<int>(std::size(kDays))) {
                change_settings([choice](Settings& s) { s.delete_after_days = kDays[choice]; });
            }
        };
        row.reset = [defaults](Settings& s) { s.delete_after_days = defaults.delete_after_days; };
        add(std::move(row));
    }
    {
        SettingRow row;
        row.id = "usage";
        row.section = "storage";
        row.kind = Kind::Custom;
        row.label = L"Usage";
        row.full = true;
        row.keywords = L"usage space";
        row.height = 18 + 10 + 14 + 12 + 18;
        row.paint = [this](const Rect& area) {
            Graphics& g = *graphics_;
            const LibraryCounts counts = library_.counts(now_unix());
            const std::int64_t cache = std::max<std::int64_t>(cache_bytes_, 0);
            const double used_gb = static_cast<double>(counts.bytes + cache) / 1e9;
            // The bar shows the share of the limit; without one, of what is used.
            const double max_gb = settings_.max_gb > 0 ? settings_.max_gb : std::max(1.0, used_gb * 1.25);
            g.text(to_wide(format_size(counts.bytes + cache)) + L" used", Rect{area.x, area.y, area.w / 2, 18},
                   TextStyle{14, Weight::Bold, false, true}, kInk);
            g.text(settings_.max_gb > 0 ? std::format(L"of {} GB", settings_.max_gb) : std::wstring(L"no limit set"),
                   Rect{area.x + area.w / 2, area.y, area.w / 2, 18}, TextStyle{13}, kMute, Align::End);
            const Rect meter{area.x, area.y + 28, area.w, 14};
            g.fill_round(meter, 7, kLine);
            g.push_round_clip(meter, 7);
            const float clips_w = meter.w * static_cast<float>(std::min(1.0, static_cast<double>(counts.bytes) / 1e9 / max_gb));
            const float cache_w = meter.w * static_cast<float>(std::min(1.0, static_cast<double>(cache) / 1e9 / max_gb));
            const Color clips_color = Color::hex(0x0b8f7c);
            const Color cache_color = Color::hex(0x8aa09c);
            g.fill(Rect{meter.x, meter.y, clips_w, meter.h}, clips_color);
            g.fill(Rect{meter.x + clips_w, meter.y, cache_w, meter.h}, cache_color);
            g.pop_clip();
            float lx = area.x;
            const float ly = meter.bottom() + 12;
            for (const auto& [name, bytes, color] :
                 {std::tuple{L"Clips", counts.bytes, clips_color}, std::tuple{L"Cache", cache, cache_color}}) {
                g.fill_circle(lx + 5, ly + 9, 5, color);
                lx += 17 + g.text(std::format(L"{} {}", name, to_wide(format_size(bytes))),
                                  Rect{lx + 17, ly, 200, 18}, TextStyle{12.5f}, kMute);
                lx += 14;
            }
        };
        add(std::move(row));
    }
    {
        SettingRow row;
        row.id = "cache";
        row.section = "storage";
        row.kind = Kind::Custom;
        row.label = L"Clear cache";
        row.description = L"Thumbnails and previews. Clips are not touched; thumbnails are made again.";
        row.keywords = L"cache thumbnails";
        row.height = 30;
        row.paint = [this](const Rect& area) {
            const float w = button_width(L"Clear cache", 0, true);
            push_button(area.right() - w, area.y, L"Clear cache", 0, ButtonStyle::Line, true, Action::Setting,
                        "cache-clear", cache_bytes_ <= 0);
            graphics_->text(cache_bytes_ < 0 ? std::wstring(L"...") : to_wide(format_size(cache_bytes_)),
                            Rect{area.right() - w - 12 - 120, area.y, 120, 30}, TextStyle{14, Weight::Bold, false, true},
                            kInk, Align::End);
        };
        add(std::move(row));
    }

    {
        SettingRow row;
        row.id = "repair";
        row.section = "storage";
        row.kind = Kind::Custom;
        row.label = L"Repair clips from other programs";
        row.description = L"Mixes the mic of Medal and similar apps into the main sound. Originals go to \"Before repair\".";
        row.keywords = L"repair medal mic microphone audio sound track fix";
        row.full = true;
        row.height = repair_row_height();
        row.paint = [this](const Rect& area) { paint_repair_row(area); };
        add(std::move(row));
    }

    // Sharing & Accounts: links through catbox.moe (core/ShareUpload.h); accounts later.
    enum_row("sharing", "shareHost", L"Upload shared clips to",
             L"catbox.moe keeps a clip (it cannot be deleted again); Litterbox deletes it after 3 days. Anyone with "
             L"the link can watch it. Discord plays both links in the chat.",
             {L"catbox.moe", L"Litterbox (3 days)"}, &Settings::share_host)
        .keywords = L"share link discord catbox litterbox upload";
    toggle_row("sharing", "autoCopy", L"Copy the link when an upload finishes", L"So you can paste it straight away.",
               settings_.share_copy_link, defaults.share_copy_link, [](Settings& s, bool v) { s.share_copy_link = v; })
        .keywords = L"copy link clipboard";
    soon_row("sharing", "accounts", Kind::Toggle, L"Connected accounts", L"YouTube and others are coming later.");

    // Notifications & Overlay.
    toggle_row("notify", "toastSave", L"Show a message when a clip is saved",
               L"Only when the Backtrack window is not in front.", settings_.notification, defaults.notification,
               [](Settings& s, bool v) { s.notification = v; })
        .keywords = L"toast notification popup";
    toggle_row("notify", "soundSave", L"Play a sound when a clip is saved",
               L"The Windows \"asterisk\" sound, so you know without looking.", settings_.sound, defaults.sound,
               [](Settings& s, bool v) { s.sound = v; })
        .keywords = L"sound chime beep";
    toggle_row("notify", "showBar", L"Show the \"Saved\" bar in the app",
               L"The floating bar at the bottom of the library.", settings_.saved_bar, defaults.saved_bar,
               [](Settings& s, bool v) { s.saved_bar = v; });
    toggle_row("notify", "overlayShow", L"Show the in-screen overlay",
               overlay_ && !overlay_->available()
                   ? L"Needs Windows 10 version 2004 or later, which can keep it out of recordings."
                   : L"A small status chip on top of your game (not over exclusive fullscreen). It is never part of "
                     L"recordings.",
               settings_.overlay, defaults.overlay, [](Settings& s, bool v) { s.overlay = v; })
        .keywords = L"overlay chip status";
    enum_row("notify", "overlayPos", L"Overlay position", L"On the display Instant Replay records.",
             {L"Top left", L"Top right", L"Bottom left", L"Bottom right"}, &Settings::overlay_corner)
        .keywords = L"overlay corner position";
    {
        // 20 to 100 % in steps of 5.
        SettingRow row;
        row.id = "overlayOpacity";
        row.section = "notify";
        row.kind = Kind::Slider;
        row.label = L"Overlay opacity";
        row.keywords = L"overlay transparent opacity";
        const auto percent = [](float f) { return 20 + 5 * static_cast<int>(std::lround(f * 16.0f)); };
        row.value = static_cast<float>(settings_.overlay_opacity - 20) / 80.0f;
        row.value_text = std::format(L"{}%", settings_.overlay_opacity);
        row.preview = [percent](float f) { return std::format(L"{}%", percent(f)); };
        row.changed = settings_.overlay_opacity != defaults.overlay_opacity;
        row.drag = [this, percent](float f, bool done) {
            if (done) change_settings([&](Settings& s) { s.overlay_opacity = percent(f); });
        };
        row.reset = [defaults](Settings& s) { s.overlay_opacity = defaults.overlay_opacity; };
        add(std::move(row));
    }

    // Appearance.
    {
        SettingRow row;
        row.id = "theme";
        row.section = "appearance";
        row.kind = Kind::Custom;
        row.label = L"Color theme";
        row.description = L"For the whole app.";
        row.full = true;
        row.keywords = L"theme color colour accent dark light night shift harbor plum moss daylight";
        row.changed = settings_.theme != defaults.theme;
        row.reset = [defaults](Settings& s) { s.theme = defaults.theme; };
        row.height = 36;
        row.paint = [this](const Rect& area) {
            Graphics& g = *graphics_;
            // .st-preset: the background with an accent bar, the chosen one ringed in the accent.
            float x = area.x;
            for (std::size_t i = 0; i < kThemes.size(); ++i) {
                const ThemeInput colors = theme_input(kThemes[i]);
                const bool chosen = kThemes[i] == settings_.theme;
                const Rect s{x, area.y + 2, 40, 32};
                g.fill_round(s, 9, Color::hex(colors.bg.value()));
                g.fill_round(Rect{s.x + 7, s.y + 7, 12, 18}, 4, Color::hex(colors.surface.value()));
                g.fill_round(Rect{s.x + 22, s.y + 7, 10, 18}, 4, Color::hex(colors.accent.value()));
                const bool hot = hovered(Action::Setting, std::format("theme:{}", i));
                g.stroke_round(s, 9, chosen ? kAccent : hot ? kOff : kLine, chosen ? 2.0f : 1.0f);
                add_hit(s, Action::Setting, std::format("theme:{}", i));
                x += 46;
            }
            g.text(std::wstring(theme_name(settings_.theme)), Rect{x + 4, area.y, 160, 36}, TextStyle{13}, kMute);
        };
        add(std::move(row));
    }
    enum_row("appearance", "density", L"Density", L"Compact puts the clips closer together.",
             {L"Comfortable", L"Compact"}, &Settings::density)
        .keywords = L"density compact spacing";
    enum_row("appearance", "thumb", L"Thumbnail size", L"Five, four or three clips in a row on a wide window.",
             {L"S", L"M", L"L"}, &Settings::thumbnail_size)
        .keywords = L"thumbnail size grid columns";
    toggle_row("appearance", "motion", L"Reduce motion", L"Turns off the overlay's fades.", settings_.reduce_motion,
               defaults.reduce_motion, [](Settings& s, bool v) { s.reduce_motion = v; })
        .keywords = L"motion animation";

    // About.
    {
        SettingRow row;
        row.id = "about";
        row.section = "about";
        row.kind = Kind::Custom;
        row.full = true;
        row.keywords = L"version about";
        row.height = 56;
        row.paint = [this](const Rect& area) {
            Graphics& g = *graphics_;
            const Rect logo{area.x, area.y, 56, 56};
            g.logo(logo);
            g.text(L"Backtrack", Rect{area.x + 72, area.y + 6, 300, 24}, TextStyle{18, Weight::Bold}, kInk);
            g.text(std::format(L"Version {}", to_wide(BT_VERSION)), Rect{area.x + 72, area.y + 30, 300, 20}, TextStyle{13},
                   kMute);
        };
        add(std::move(row));
    }
    {
        SettingRow row;
        row.id = "news";
        row.section = "about";
        row.kind = Kind::Custom;
        row.label = L"What is new";
        row.full = true;
        row.keywords = L"changelog release notes";
        static const wchar_t* const kNews[] = {
            L"Export clips as MP4 or GIF, with a \"Fits Discord\" option that stays under 10 MB",
            L"Colour themes, an in-game overlay, bookmarks, and more room between clips",
            L"Mouse pointer, resolution and quality settings; separate mic and game sound tracks",
            L"Updates itself in the background",
            L"Your microphone in every clip, with noise suppression, a mute key and push-to-talk",
            L"A page for every clip: play it, trim it without losing quality, add tags and notes",
            L"Record and Settings pages: change everything here instead of in backtrack.ini",
            L"Collections, favorites and a Trash that keeps clips for 30 days",
            L"Instant Replay with game sound, on your graphics card's own encoder",
        };
        row.height = 20 + static_cast<float>(std::size(kNews)) * 21;
        row.paint = [this](const Rect& area) {
            Graphics& g = *graphics_;
            g.text(to_wide(BT_VERSION), Rect{area.x, area.y, 200, 18}, TextStyle{14, Weight::Semibold}, kInk);
            float y = area.y + 22;
            for (const wchar_t* line : kNews) {
                g.fill_circle(area.x + 8, y + 10, 2, kMute);
                g.text(line, Rect{area.x + 18, y, area.w - 18, 20}, TextStyle{13}, kMute);
                y += 21;
            }
        };
        add(std::move(row));
    }
    {
        SettingRow row;
        row.id = "licenses";
        row.section = "about";
        row.kind = Kind::Custom;
        row.label = L"Open source licenses";
        row.full = true;
        row.keywords = L"licenses open source";
        row.height = 40;
        row.paint = [this](const Rect& area) {
            graphics_->paragraph(L"None needed: Backtrack is built only on parts of Windows (Media Foundation, Direct3D, "
                            L"Direct2D). No third-party code is included.",
                                 area, TextStyle{13}, kMute);
        };
        add(std::move(row));
    }
    toggle_row("about", "debugLog", L"Detailed log",
               L"Adds performance counters to backtrack.log once a minute. Useful when reporting a problem.",
               settings_.log_level == LogLevel::Debug, defaults.log_level == LogLevel::Debug,
               [](Settings& s, bool v) { s.log_level = v ? LogLevel::Debug : LogLevel::Info; })
        .keywords = L"debug log diagnostics";
    {
        SettingRow row;
        row.id = "diag";
        row.section = "about";
        row.kind = Kind::Custom;
        row.label = L"Diagnostics";
        row.description = L"Details to include when you report a problem.";
        row.keywords = L"diagnostics support log";
        row.height = 30;
        row.paint = [this](const Rect& area) {
            const float copy_w = button_width(L"Copy diagnostics", glyph::kCopy, true);
            const float log_w = button_width(L"Open log", glyph::kFolder, true);
            push_button(area.right() - copy_w, area.y, L"Copy diagnostics", glyph::kCopy, ButtonStyle::Line, true,
                        Action::Setting, "diag");
            push_button(area.right() - copy_w - 8 - log_w, area.y, L"Open log", glyph::kFolder, ButtonStyle::Ghost, true,
                        Action::Setting, "open-log");
        };
        add(std::move(row));
    }
    return rows;
}

// ---------------------------------------------------------------------------------------------
// Actions

void App::run_setting(const std::string& what, const Rect& anchor) {
    if (what.starts_with("tab:")) {
        settings_section_ = what.substr(4);
        SetWindowTextW(settings_search_, L"");
        settings_query_.clear();
        page_scroll_ = 0;
        if (settings_section_ == "storage") cache_bytes_ = folder_bytes(paths_.cache);
        invalidate();
        return;
    }
    if (what == "folder-change") {
        change_clips_folder();
        return;
    }
    if (what.starts_with("repair-")) {
        run_repair(what);
        return;
    }
    if (what == "folder-open") {
        shell::open(recorder_->output_dir());
        return;
    }
    if (what == "open-log") {
        shell::open(paths_.log);
        return;
    }
    if (what == "cache-clear") {
        std::error_code ec;
        for (fs::directory_iterator it(paths_.cache, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code remove_ec;
            fs::remove(it->path(), remove_ec);  // one in use is simply left
        }
        for (const auto& [key, used] : bitmap_use_) graphics_->forget_bitmap(key);
        bitmap_use_.clear();
        pending_thumbs_.clear();
        thumbs_missing_.clear();
        urgent_.clear();
        cache_bytes_ = folder_bytes(paths_.cache);
        invalidate();
        return;
    }
    if (what == "diag") {
        const BufferStatus status = recorder_->buffer_status();
        std::wstring text = std::format(L"Backtrack {}\r\nStatus: {}\r\nEncoder: {}\r\n", to_wide(BT_VERSION),
                                        to_wide(recorder_->status_text()),
                                        status.encoder_running ? (status.hardware_encoder ? L"hardware" : L"software")
                                                               : L"not running");
        for (std::size_t i = 0; i < monitors_.size(); ++i) {
            text += std::format(L"Display {}: {}x{} on {}{}\r\n", i + 1, monitors_[i].desktop_size.width,
                                monitors_[i].desktop_size.height, to_wide(monitors_[i].adapter_name),
                                monitors_[i].primary ? L" (main)" : L"");
        }
        if (const auto ini = read_ini_file(paths_.ini)) text += L"\r\n" + to_wide(*ini);
        if (OpenClipboard(window_)) {
            EmptyClipboard();
            const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
            if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                if (void* data = GlobalLock(memory)) {
                    memcpy(data, text.c_str(), bytes);
                    GlobalUnlock(memory);
                    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) GlobalFree(memory);
                } else {
                    GlobalFree(memory);
                }
            }
            CloseClipboard();
            show_notice(L"Diagnostics copied. Paste them into your message.");
        }
        return;
    }
    if (const auto target = hotkey_target_of(what, "")) {
        capturing_hotkey_ = true;
        capture_target_ = *target;
        message_target_ = capture_target_;
        hotkey_message_.clear();
        // Free the keys while listening, or pressing a current one would save a clip instead.
        unregister_hotkeys();
        SetFocus(window_);
        invalidate();
        return;
    }
    if (const auto target = hotkey_target_of(what, "-reset")) {
        hotkey_message_.clear();
        // The default of one may be what another is set to now: one key cannot do two things.
        const Settings d{};
        const std::string& default_keys = hotkey_text(d, *target);
        if (*target != HotkeyTarget::PushToTalk) {
            for (int i = 0; i < kGlobalHotkeys; ++i) {
                const auto other = static_cast<HotkeyTarget>(i);
                if (other != *target && !default_keys.empty() && default_keys == hotkey_text(settings_, other)) {
                    message_target_ = *target;
                    hotkey_message_ = std::format(L"{} already {}. Change that one first.", to_wide(default_keys),
                                                  hotkey_does(other));
                    invalidate();
                    return;
                }
            }
        }
        const HotkeyTarget t = *target;
        change_settings([t](Settings& s) { hotkey_text(s, t) = hotkey_text(Settings{}, t); });
        return;
    }
    if (what == "update" || what == "update-download" || what == "update-install" || what == "update-page") {
        if (updater_ == nullptr) return;
        if (what == "update") updater_->check();
        if (what == "update-download") updater_->download();
        if (what == "update-page") shell::open(fs::path(to_wide(updater_->state().page_url)));
        if (what == "update-install") {
            // A clip being saved is finished first: quit() waits for the recorder.
            if (updater_->install_now()) {
                update_started_ = true;
                quit();
            } else {
                show_notice(L"The update could not be started. backtrack.log says why.");
            }
        }
        invalidate();
        return;
    }
    if (what.starts_with("theme:")) {
        const int index = std::atoi(what.c_str() + 6);
        if (index >= 0 && static_cast<std::size_t>(index) < kThemes.size()) {
            change_settings([index](Settings& s) { s.theme = kThemes[static_cast<std::size_t>(index)]; });
        }
        return;
    }
    if (what == "mic-privacy") {
        shell::open(L"ms-settings:privacy-microphone");
        return;
    }

    std::vector<SettingRow> rows = setting_rows();
    if (what == "reset-all" || what.starts_with("reset:")) {
        const std::string section = what == "reset-all" ? std::string() : what.substr(6);
        change_settings([&](Settings& s) {
            for (const SettingRow& row : rows) {
                if (row.reset && row.changed && (section.empty() || row.section == section)) row.reset(s);
            }
        });
        // "Launch with Windows" is not in backtrack.ini.
        for (const SettingRow& row : rows) {
            if (row.id == "startup" && row.changed && (section.empty() || row.section == section)) {
                set_launch_at_startup(false);
            }
        }
        return;
    }
    // "<row>" (toggle, select) or "<row>:<option>" (segmented, select menu choice).
    const std::size_t colon = what.find(':');
    const std::string id = what.substr(0, colon);
    for (SettingRow& row : rows) {
        if (row.id != id || row.soon) continue;
        if (row.kind == SettingRow::Kind::Toggle && row.choose) {
            row.choose(row.on ? 0 : 1);
        } else if (row.kind == SettingRow::Kind::Segmented && row.choose && colon != std::string::npos) {
            row.choose(std::stoi(what.substr(colon + 1)));
        } else if (row.kind == SettingRow::Kind::Select && row.choose) {
            Menu menu;
            menu.anchor = anchor;
            menu.min_width = anchor.w;
            for (std::size_t i = 0; i < row.options.size(); ++i) {
                const int choice = static_cast<int>(i);
                const std::function<void(int)> choose = row.choose;
                menu.items.push_back(MenuItem{row.options[i], [choose, choice] { choose(choice); },
                                              static_cast<int>(i) == row.selected});
            }
            open_menu(std::move(menu));
        }
        break;
    }
}

void App::drag_setting(const std::string& id, float fraction, bool done) {
    // "set:<row>" sliders: the value text follows the mouse; the setting changes on release.
    const std::string row_id = id.substr(4);
    if (fraction >= 0) drag_preview_ = std::pair{id, fraction};
    if (done) {
        const std::optional<std::pair<std::string, float>> last = std::exchange(drag_preview_, std::nullopt);
        if (last && last->first == id) {
            for (SettingRow& row : setting_rows()) {
                if (row.id == row_id && row.drag) row.drag(last->second, true);
            }
        }
    }
    invalidate();
}

void App::on_drag(const std::string& id, float fraction, bool done) {
    if (id.starts_with("clip:")) {
        drag_clip(id, fraction, done);
    } else if (id.starts_with("set:")) {
        drag_setting(id, fraction, done);
    }
}

// ---------------------------------------------------------------------------------------------
// Painting

namespace {

// A segmented control with long options does not fit beside its label in a narrow window.
bool wide_segments(Graphics& g, const std::vector<std::wstring>& options, float w) {
    float total = 6;
    for (const std::wstring& option : options) total += std::max(34.0f, g.text_width(option, TextStyle{13, Weight::Semibold}) + 22);
    return total > w * 0.45f;
}

}  // namespace

float App::paint_setting_row(const SettingRow& source, float x, float y, float w, bool last) {
    Graphics& g = *graphics_;
    using Kind = SettingRow::Kind;
    SettingRow row = source;
    if (row.kind == Kind::Segmented && wide_segments(g, row.options, w)) row.full = true;
    // .srow: 14 px above and below; label 14 semibold, description 12.5 muted.
    const bool has_label = !row.label.empty();
    const float label_h = has_label ? 20.0f + (row.description.empty() ? 0.0f : 20.0f) : 0.0f;
    const float control_h = row.kind == Kind::Custom ? row.height : 36.0f;
    const float h = row.full ? label_h + (has_label ? 12.0f : 0.0f) + control_h : std::max(label_h, control_h);
    const float top = y + 14;
    const float cy = row.full ? top + label_h + (has_label ? 12 : 0) + control_h / 2 : top + h / 2;

    if (has_label) {
        const float ly = row.full ? top : top + (h - label_h) / 2;
        const Color ink = row.soon ? kInk.with_alpha(0.55f) : kInk;
        const float lw = g.text(row.label, Rect{x, ly, row.full ? w : w * 0.55f - 12, 20}, TextStyle{14, Weight::Semibold}, ink);
        float after = x + lw + 8;
        if (row.changed) {
            g.fill_circle(after + 3.5f, ly + 10, 3.5f, kAccent);
            after += 15;
        }
        if (row.soon) soon_badge(after, ly + 10);
        if (!row.description.empty()) {
            // As wide as the control on the right leaves room for.
            const float room = row.full                   ? w
                               : row.kind == Kind::Toggle ? w - 42 - 24
                               : row.kind == Kind::Custom ? w * 0.4f - 12
                                                          : w * 0.55f - 12;
            g.text(row.description, Rect{x, ly + 22, room, 18}, TextStyle{12.5f},
                   row.soon ? kMute.with_alpha(0.6f) : kMute);
        }
    }

    const float right = x + w;
    const std::string arg = row.id;
    switch (row.kind) {
        case Kind::Toggle: toggle(right, cy, row.on, Action::Setting, arg, row.soon); break;
        case Kind::Segmented: {
            float right_edge = right;
            if (row.full) {
                float total = 6;
                for (const std::wstring& option : row.options) {
                    total += std::max(34.0f, g.text_width(option, TextStyle{13, Weight::Semibold}) + 22);
                }
                right_edge = std::min(right, x + total);
            }
            segmented(right_edge, cy, row.options, row.selected, Action::Setting, arg, row.soon);
            break;
        }
        case Kind::Select: {
            float widest = 0;
            for (const std::wstring& option : row.options) {
                widest = std::max(widest, g.text_width(option, TextStyle{13.5f}));
            }
            const float sw = std::clamp(widest + 12 + 34, 130.0f, std::max(130.0f, w * 0.42f));
            select_box(Rect{right - sw, cy - 18, sw, 36},
                       row.options.empty() ? std::wstring() : row.options[static_cast<std::size_t>(row.selected)],
                       Action::Setting, arg, row.soon);
            break;
        }
        case Kind::Slider: {
            const std::string drag_id = "set:" + row.id;
            const bool previewing = drag_preview_ && drag_preview_->first == drag_id;
            const float value = previewing ? drag_preview_->second : row.value;
            const std::wstring text = previewing && row.preview ? row.preview(value) : row.value_text;
            const float text_w = 76;
            g.text(text, Rect{right - text_w, cy - 10, text_w, 20}, TextStyle{13.5f, Weight::Bold, false, true},
                   row.soon ? kInk.with_alpha(0.45f) : kInk, Align::End);
            const float slider_w = std::clamp(w * 0.45f - text_w - 16, 100.0f, 200.0f);
            slider(Rect{right - text_w - 16 - slider_w, cy - 12, slider_w, 24}, value, drag_id, row.soon);
            break;
        }
        case Kind::Custom: {
            const Rect area = row.full ? Rect{x, cy - control_h / 2, w, control_h}
                                       : Rect{x + w * 0.4f, cy - control_h / 2, w * 0.6f, control_h};
            if (row.paint) row.paint(area);
            break;
        }
    }
    const float bottom = top + h + 14;
    if (!last) g.fill(Rect{x, bottom - 1, w, 1}, kLine);
    return bottom;
}

float App::hotkey_row(float x, float y, float w, HotkeyTarget target, const wchar_t* label, const wchar_t* description,
                      const std::string& keys, const std::string& default_keys, bool registered) {
    Graphics& g = *graphics_;
    const std::string arg = std::string("hotkey-") + hotkey_arg(target);
    // .hk: 10 px padding, label + small description, the key button on the right.
    g.text(label, Rect{x, y + 10, w * 0.5f, 20}, TextStyle{14, Weight::Semibold}, kInk);
    g.text(description, Rect{x, y + 30, w - 34 - 8 - 170 - 12, 18}, TextStyle{12.5f}, kMute);
    const Rect key_button{x + w - 34 - 8 - 170, y + 10, 170, 38};
    const bool listening = capturing_hotkey_ && capture_target_ == target;
    if (listening) {
        g.fill_round(key_button, 10, kSoft);
        g.stroke_round(key_button, 10, kAccent);
        g.text(target == HotkeyTarget::PushToTalk ? L"Press a key or side button" : L"Press keys...", key_button.inset(10, 0),
               TextStyle{13, Weight::Semibold}, kAccent, Align::End);
    } else {
        if (hovered(Action::Setting, arg)) {
            g.fill_round(key_button, 10, kBg);
            g.stroke_round(key_button, 10, kOff);
        }
        if (keys.empty()) {
            g.text(L"None", key_button.inset(10, 0), TextStyle{13}, kMute, Align::End);
        } else {
            float chips_w = 0;
            const std::wstring text = to_wide(keys);
            for (std::size_t start = 0; start <= text.size();) {
                const std::size_t plus = text.find(L'+', start);
                const std::wstring part = text.substr(start, plus == std::wstring::npos ? std::wstring::npos : plus - start);
                chips_w += std::max(22.0f, g.text_width(part, TextStyle{11, Weight::Semibold}) + 12) + 3;
                if (plus == std::wstring::npos) break;
                start = plus + 1;
            }
            key_chips(key_button.right() - 10 - chips_w + 3, key_button.y + 9, keys);
        }
    }
    add_hit(key_button, Action::Setting, arg);
    icon_button(Rect{x + w - 34, y + 12, 34, 34}, glyph::kRestoreDefaults, 13, Action::Setting, arg + "-reset",
                keys == default_keys);
    y += 58;
    std::wstring warning;
    if (!hotkey_message_.empty() && message_target_ == target) {
        warning = hotkey_message_;
    } else if (!registered && !keys.empty() && !capturing_hotkey_) {  // all are let go while listening
        // register_hotkey gives a key set twice to the first one only.
        warning = std::format(L"{} is used by another program. Pick another one.", to_wide(keys));
        for (int i = 0; i < static_cast<int>(target) && i < kGlobalHotkeys; ++i) {
            if (keys == hotkey_text(settings_, static_cast<HotkeyTarget>(i))) {
                warning = std::format(L"{} already {}. Pick another key.", to_wide(keys),
                                      hotkey_does(static_cast<HotkeyTarget>(i)));
                break;
            }
        }
    }
    if (!warning.empty()) {
        // .warn: why the key does not work.
        g.icon(glyph::kInfo, Rect{x, y - 4, 16, 20}, 12, kWarn);
        g.text(warning, Rect{x + 22, y - 4, w - 22, 20}, TextStyle{12.5f, Weight::Semibold}, kWarn);
        y += 26;
    }
    g.fill(Rect{x, y - 1, w, 1}, kLine);
    return y;
}

float App::paint_hotkeys(float x, float y, float w) {
    Graphics& g = *graphics_;
    const Settings defaults{};
    y = hotkey_row(x, y, w, HotkeyTarget::Save, L"Save clip", L"Saves the last stretch of Instant Replay as a clip",
                   settings_.save_hotkey, defaults.save_hotkey, hotkey_ok_[0]);
    y = hotkey_row(x, y, w, HotkeyTarget::MicMute, L"Toggle microphone",
                   L"Mutes or unmutes your mic in recordings. Backspace: no key", settings_.mic_mute_hotkey,
                   defaults.mic_mute_hotkey, hotkey_ok_[1]);
    y = hotkey_row(x, y, w, HotkeyTarget::Overlay, L"Show overlay",
                   L"Shows or hides the small status overlay. Backspace: no key", settings_.overlay_hotkey,
                   defaults.overlay_hotkey, hotkey_ok_[2]);
    y = hotkey_row(x, y, w, HotkeyTarget::Bookmark, L"Add a bookmark",
                   L"Marks a moment; it shows on the clip's timeline. Backspace: no key", settings_.bookmark_hotkey,
                   defaults.bookmark_hotkey, hotkey_ok_[3]);
    y = hotkey_row(x, y, w, HotkeyTarget::PushToTalk, L"Push-to-talk",
                   settings_.push_to_talk ? L"Hold it to record your mic" : L"Turn on push-to-talk in Audio to use it",
                   settings_.push_to_talk_key, defaults.push_to_talk_key, true);
    // Not there yet: greyed with "Soon".
    struct Row {
        const wchar_t* label;
        const wchar_t* description;
        const char* keys;
    };
    static constexpr Row kSoon[] = {
        {L"Start or stop recording", L"Records the whole screen until you stop it", "Alt+F9"},
        {L"Take screenshot", L"Captures the selected display as an image", "Alt+F12"},
    };
    for (std::size_t i = 0; i < std::size(kSoon); ++i) {
        const Row& r = kSoon[i];
        g.text(r.label, Rect{x, y + 10, w * 0.5f, 20}, TextStyle{14, Weight::Semibold}, kInk.with_alpha(0.55f));
        soon_badge(x + g.text_width(r.label, TextStyle{14, Weight::Semibold}) + 8, y + 20);
        g.text(r.description, Rect{x, y + 30, w - 34 - 8 - 170 - 12, 18}, TextStyle{12.5f}, kMute.with_alpha(0.6f));
        const Rect key_button{x + w - 34 - 8 - 170, y + 10, 170, 38};
        float chips_w = 0;
        const std::wstring text = to_wide(r.keys);
        for (std::size_t start = 0; start <= text.size();) {
            const std::size_t plus = text.find(L'+', start);
            const std::wstring part = text.substr(start, plus == std::wstring::npos ? std::wstring::npos : plus - start);
            chips_w += std::max(22.0f, g.text_width(part, TextStyle{11, Weight::Semibold}) + 12) + 3;
            if (plus == std::wstring::npos) break;
            start = plus + 1;
        }
        g.push_clip(key_button);
        key_chips(key_button.right() - 10 - chips_w + 3, key_button.y + 9, r.keys);
        g.pop_clip();
        g.fill(key_button, kCard.with_alpha(0.5f));  // greyed
        y += 58;
        if (i + 1 < std::size(kSoon)) g.fill(Rect{x, y - 1, w, 1}, kLine);
    }
    return y;
}

void App::paint_settings_page(const Rect& area) {
    Graphics& g = *graphics_;
    if (cache_bytes_ < 0) cache_bytes_ = folder_bytes(paths_.cache);
    const Rect page = begin_page(area);
    const float x0 = page.x + kPad;
    const float cw = page.w - 2 * kPad;
    float y = page.y + kPageTop;
    std::vector<SettingRow> rows = setting_rows();

    // .head: title, subtitle, search, reset all.
    g.text(L"Settings", Rect{x0, y, 300, 38}, TextStyle{28, Weight::Bold, true}, kInk);
    const std::wstring query = lower(to_wide(settings_query_));
    std::vector<const SettingRow*> matches;
    if (!query.empty()) {
        for (const SettingRow& row : rows) {
            const Section* s = find_section(row.section);
            const std::wstring haystack =
                lower((s != nullptr ? std::wstring(s->title) : L"") + L" " + row.label + L" " + row.description + L" " +
                      row.keywords);
            if (haystack.find(query) != std::wstring::npos) matches.push_back(&row);
        }
    }
    std::wstring subtitle = L"Changes apply right away and are remembered";
    if (!query.empty()) {
        subtitle = matches.empty() ? std::wstring(L"No matches")
                                   : std::format(L"{} matching setting{}", matches.size(), matches.size() == 1 ? L"" : L"s");
    }
    g.text(subtitle, Rect{x0, y + 40, cw * 0.55f, 20}, TextStyle{13.5f}, kMute);
    if (!ini_problem_.empty()) {
        g.text(to_wide(ini_problem_), Rect{x0, y + 66, cw, 20}, TextStyle{13, Weight::Semibold}, kWarn);
        y += 24;
    }
    bool any_changed = false;
    for (const SettingRow& row : rows) any_changed = any_changed || (row.changed && row.reset);
    const float reset_w = button_width(L"Reset all to defaults", glyph::kRestoreDefaults, false);
    push_button(x0 + cw - reset_w, y + 22, L"Reset all to defaults", glyph::kRestoreDefaults, ButtonStyle::Line, false,
                Action::Setting, "reset-all", !any_changed);
    const float search_w = std::min(280.0f, std::max(160.0f, cw - reset_w - 12 - 380));
    const Rect search{x0 + cw - reset_w - 12 - search_w, y + 22, search_w, 38};
    const bool focused = GetFocus() == settings_search_;
    g.fill_round(search, kPageRadius, kInk.with_alpha(0.05f));
    g.stroke_round(search, kPageRadius, focused ? kAccent : kLine);
    g.icon(glyph::kSearch, Rect{search.x + 12, search.y, 16, search.h}, 13, kMute);
    place_control(settings_search_, Rect{search.x + 36, search.y + 10, search.w - 46, 18}, true);
    y += 60 + 18;

    // .swrap: the section list (210) and the section.
    const bool stacked = cw < 760;
    const float nav_w = stacked ? 0.0f : 210.0f;
    const float sx = x0 + (stacked ? 0.0f : nav_w + 28);
    const float sw = std::min(cw - (stacked ? 0.0f : nav_w + 28), 780.0f);
    float nav_bottom = y;
    if (!stacked) {
        float ny = y;
        for (const Section& s : kSections) {
            const Rect r{x0, ny, nav_w, 38};
            const bool on = query.empty() && settings_section_ == s.id;
            const std::string arg = std::string("tab:") + s.id;
            if (on) {
                g.fill_round(Rect{r.x, r.y + 1, r.w, r.h}, 10, kLine);
                g.fill_round(r, 10, kCard);
            } else if (hovered(Action::Setting, arg)) {
                g.fill_round(r, 10, kInk.with_alpha(0.06f));
            }
            g.icon(s.glyph, Rect{r.x + 12, r.y, 17, r.h}, 14, on ? kInk : kInk.with_alpha(0.85f));
            g.text(s.title, Rect{r.x + 39, r.y, r.w - 60, r.h}, TextStyle{14, on ? Weight::Semibold : Weight::Regular}, kInk);
            bool changed = false;
            for (const SettingRow& row : rows) changed = changed || (row.section == s.id && row.changed);
            if (changed) g.fill_circle(r.right() - 16, r.y + r.h / 2, 4, kAccent);
            add_hit(r, Action::Setting, arg);
            ny += 38 + 2;
        }
        nav_bottom = ny;
    }

    const auto section_block = [&](const Section& s, const std::vector<const SettingRow*>& list, bool search_mode,
                                   float top) -> float {
        // .sh: title (+ "N changed"), description, Reset section on the right.
        int changed = 0;
        for (const SettingRow* row : list) changed += row->changed ? 1 : 0;
        const float tw = g.text(s.title, Rect{sx, top, sw * 0.6f, 26}, TextStyle{19, Weight::Bold, true}, kInk);
        if (changed > 0 && !search_mode) {
            const std::wstring count = std::format(L"{} changed", changed);
            const float pw = g.text_width(count, TextStyle{12, Weight::Semibold}) + 18;
            const Rect pill{sx + tw + 10, top + 3, pw, 20};
            g.fill_round(pill, 10, kSoft);
            g.text(count, pill, TextStyle{12, Weight::Semibold}, kAccent, Align::Center);
        }
        float head_h = 26;
        if (!search_mode && s.description[0] != L'\0') {
            g.text(s.description, Rect{sx, top + 28, sw * 0.7f, 18}, TextStyle{13}, kMute);
            head_h = 48;
        }
        if (search_mode) {
            const float bw = button_width(L"Open section", glyph::kChevronRight, true);
            push_button(sx + sw - bw, top - 2, L"Open section", glyph::kChevronRight, ButtonStyle::Ghost, true,
                        Action::Setting, std::string("tab:") + s.id);
        } else if (std::string_view(s.id) != "about") {
            bool resettable = false;
            for (const SettingRow* row : list) resettable = resettable || (row->changed && row->reset);
            const float bw = button_width(L"Reset section", glyph::kRestoreDefaults, true);
            push_button(sx + sw - bw, top + head_h - 30, L"Reset section", glyph::kRestoreDefaults, ButtonStyle::Line,
                        true, Action::Setting, std::string("reset:") + s.id, !resettable);
        }
        float ry = top + head_h + 12;
        // .sg: the rows on a card, 4 px / 20 px padding. Measured by drawing into a clip first is
        // not possible, so the card height is the sum of the row heights.
        float card_h = 8;
        for (const SettingRow* row : list) {
            const bool has_label = !row->label.empty();
            const float label_h = has_label ? 20.0f + (row->description.empty() ? 0.0f : 20.0f) : 0.0f;
            const float control_h = row->kind == SettingRow::Kind::Custom ? row->height : 36.0f;
            const bool full = row->full || (row->kind == SettingRow::Kind::Segmented && wide_segments(g, row->options, sw - 40));
            const float h = full ? label_h + (has_label ? 12.0f : 0.0f) + control_h : std::max(label_h, control_h);
            card_h += h + 28;
        }
        g.fill_round(Rect{sx, ry, sw, card_h}, kPanelRadius, kCard);
        float iy = ry + 4;
        for (std::size_t i = 0; i < list.size(); ++i) {
            iy = paint_setting_row(*list[i], sx + 20, iy, sw - 40, i + 1 == list.size());
        }
        return ry + card_h;
    };

    float bottom = y;
    float sy = stacked ? nav_bottom : y;
    if (stacked) {
        // Narrow window: the sections as a wrapping row of buttons above.
        float bx = x0;
        float by = y;
        for (const Section& s : kSections) {
            const float bw = button_width(s.title, 0, true);
            if (bx + bw > x0 + cw && bx > x0) {
                bx = x0;
                by += 36;
            }
            const bool on = query.empty() && settings_section_ == s.id;
            push_button(bx, by, s.title, 0, on ? ButtonStyle::Primary : ButtonStyle::Line, true, Action::Setting,
                        std::string("tab:") + s.id);
            bx += bw + 6;
        }
        sy = by + 30 + 18;
    }
    if (!query.empty()) {
        if (matches.empty()) {
            g.text(std::format(L"No settings match \"{}\"", to_wide(settings_query_)), Rect{sx, sy + 60, sw, 26},
                   TextStyle{18, Weight::Bold, true}, kInk, Align::Center);
            g.text(L"Try a shorter word, like \"audio\" or \"hotkey\".", Rect{sx, sy + 92, sw, 20}, TextStyle{13.5f}, kMute,
                   Align::Center);
            bottom = sy + 120;
        } else {
            for (const Section& s : kSections) {
                std::vector<const SettingRow*> list;
                for (const SettingRow* row : matches) {
                    if (row->section == s.id) list.push_back(row);
                }
                if (list.empty()) continue;
                sy = section_block(s, list, true, sy) + 30;
            }
            bottom = sy;
        }
    } else {
        const Section* s = find_section(settings_section_);
        if (s == nullptr) s = &kSections[0];
        std::vector<const SettingRow*> list;
        for (const SettingRow& row : rows) {
            if (row.section == s->id) list.push_back(&row);
        }
        bottom = section_block(*s, list, false, sy);
    }
    end_page(area, std::max(bottom, nav_bottom) + 28);
}

}  // namespace bt::ui
