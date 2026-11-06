#pragma once

// The Backtrack window (design: backtrack.html, the "Night shift" look): our own title bar, a
// sidebar with the library views and collections, and one page at a time: the clip grid, the clip
// page (player, trim, details), Record and Settings. Closing the window hides it to the tray icon;
// Instant Replay keeps running until "Quit" in the tray menu.
//
// Drawing is immediate mode: paint() lays out and draws everything and records where each button
// is (hits_), the mouse handlers look those up. No retained widget tree to keep in sync. Menus
// and drop-down lists are drawn inside the window too (menu_), so they match the dark look and
// never run a modal loop of their own.
//
// The window owns the recorder: a setting that only takes effect in a new pipeline (frame rate,
// monitor, ...) stops it and makes a new one with the new settings (restart_recorder).
//
// The member functions are split over several files: App.cpp (window, data, library page),
// Widgets.cpp (buttons, toggles, sliders, menus), ClipPage.cpp, RecordPage.cpp, SettingsPage.cpp.
//
// UI thread only, except SavedInbox::push (the recorder's writer thread), the MediaProbe ready
// callback and the background worker, which only post messages.

#include "core/Config.h"
#include "core/Library.h"
#include "platform/MediaProbe.h"
#include "platform/MicCapture.h"
#include "platform/Recorder.h"
#include "core/ExportPlan.h"
#include "platform/Updater.h"
#include "platform/VideoPlayer.h"
#include "ui/Graphics.h"
#include "ui/Overlay.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bt::ui {

struct AppPaths {
    std::filesystem::path exe_dir;
    std::filesystem::path ini;
    std::filesystem::path log;
    std::filesystem::path library;  // library.json
    std::filesystem::path cache;  // thumbnails
};

// Saved clips travel from the recorder's writer thread to the window through here. It also plays
// the "saved" sound, so a sound setting changed in the window applies to the very next clip.
class SavedInbox {
public:
    void attach(HWND window);
    void set_sound(bool on) { sound_ = on; }
    void push(const SaveResult& result);
    std::vector<SaveResult> take();

private:
    std::mutex mutex_;
    std::vector<SaveResult> results_;
    HWND window_ = nullptr;
    std::atomic<bool> sound_{true};
};

// Which key the Hotkeys page is changing. The first kGlobalHotkeys are global hotkeys
// (RegisterHotKey id = index + 1); push-to-talk is a held key the recorder reads itself.
enum class HotkeyTarget { Save, MicMute, Overlay, Bookmark, PushToTalk };
inline constexpr int kGlobalHotkeys = 4;

// Makes a recorder for the given settings (not started yet).
using RecorderFactory = std::function<std::unique_ptr<Recorder>(const Settings& settings)>;

class App {
public:
    // "Use the default colour" for the optional colour parameters below.
    static constexpr Color kNoColor{0, 0, 0, 0};

    // The window class name: a second Backtrack finds the running one by it.
    static constexpr wchar_t kWindowClass[] = L"Backtrack.Window";
    // Registered message a second copy sends to bring this window forward.
    static constexpr wchar_t kShowMessage[] = L"Backtrack.Show";

    // Makes and starts the recorder, then the window. updater may be null (no update checks).
    App(const Settings& settings, const AppPaths& paths, RecorderFactory make_recorder, SavedInbox& inbox,
        Updater* updater = nullptr);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    HWND window() const { return window_; }
    Recorder& recorder() { return *recorder_; }
    // SW_HIDE leaves it in the tray; a minimised start stays minimised without taking focus.
    void show(int show_command);

    // Also used by the test options.
    void save_clip();
    void toggle_pause();
    void quit();
    // At the end: stops the recorder (every queued clip is written), files the clips it finished
    // meanwhile and saves the library.
    void finish();
    // After the window closed: may a downloaded update be installed now? Not when Windows ended
    // the session or an installer closed Backtrack, and not when "Install now" already started it.
    bool may_install_update() const { return !session_ended_ && !update_started_; }

private:
    enum class Page { Library, Clip, Record, Uploads, Settings };

