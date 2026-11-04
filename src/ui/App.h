#pragma once

// The Backtrack window (design: the "01 Breathe" mockup): our own title bar, a sidebar with the
// library views and collections, the clip grid and the "Saved" bar. Closing the window hides it to
// the tray icon; Instant Replay keeps running until "Quit" in the tray menu.
//
// Drawing is immediate mode: paint() lays out and draws everything and records where each button
// is (hits_), the mouse handlers look those up. No retained widget tree to keep in sync.
//
// UI thread only, except SavedInbox::push (the recorder's writer thread) and the MediaProbe
// ready callback, which only post messages.

#include "core/Config.h"
#include "core/Library.h"
#include "platform/MediaProbe.h"
#include "platform/Recorder.h"
#include "ui/Graphics.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
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

// Saved clips travel from the recorder's writer thread to the window through here.
class SavedInbox {
public:
    void attach(HWND window);
    void push(const SaveResult& result);
    std::vector<SaveResult> take();

private:
    std::mutex mutex_;
    std::vector<SaveResult> results_;
    HWND window_ = nullptr;
};

class App {
public:
    // The window class name: a second Backtrack finds the running one by it.
    static constexpr wchar_t kWindowClass[] = L"Backtrack.Window";
    // Registered message a second copy sends to bring this window forward.
    static constexpr wchar_t kShowMessage[] = L"Backtrack.Show";

    App(const Settings& settings, const AppPaths& paths, Recorder& recorder, SavedInbox& inbox);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    HWND window() const { return window_; }
    // SW_HIDE leaves it in the tray; a minimised start stays minimised without taking focus.
    void show(int show_command);

    // Also used by the test options.
    void save_clip();
    void toggle_pause();
    void quit();
    // After the recorder has stopped: files the clips it finished meanwhile and saves the library.
    void finish();

private:
    enum class Page { Library, Record, Uploads, Settings };

    enum class Action {
        None,
        Minimize, Maximize, Close,
        View,  // arg: "all", "favorites", "recent", "trash" or a collection id
        NewCollection,
        Page,  // arg: "record", "uploads", "settings"
        SaveNow, TogglePause,
        Sort,
        OpenClip, ClipMenu, ToggleFavorite,  // arg: file
        SavedOpen, SavedFolder, SavedTrash, SavedClose,
        CollectionMenu,  // arg: id
        MoreCollections,
        Search,
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
        std::chrono::steady_clock::time_point at;
    };

    struct Editing {
        enum class Kind { CollectionName, ClipTitle } kind;
        std::string target;  // collection id or clip file
    };

    void construct();
    void release();  // everything the window owns; also when construct() failed halfway
    void invalidate();
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

    // Painting.
    void paint();
    void paint_title_bar(float width);
    void paint_sidebar(float height);
    void paint_library(const Rect& area);
    void paint_card(const ClipEntry& clip, const Rect& rect);
    void paint_thumbnail(const ClipEntry& clip, const Rect& rect, float radius);
    void paint_saved_bar(const Rect& area);
    void paint_placeholder_page(const Rect& area, const wchar_t* title, const wchar_t* text);
    void nav_item(const Rect& rect, wchar_t glyph, std::wstring_view label, std::size_t count, bool selected,
                  Action action, std::string arg, std::optional<Color> dot = std::nullopt, bool soon = false);
    void button(const Rect& rect, std::wstring_view label, wchar_t glyph, Action action, std::string arg,
                bool primary = false, bool danger = false);
    float key_chip(float x, float y, std::wstring_view key);
    void add_hit(const Rect& rect, Action action, std::string arg = {});
    bool hovered(Action action, std::string_view arg = {}) const;

    // Input.
    void on_mouse_move(float x, float y);
    std::optional<Hit> hit_at(float x, float y) const;
    void on_press(float x, float y);
    void on_click(float x, float y, bool right);
    void more_collections_menu(POINT screen);
    void run(const Hit& hit, bool right, POINT screen);
    void clip_menu(const std::string& file, POINT screen);
    void collection_menu(const std::string& id, POINT screen);
    void sort_menu(POINT screen);
    // Shows the inline text box over the collection name or card title being renamed.
    void begin_edit(Editing editing, const std::wstring& text);
    void end_edit(bool commit);
    void layout_search_box();
    void update_edit_font();

    // Tray.
    void add_tray_icon();
    void remove_tray_icon();
    void tray_menu();
    void notify_saved(const SaveResult& result);

    std::wstring view_title() const;
    LibraryQuery current_query() const;
    void set_view(LibraryView view, std::string collection = {});
    void refresh_status();
    void register_hotkey();
    float dip(int px) const { return static_cast<float>(px) / graphics_->scale(); }

    const Settings settings_;
    const AppPaths paths_;
    Recorder& recorder_;
    SavedInbox& inbox_;

    HWND window_ = nullptr;
    HWND search_box_ = nullptr;
    HWND edit_box_ = nullptr;
    HFONT edit_font_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    HICON icon_big_ = nullptr;
    HICON icon_small_ = nullptr;
    UINT taskbar_created_ = 0;
    UINT show_message_ = 0;
    bool tray_added_ = false;
    bool hotkey_registered_ = false;
    bool quitting_ = false;
    bool tracking_mouse_ = false;
    bool max_hovered_ = false;  // the maximize button is hovered through WM_NCHITTEST (snap layouts)
    bool notified_hidden_ = false;  // told once per run that closing keeps Backtrack in the tray

    std::unique_ptr<Graphics> graphics_;
    std::unique_ptr<MediaProbe> probe_;

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
};

}  // namespace bt::ui