    enum class Action {
        None,
        Minimize, Maximize, Close,
        View,  // arg: "all", "favorites", "recent", "trash" or a collection id
        NewCollection,
        Page,  // arg: "record", "uploads", "settings"
        SaveNow, TogglePause,
        Sort,
        OpenClip, ToggleFavorite,  // arg: file
        SavedOpen, SavedFolder, SavedTrash, SavedClose,
        MoreCollections,
        Search,
        Clip,  // a control on the clip page; arg names it
        Record,  // a control on the Record page
        Setting,  // a control on the Settings page; arg "<row id>" or "<row id>:<option>"
        Drag,  // press and drag: a slider, the seek bar, a trim handle; arg names it (tracks_)
        MenuItem,  // arg: index into menu_->items
        MenuDismiss,  // anywhere outside an open menu
        Disabled,  // a control that is not available ("Soon"): shows no hover, does nothing
    };

    struct Hit {
        Rect rect;
        Action action = Action::None;
        std::string arg;
    };

    struct SavedBar {
        std::string file;  // empty when the clip went elsewhere (Videos fallback)
        std::filesystem::path path;
        std::string title;
        std::string detail;
        std::string headline;  // "Saved" or "Trimmed clip saved"
        std::chrono::steady_clock::time_point at;
    };

    struct Editing {
        enum class Kind { CollectionName, ClipTitle } kind;
        std::string target;  // collection id or clip file
    };

    struct MenuItem {
        std::wstring label;
        std::function<void()> run;  // null: a separator
        bool checked = false;
        bool danger = false;
        bool enabled = true;
        std::optional<Color> dot;  // a collection colour in front of the label
    };
    struct Menu {
        Rect anchor;  // opens below it (above when there is no room), or at its top-left for a right-click
        bool at_point = false;
        float scroll = 0;  // a menu taller than the window scrolls with the wheel
        float min_width = 0;
        std::vector<MenuItem> items;
    };

    enum class ButtonStyle { Primary, Line, Ghost, Danger, DangerFill };

    struct ClipView {
        std::string file;
        std::unique_ptr<VideoPlayer> player;
        std::uint64_t generation = 0;  // the graphics device the player decodes on
        double in = 0;  // trim, seconds
        double out = 0;  // 0 until the length is known: then the whole clip
        double volume = 0.8;
        bool muted = false;
        int speed = 1;  // index into the speed list
        bool wide = false;
        bool scrubbing = false;
        bool resume_after_scrub = false;
        bool strip_requested = false;
        bool trim_touched = false;  // the user moved a handle: the length the player reports does not reset it
        LPARAM player_tag = 0;  // events from an earlier player of this page carry another tag
        std::string error;  // the player could not be made
        int strip_frames = 0;  // filmstrip pictures uploaded ("strip|<file>|<i>")
    };

    // The Export window (ExportDialog.cpp). Shared with the background job that runs the export.
    struct ExportView {
        enum class Stage { Form, Running, Done, Failed };
        std::string file;
        std::string title;
        ExportOptions options;
        std::optional<ExportSource> source;  // read in the background when the window opens
        Stage stage = Stage::Form;
        std::shared_ptr<std::atomic<double>> progress = std::make_shared<std::atomic<double>>(0.0);
        std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
        // Set on the worker the moment the file is complete, before the UI hears about it.
        std::shared_ptr<std::atomic<bool>> finished = std::make_shared<std::atomic<bool>>(false);
        std::string message;  // Failed: why
        std::filesystem::path result_path;
        std::uint64_t result_bytes = 0;
    };

    // Values a Settings page row can show and change; built in SettingsPage.cpp.
    struct SettingRow;

    void construct();
    void release();  // everything the window owns; also when construct() failed halfway
    void invalidate();
    void apply_look();  // the colour theme from settings_, into the drawing tokens and the GDI brushes
    void show_notice(std::wstring text);
    void request_missing_info();
    void keep_copy_of_library(const wchar_t* why);
    // False (with a notice) while clips go to the Videos fallback: the library's files are elsewhere.
    bool clips_folder_available();

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT hit_test(POINT screen) const;

    // Data.
    void load_library();
    void save_library();
    void scan_folder();
    void apply_media_results();
    void run_cleanup();
    void on_saved(const SaveResult& result);
    std::filesystem::path clip_path(const std::string& file) const;
    std::int64_t now_unix() const;
    void take_saved();

    // Background work (trim, filmstrip): `job` runs on the worker thread with COM ready and
    // returns what to do on the UI thread afterwards.
    void run_in_background(std::function<std::function<void()>()> job);
    void worker_main();
    void run_ui_tasks();

    // Settings.
    void change_settings(const std::function<void(Settings&)>& edit);
    void write_ini();
    void restart_recorder();
    void apply_hotkey();
    bool launch_at_startup() const;
    void set_launch_at_startup(bool on);
    void change_clips_folder();
    CleanupPolicy cleanup_policy() const;

    // Painting.
    void paint();
    void paint_title_bar(float width);
    void paint_sidebar(float height);
    void paint_library(const Rect& area);
    void paint_card(const ClipEntry& clip, const Rect& rect);
    void paint_thumbnail(const ClipEntry& clip, const Rect& rect, float radius);
    void paint_saved_bar(const Rect& area);
    void paint_placeholder_page(const Rect& area, const wchar_t* title, const wchar_t* text);
    void paint_clip_page(const Rect& area);
    void paint_player(const ClipEntry& clip, const Rect& rect);
    void paint_trim(const ClipEntry& clip, const Rect& rect);
    float paint_clip_side(const ClipEntry& clip, const Rect& column);
    void paint_record_page(const Rect& area);
    void paint_settings_page(const Rect& area);
    float paint_setting_row(const SettingRow& row, float x, float y, float w, bool last);
    float paint_hotkeys(float x, float y, float w);
    void paint_menu(float width, float height);
    // The page below the title bar scrolls when it is taller than the window; begin/end set it up.
    Rect begin_page(const Rect& area);
    void end_page(const Rect& area, float content_bottom);

    // Widgets (Widgets.cpp). Each records its hit; `disabled` draws it faded and inert.
    void nav_item(const Rect& rect, wchar_t glyph, std::wstring_view label, std::size_t count, bool selected,
                  Action action, std::string arg, std::optional<Color> dot = std::nullopt, bool soon = false);
    void button(const Rect& rect, std::wstring_view label, wchar_t glyph, Action action, std::string arg,
                bool primary = false, bool danger = false);
    float button_width(std::wstring_view label, wchar_t glyph, bool compact) const;
    // A mockup .btn: height 38 (30 when small). Returns its width.
    float push_button(float x, float y, std::wstring_view label, wchar_t glyph, ButtonStyle style, bool compact,
                      Action action, std::string arg, bool disabled = false, std::optional<Color> ink = std::nullopt);
    void toggle(float right, float cy, bool on, Action action, std::string arg, bool disabled = false);
    // Returns the width; options become hits "<arg>:<index>".
    float segmented(float right, float cy, const std::vector<std::wstring>& options, int selected, Action action,
                    const std::string& arg, bool disabled = false);
    void select_box(const Rect& rect, std::wstring_view value, Action action, std::string arg, bool disabled = false);
    // A range input: value 0..1; dragging calls on_drag(arg, fraction).
    void slider(const Rect& rect, float value, const std::string& arg, bool disabled = false, Color fill = kNoColor);
    void soon_badge(float x, float cy);
    float key_chip(float x, float y, std::wstring_view key);
    float key_chips(float x, float y, const std::string& hotkey);  // "Ctrl+Alt+F8" as chips
    void icon_button(const Rect& rect, wchar_t glyph, float size, Action action, std::string arg,
                     bool disabled = false, Color ink = kNoColor, Color hover = kNoColor);
    void add_hit(const Rect& rect, Action action, std::string arg = {});
    bool hovered(Action action, std::string_view arg = {}) const;
    void open_menu(Menu menu);
    void close_menu();

    // Input.
    void on_mouse_move(float x, float y);
    std::optional<Hit> hit_at(float x, float y) const;
    void on_press(float x, float y, bool right);
    void on_click(float x, float y, bool right);
    void run(const Hit& hit, bool right, POINT screen);
    // fraction 0..1 along the track, or < 0 when the drag ended without a position.
    void on_drag(const std::string& id, float fraction, bool done);
    void drag_clip(const std::string& id, float fraction, bool done);
    void drag_setting(const std::string& id, float fraction, bool done);
    bool on_key(WPARAM key);  // true when handled
    void on_wheel(float delta_dips);
    void more_collections_menu(const Rect& anchor);
    void clip_menu(const std::string& file, POINT screen);
    void collection_menu(const std::string& id, POINT screen);
    void add_to_collection_menu(const std::string& file, const Rect& anchor);
    void sort_menu(const Rect& anchor);
    void move_to_trash(const std::string& file);
    void recycle_now(const std::string& file);
    // Shows the inline text box over the collection name or clip title being renamed.
    void begin_edit(Editing editing, const std::wstring& text);
    void end_edit(bool commit);
    void layout_search_box();
    void update_edit_font();
    // Child edit controls of the pages: placed after each paint, hidden when not on screen.
    void place_control(HWND control, const Rect& rect, bool show);
    void hide_page_controls();

    // Pages.
    void set_page(Page page);
    void run_clip(const std::string& what, const Rect& anchor);
    void run_record(const std::string& what, const Rect& anchor);
    void run_setting(const std::string& what, const Rect& anchor);
    void open_clip(const std::string& file);
    void close_clip();
    void commit_clip_text();
    void make_player();
    void tick_player();
    void save_trim();
    void open_export();
    void close_export();  // cancels a running export
    void run_export(const std::string& what, const Rect& anchor);
    void paint_export(float width, float height);
    std::vector<std::string> clip_order() const;  // the files of the library view the clip page came from
    void request_filmstrip();
    std::vector<SettingRow> setting_rows();
    void capture_hotkey(WPARAM key);
    void cancel_hotkey_capture();
    void sample_load();  // Backtrack's own CPU and memory use, for the Record page
    void toggle_mic_mute();
    // Save and mute keys arrive two ways: as WM_HOTKEY, and from the raw keyboard input, which a
    // game that grabs the keyboard (DirectInput exclusive, e.g. GTA V borderless) cannot switch
    // off. The first of the two acts; the other, within a moment, is the same press.
    void on_shortcut(int id);
    void on_raw_key(const RAWKEYBOARD& key);
    void unregister_hotkeys();
    static std::string& hotkey_text(Settings& s, HotkeyTarget target);
    static const std::string& hotkey_text(const Settings& s, HotkeyTarget target);
    void toggle_overlay();
    void add_bookmark();
    void refresh_overlay();  // what the in-screen chip shows, and where
    void flash_overlay(std::wstring text, Color dot);
    RECT recorded_monitor_rect() const;  // in screen pixels
    void apply_audio_live();  // volumes, noise suppression, push-to-talk: no restart needed
    void refresh_devices();  // monitors_ and microphones_
    float hotkey_row(float x, float y, float w, HotkeyTarget target, const wchar_t* label, const wchar_t* description,
                     const std::string& keys, const std::string& default_keys, bool registered);

    // Tray.
    void add_tray_icon();
    void remove_tray_icon();
    void tray_menu();
    void notify_saved(const SaveResult& result);

    std::wstring view_title() const;
    int recorded_display() const;  // the monitor index Instant Replay really records
    LibraryQuery current_query() const;
    void set_view(LibraryView view, std::string collection = {});
    void refresh_status();
    void register_hotkey();
    float dip(int px) const { return static_cast<float>(px) / graphics_->scale(); }

    Settings settings_;
    Settings recorder_settings_;  // what the running recorder was made with
    const AppPaths paths_;
    RecorderFactory make_recorder_;
    std::unique_ptr<Recorder> recorder_;
    SavedInbox& inbox_;
    Updater* updater_ = nullptr;
    bool session_ended_ = false;  // WM_ENDSESSION: logoff, shutdown or an installer closed us
    bool update_started_ = false;  // "Install now" started the installer

    HWND window_ = nullptr;
    HWND search_box_ = nullptr;
    HWND edit_box_ = nullptr;
    HWND notes_box_ = nullptr;  // clip page
    HWND tag_box_ = nullptr;  // clip page "Add tag"
    HWND settings_search_ = nullptr;
    HFONT edit_font_ = nullptr;
    HFONT title_font_ = nullptr;  // the clip page title while it is renamed
    HBRUSH edit_brush_ = nullptr;  // --raise
    HBRUSH bg_brush_ = nullptr;  // --bg
    HBRUSH card_brush_ = nullptr;  // --card
    HBRUSH field_brush_ = nullptr;  // .search on --bg
    HICON icon_big_ = nullptr;
    HICON icon_small_ = nullptr;
    UINT taskbar_created_ = 0;
    UINT show_message_ = 0;
    bool tray_added_ = false;
    bool hotkey_ok_[kGlobalHotkeys]{};  // by HotkeyTarget: Windows accepted the key
    bool quitting_ = false;
    bool finished_ = false;
    bool tracking_mouse_ = false;
    bool max_hovered_ = false;  // the maximize button is hovered through WM_NCHITTEST (snap layouts)
    bool notified_hidden_ = false;  // told once per run that closing keeps Backtrack in the tray

    std::unique_ptr<Graphics> graphics_;
    std::unique_ptr<MediaProbe> probe_;
    std::unique_ptr<Overlay> overlay_;
    // Bookmark presses on the recording clock, kept as long as the replay buffer reaches back.
    std::deque<Time100ns> bookmarks_;

    Library library_;
    std::set<std::string> new_files_;  // saved during this run and not opened yet: "New" badge
    std::set<std::string> probing_;  // "file|size" requested from the probe, result not back yet
    std::set<std::string> thumbs_missing_;  // "file|size" probed, no thumbnail could be made
    std::map<std::wstring, std::vector<std::uint8_t>> pending_thumbs_;  // waiting for upload at the next paint
    std::map<std::wstring, std::uint64_t> bitmap_use_;  // thumbnail key -> frame it was last drawn in
    std::vector<std::pair<std::string, std::int64_t>> to_request_;  // thumbnails cards asked for this frame
    std::uint64_t frame_ = 0;
    std::chrono::steady_clock::time_point next_scan_{};
    std::chrono::steady_clock::time_point next_cleanup_{};

    Page page_ = Page::Library;
    LibraryView view_ = LibraryView::All;
    std::string view_collection_;
    ClipSort sort_ = ClipSort::Newest;
    std::string search_;
    float scroll_ = 0.0f;
    float max_scroll_ = 0.0f;
    float page_scroll_ = 0.0f;  // the other pages
    float page_max_scroll_ = 0.0f;
    Rect page_view_;  // the visible part of the current page
    Rect grid_area_;
    Rect search_rect_;
    Rect edit_rect_;
    bool edit_positioned_ = false;
    std::optional<SavedBar> saved_bar_;
    std::optional<Editing> editing_;
    std::string status_;

    std::vector<Hit> hits_;
    std::optional<Hit> hover_;
    std::optional<Hit> pressed_;  // what the mouse went down on; a click needs the same on release
    std::optional<std::string> dragging_;  // the Drag hit being dragged
    std::map<std::string, Rect> tracks_;  // where each draggable thing's track is, this frame
    std::optional<Menu> menu_;
    std::map<HWND, std::pair<Rect, bool>> controls_;  // where each page edit control should be
    std::vector<std::size_t> hidden_collections_;  // collection indices behind "N more"
    std::set<std::string> urgent_;  // "file|size" asked for with priority, result not back yet
    std::wstring notice_;  // a short problem report in the header bar (failed recycle)
    std::chrono::steady_clock::time_point notice_until_{};
    bool library_writable_ = true;  // false when library.json exists but could not be read
    bool max_pressed_ = false;  // the maximize button was pressed; releasing on it toggles
    bool recycling_ = false;  // shell::recycle is waiting (it keeps the window responsive meanwhile)
    std::set<std::string> cleanup_skipped_;  // could not be removed this run; not retried hourly
    float mouse_x_ = -1.0f;
    float mouse_y_ = -1.0f;

    // Clip page.
    std::optional<ClipView> clip_view_;
    std::shared_ptr<ExportView> export_;  // the Export window, while it is open
    std::string clip_title_drawn_;  // the title the side panel shows, to notice a rename elsewhere
    bool trimming_ = false;
    LPARAM next_player_tag_ = 1;
    bool shell_busy_ = false;
    bool quit_pending_ = false;  // Quit chosen while a shell operation waits: done when it returns  // a folder move or picker is running and pumping messages: no scans, no second one

    // Settings page.
    std::string settings_section_ = "general";
    std::string settings_query_;
    bool capturing_hotkey_ = false;
    HotkeyTarget capture_target_ = HotkeyTarget::Save;  // which key "press keys" is listening for
    HotkeyTarget message_target_ = HotkeyTarget::Save;  // the row hotkey_message_ belongs to
    struct RawKey {
        std::chrono::steady_clock::time_point since;  // when it went down
        std::chrono::steady_clock::time_point last;  // its last "down" (auto-repeat refreshes it)
        std::uint32_t modifier = 0;  // hotkey_mod bit for Ctrl/Alt/Shift/Win, else 0
    };
    std::map<std::uint32_t, RawKey> raw_keys_down_;  // physical keys down, from the raw input
    std::optional<Hotkey> registered_keys_[kGlobalHotkeys];  // parsed once, for the raw input path
    std::chrono::steady_clock::time_point last_shortcut_[kGlobalHotkeys + 1]{};  // by hotkey id: a press seen both ways acts once
    std::vector<MicDevice> microphones_;  // refreshed when the Record or Settings page opens
    std::vector<PlaybackDevice> playback_devices_;  // the same, for the system audio source
    std::wstring hotkey_message_;  // why the last pressed combination cannot be used
    std::optional<std::chrono::steady_clock::time_point> restart_due_;  // settings wait a moment, then restart once
    std::string ini_problem_;  // the last write of backtrack.ini failed
    std::vector<MonitorInfo> monitors_;  // refreshed when the Record or Settings page opens
    std::optional<std::pair<std::string, float>> drag_preview_;  // a settings slider being dragged: applied on release
    std::int64_t cache_bytes_ = -1;  // thumbnail cache size, measured when the Storage section opens

    // Record page: Backtrack's own load, sampled once a second.
    std::deque<float> cpu_history_;
    std::deque<float> memory_history_;
    std::uint64_t last_cpu_time_ = 0;
    std::chrono::steady_clock::time_point last_cpu_sample_{};

    // Background worker.
    std::atomic<HWND> post_window_{nullptr};  // what the worker posts to; never changes once the window exists
    std::thread worker_;
    std::mutex worker_mutex_;
    std::condition_variable worker_wake_;
    std::deque<std::function<std::function<void()>()>> jobs_;
    std::vector<std::function<void()>> ui_tasks_;
    bool stop_worker_ = false;
};

}  // namespace bt::ui
