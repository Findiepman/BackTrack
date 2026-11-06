#include "ui/App.h"

#include "core/Hotkey.h"
#include "platform/Log.h"
#include "platform/Shell.h"
#include "platform/Text.h"
#include "ui/AppIds.h"
#include "ui/Icon.h"
#include "ui/Theme.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

namespace bt::ui {
namespace {

namespace fs = std::filesystem;
using namespace theme;

using namespace ids;
constexpr UINT_PTR kMeterTimer = 3;
constexpr UINT kMeterMs = 66;  // the Record page's sound meters, about 15 times a second
constexpr UINT kTrayId = 1;
constexpr int kSearchBoxId = 101;
constexpr int kEditBoxId = 102;
constexpr int kNotesBoxId = 103;
constexpr int kTagBoxId = 104;
constexpr int kSettingsSearchId = 105;

constexpr auto kScanInterval = std::chrono::seconds(3);
constexpr auto kCleanupInterval = std::chrono::hours(1);
constexpr float kWheelStep = 120.0f;  // DIPs per wheel notch
constexpr float kResizeBorder = 6.0f;  // DIPs, where the edge of the window resizes it
constexpr float kMinWidth = 960.0f;
constexpr float kMinHeight = 600.0f;
constexpr float kStartWidth = 1280.0f;
constexpr float kStartHeight = 800.0f;
constexpr std::size_t kMaxBitmaps = 240;  // thumbnails kept on the GPU (~80 MB at 384x216)
constexpr auto kNoticeTime = std::chrono::seconds(8);

// Tray menu command ids (the tray menu is the one Windows menu left: it opens outside the window).
enum MenuId : UINT {
    kTrayOpen = 4000, kTraySave, kTrayPause, kTrayFolder, kTraySettings, kTrayLog, kTrayQuit, kTrayMute,
};

struct SortName {
    ClipSort sort;
    const wchar_t* label;
};
constexpr SortName kSortNames[] = {
    {ClipSort::Newest, L"Newest"},   {ClipSort::Oldest, L"Oldest"},   {ClipSort::Longest, L"Longest"},
    {ClipSort::Shortest, L"Shortest"}, {ClipSort::Largest, L"Largest"}, {ClipSort::Name, L"Name"},
};

const wchar_t* sort_label(ClipSort sort) {
    for (const SortName& s : kSortNames) {
        if (s.sort == sort) return s.label;
    }
    return L"Newest";
}

COLORREF colorref(Color c) {
    return RGB(static_cast<int>(std::lround(c.r * 255)), static_cast<int>(std::lround(c.g * 255)),
               static_cast<int>(std::lround(c.b * 255)));
}

std::string path_text(const fs::path& path) {
    return to_utf8(path.wstring());
}

std::int64_t unix_from_filetime(const FILETIME& time) {
    constexpr std::int64_t kFileTimeTicksPerSecond = 10'000'000;  // FILETIME counts 100 ns
    constexpr std::int64_t kEpochDifference = 11'644'473'600;  // 1601 to 1970, seconds
    const std::int64_t ticks =
        (static_cast<std::int64_t>(time.dwHighDateTime) << 32) | static_cast<std::int64_t>(time.dwLowDateTime);
    return ticks / kFileTimeTicksPerSecond - kEpochDifference;
}

std::wstring thumb_key(const ClipEntry& clip) {
    return to_wide(clip.file) + L"|" + std::to_wstring(clip.size_bytes);
}

std::string probe_key(const std::string& file, std::int64_t size) {
    return file + "|" + std::to_string(size);
}

std::wstring window_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(std::max(length, 0)) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(std::max(length, 0)));
    return text;
}

// Windows' own case rules for search, and its sort order for names (accented letters next to the
// plain ones). Installed into core/Library at startup.
std::string fold_text(std::string_view text) {
    const std::wstring wide = to_wide(text);
    if (wide.empty()) return {};
    const int length = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_LOWERCASE | LCMAP_LINGUISTIC_CASING, wide.c_str(),
                                     static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr, 0);
    if (length <= 0) return std::string(text);
    std::wstring lowered(static_cast<std::size_t>(length), L'\0');
    LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_LOWERCASE | LCMAP_LINGUISTIC_CASING, wide.c_str(),
                  static_cast<int>(wide.size()), lowered.data(), length, nullptr, nullptr, 0);
    return to_utf8(lowered);
}

int compare_text(std::string_view a, std::string_view b) {
    const std::wstring x = to_wide(a);
    const std::wstring y = to_wide(b);
    const int result = CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, x.c_str(),
                                       static_cast<int>(x.size()), y.c_str(), static_cast<int>(y.size()), nullptr,
                                       nullptr, 0);
    if (result == 0) return x < y ? -1 : x > y ? 1 : 0;  // the call failed: plain order
    return result - CSTR_EQUAL;
}

// Enter and Escape in the inline edit box (rename) and the search box: the owner decides.
LRESULT CALLBACK edit_subclass(HWND edit, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
    if (message == WM_GETDLGCODE) {
        return DefSubclassProc(edit, message, wparam, lparam) | DLGC_WANTALLKEYS;
    }
    if (message == WM_KEYDOWN && (wparam == VK_RETURN || wparam == VK_ESCAPE)) {
        SendMessageW(GetParent(edit), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(edit), wparam == VK_RETURN ? IDOK : IDCANCEL),
                     reinterpret_cast<LPARAM>(edit));
        return 0;
    }
    if (message == WM_CHAR && (wparam == L'\r' || wparam == 27)) {
        return 0;  // no beep
    }
    return DefSubclassProc(edit, message, wparam, lparam);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// SavedInbox

void SavedInbox::attach(HWND window) {
    std::lock_guard lock(mutex_);
    window_ = window;
}

void SavedInbox::push(const SaveResult& result) {
    // §8: without the window in front of a fullscreen game, sound is the only feedback: the
    // "asterisk" for a saved clip, the "critical stop" for a failed one.
    if (sound_) {
        PlaySoundW(result.ok ? L"SystemAsterisk" : L"SystemHand", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
    }
    std::lock_guard lock(mutex_);
    results_.push_back(result);
    if (window_ != nullptr) {
        PostMessageW(window_, kMsgSaved, 0, 0);
    }
}

std::vector<SaveResult> SavedInbox::take() {
    std::lock_guard lock(mutex_);
    return std::exchange(results_, {});
}

// ---------------------------------------------------------------------------------------------
// Window

App::App(const Settings& settings, const AppPaths& paths, RecorderFactory make_recorder, SavedInbox& inbox,
         Updater* updater)
    : settings_(settings), paths_(paths), make_recorder_(std::move(make_recorder)), inbox_(inbox), updater_(updater) {
    try {
        construct();
    } catch (...) {
        // The window already points at this object; it must not outlive it.
        release();
        throw;
    }
}

void App::construct() {
    inbox_.set_sound(settings_.sound);
    recorder_ = make_recorder_(settings_);
    recorder_->start();
    recorder_settings_ = settings_;
    apply_audio_live();  // the push-to-talk key is parsed here
    worker_ = std::thread([this] { worker_main(); });

    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    show_message_ = RegisterWindowMessageW(kShowMessage);

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    icon_big_ = make_app_icon(GetSystemMetrics(SM_CXICON));
    icon_small_ = make_app_icon(GetSystemMetrics(SM_CXSMICON));

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = 0;  // no CS_DBLCLKS: a double-click is two clicks, never a third action
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = icon_big_;
    window_class.hIconSm = icon_small_;
    window_class.lpszClassName = kWindowClass;
    BT_CHECK_WIN32(RegisterClassExW(&window_class) != 0);

    // Created at 96 DPI size, then resized for the monitor it lands on (WM_DPICHANGED is not
    // sent for the first placement).
    window_ = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, L"Backtrack", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, static_cast<int>(kStartWidth),
                              static_cast<int>(kStartHeight), nullptr, nullptr, instance, this);
    BT_CHECK_WIN32(window_ != nullptr);
    post_window_ = window_;
    graphics_ = std::make_unique<Graphics>(window_);
    const float scale = graphics_->scale();
    const MONITORINFO monitor = [this] {
        MONITORINFO info{sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTOPRIMARY), &info);
        return info;
    }();
    const int width = std::min(static_cast<int>(kStartWidth * scale),
                               static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
    const int height = std::min(static_cast<int>(kStartHeight * scale),
                                static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
    SetWindowPos(window_, nullptr, monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2,
                 monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    // Our own title bar: keep the DWM shadow, rounded corners on Windows 11 and a dark border.
    const MARGINS shadow{1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(window_, &shadow);
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    // Explorer's "taskbar is back" broadcast and the second copy's "show yourself" must get through
    // even when Backtrack runs elevated (UIPI would drop them).
    if (taskbar_created_ != 0) ChangeWindowMessageFilterEx(window_, taskbar_created_, MSGFLT_ALLOW, nullptr);
    if (show_message_ != 0) ChangeWindowMessageFilterEx(window_, show_message_, MSGFLT_ALLOW, nullptr);

    apply_look();
    search_box_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, window_,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSearchBoxId)), instance, nullptr);
    edit_box_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, window_,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditBoxId)), instance, nullptr);
    notes_box_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 0, 0,
                                 10, 10, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNotesBoxId)), instance,
                                 nullptr);
    tag_box_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, window_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTagBoxId)), instance, nullptr);
    settings_search_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, window_,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsSearchId)), instance, nullptr);
    SetWindowSubclass(search_box_, edit_subclass, 1, 0);
    SetWindowSubclass(edit_box_, edit_subclass, 1, 0);
    SetWindowSubclass(tag_box_, edit_subclass, 1, 0);
    SetWindowSubclass(settings_search_, edit_subclass, 1, 0);
    SendMessageW(tag_box_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Add tag"));
    SendMessageW(tag_box_, EM_SETLIMITTEXT, 40, 0);
    SendMessageW(notes_box_, EM_SETLIMITTEXT, 4000, 0);
    SendMessageW(settings_search_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search settings"));
    SendMessageW(settings_search_, EM_SETLIMITTEXT, 60, 0);
    SendMessageW(search_box_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search clips"));
    SendMessageW(search_box_, EM_SETLIMITTEXT, 200, 0);
    SendMessageW(edit_box_, EM_SETLIMITTEXT, 200, 0);
    update_edit_font();

    probe_ = std::make_unique<MediaProbe>(paths_.cache, [window = window_] { PostMessageW(window, kMsgProbe, 0, 0); });
    set_text_rules(TextRules{&fold_text, &compare_text});
    load_library();
    scan_folder();
    request_missing_info();
    run_cleanup();
    next_cleanup_ = std::chrono::steady_clock::now() + kCleanupInterval;

    register_hotkey();
    {
        // INPUTSINK: also while another window (a game) is in front.
        RAWINPUTDEVICE keyboard{};
        keyboard.usUsagePage = 0x01;  // generic desktop
        keyboard.usUsage = 0x06;  // keyboard
        keyboard.dwFlags = RIDEV_INPUTSINK;
        keyboard.hwndTarget = window_;
        if (!RegisterRawInputDevices(&keyboard, 1, sizeof(keyboard))) {
            log::warn("Cannot read the keyboard directly ({}); the save key may not work in some games",
                      hresult_text(HRESULT_FROM_WIN32(GetLastError())));
        }
    }
    add_tray_icon();
    if (monitors_.empty()) refresh_devices();  // the overlay sits on the recorded display
    overlay_ = std::make_unique<Overlay>();
    refresh_status();
    refresh_overlay();
    if (updater_ != nullptr) updater_->notify(window_, kMsgUpdate);
    inbox_.attach(window_);
    SetTimer(window_, kTickTimer, kTickMs, nullptr);
    // A clip saved before the inbox knew the window (the recorder starts first) is picked up now.
    PostMessageW(window_, kMsgSaved, 0, 0);
}

App::~App() {
    if (window_ != nullptr && IsWindow(window_)) {
        // Unwinding past a window that is still open (an error in the message loop). Tidied up in
        // release(), not by WM_DESTROY: its PostQuitMessage would close the error message box.
        save_library();
    }
    release();
}

void App::apply_look() {
    theme::apply_theme(settings_.theme);
    // The edit boxes paint with GDI brushes in the theme's colours (WM_CTLCOLOREDIT).
    for (HBRUSH* brush : {&edit_brush_, &bg_brush_, &card_brush_, &field_brush_}) {
        if (*brush != nullptr) DeleteObject(*brush);
    }
    edit_brush_ = CreateSolidBrush(colorref(kRaise));
    bg_brush_ = CreateSolidBrush(colorref(kBg));
    card_brush_ = CreateSolidBrush(colorref(kCard));
    field_brush_ = CreateSolidBrush(colorref(kInk.mix(kBg, 0.05f)));
    // The frame Windows draws around our title bar: dark or light, with the theme's line colour.
    const BOOL dark = kLightTheme ? FALSE : TRUE;
    DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const COLORREF border = colorref(kLine);
    DwmSetWindowAttribute(window_, DWMWA_BORDER_COLOR, &border, sizeof(border));
    for (HWND box : {search_box_, edit_box_, notes_box_, tag_box_, settings_search_}) {
        if (box != nullptr) InvalidateRect(box, nullptr, TRUE);
    }
    invalidate();
}

void App::worker_main() {
    // Trims and filmstrips use Media Foundation, which wants COM on the thread.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (true) {
        std::function<std::function<void()>()> job;
        {
            std::unique_lock lock(worker_mutex_);
            worker_wake_.wait(lock, [this] { return stop_worker_ || !jobs_.empty(); });
            if (jobs_.empty()) break;  // stopping, and nothing left to finish
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        std::function<void()> then;
        try {
            then = job();
        } catch (const std::exception& e) {
            log::error("Background job failed: {}", e.what());
        } catch (...) {
            log::error("Background job failed");
        }
        std::lock_guard lock(worker_mutex_);
        if (then) ui_tasks_.push_back(std::move(then));
        const HWND target = post_window_.load();
        if (target != nullptr && !stop_worker_) PostMessageW(target, kMsgWork, 0, 0);
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

void App::run_in_background(std::function<std::function<void()>()> job) {
    {
        std::lock_guard lock(worker_mutex_);
        if (stop_worker_) return;
        jobs_.push_back(std::move(job));
    }
    worker_wake_.notify_one();
}

void App::run_ui_tasks() {
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard lock(worker_mutex_);
        tasks.swap(ui_tasks_);
    }
    for (auto& task : tasks) {
        if (window_ == nullptr) return;  // closing: what they would update is gone
        try {
            task();  // one failing must not drop the others (a trim's "done" resets trimming_)
        } catch (const std::exception& e) {
            log::error("Finishing background work failed: {}", e.what());
        }
    }
}

void App::release() {
    if (export_) export_->cancel->store(true);  // stops within a second; the worker is joined below
    if (share_) share_->cancel->store(true);
    {
        // A trim that is being written finishes (never a half file); queued jobs are dropped.
        std::lock_guard lock(worker_mutex_);
        stop_worker_ = true;
        jobs_.clear();
    }
    worker_wake_.notify_one();
    if (worker_.joinable()) worker_.join();
    post_window_ = nullptr;
    ui_tasks_.clear();
    clip_view_.reset();  // the player decodes on the window's graphics device
    if (window_ != nullptr && IsWindow(window_)) {
        unregister_hotkeys();
        // The raw keyboard registration points at this window; let it go with it.
        RAWINPUTDEVICE keyboard{0x01, 0x06, RIDEV_REMOVE, nullptr};
        RegisterRawInputDevices(&keyboard, 1, sizeof(keyboard));
        remove_tray_icon();
    }
    inbox_.attach(nullptr);
    if (updater_ != nullptr) updater_->notify(nullptr, 0);
    overlay_.reset();
    probe_.reset();  // its thread posts to the window; stop it first
    if (window_ != nullptr && IsWindow(window_)) {
        KillTimer(window_, kTickTimer);
        KillTimer(window_, kPlayerTimer);
        SetWindowLongPtrW(window_, GWLP_USERDATA, 0);  // DestroyWindow below goes to DefWindowProc
        DestroyWindow(window_);
    }
    window_ = nullptr;
    if (edit_font_ != nullptr) DeleteObject(edit_font_);
    if (edit_brush_ != nullptr) DeleteObject(edit_brush_);
    if (bg_brush_ != nullptr) DeleteObject(bg_brush_);
    if (card_brush_ != nullptr) DeleteObject(card_brush_);
    if (field_brush_ != nullptr) DeleteObject(field_brush_);
    if (title_font_ != nullptr) DeleteObject(title_font_);
    bg_brush_ = nullptr;
    card_brush_ = nullptr;
    field_brush_ = nullptr;
    title_font_ = nullptr;
    if (icon_big_ != nullptr) DestroyIcon(icon_big_);
    if (icon_small_ != nullptr) DestroyIcon(icon_small_);
    edit_font_ = nullptr;
    edit_brush_ = nullptr;
    icon_big_ = nullptr;
    icon_small_ = nullptr;
    UnregisterClassW(kWindowClass, GetModuleHandleW(nullptr));
    graphics_.reset();
    // Unwinding (an error): the recorder still finishes the clip it is writing.
    recorder_.reset();
}

void App::finish() {
    if (finished_) return;
    finished_ = true;
    if (recorder_->saving()) log::info("Finishing the clip that is being saved...");
    recorder_->stop();  // §6: every queued clip is written before Backtrack ends
    // The clip that was still being written when Quit was chosen goes into the library too.
    take_saved();
    save_library();
    refresh_overlay();  // Instant Replay is off now: the chip goes
}

void App::take_saved() {
    for (const SaveResult& result : inbox_.take()) {
        on_saved(result);
    }
}

void App::invalidate() {
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void App::request_missing_info() {
    if (recorder_->output_dir_is_fallback() || window_ == nullptr) return;
    // Straight over the clips: find() per name would be quadratic with thousands of new clips.
    for (const ClipEntry& clip : library_.clips()) {
        if (clip.duration_s <= 0.0 && probing_.insert(probe_key(clip.file, clip.size_bytes)).second) {
            probe_->request(recorder_->output_dir(), clip.file, clip.size_bytes, true);
        }
    }
}

void App::show_notice(std::wstring text) {
    notice_ = std::move(text);
    notice_until_ = std::chrono::steady_clock::now() + kNoticeTime;
    invalidate();
}

void App::show(int show_command) {
    if (show_command == SW_HIDE) {
        return;  // started "minimized to the tray"
    }
    ShowWindow(window_, show_command == SW_SHOWMINIMIZED || show_command == SW_MINIMIZE ||
                                show_command == SW_SHOWMINNOACTIVE
                            ? SW_SHOWMINNOACTIVE
                            : show_command);
    if (IsIconic(window_) && show_command == SW_SHOW) {
        ShowWindow(window_, SW_RESTORE);
    }
    scan_folder();
    invalidate();
}

void App::quit() {
    if (shell_busy_ || recycling_) {
        // A folder move or a Recycle Bin call is waiting on this thread: quit once it returns.
        quit_pending_ = true;
        return;
    }
    quitting_ = true;
    if (editing_) end_edit(true);
    commit_clip_text();
    DestroyWindow(window_);
}

LRESULT CALLBACK App::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    App* app = nullptr;
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->window_ = window;
    } else {
        app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    if (app == nullptr || app->graphics_ == nullptr) {
        // Before the constructor finished: only the frame matters (no frame drawn by Windows).
        if (message == WM_NCCALCSIZE && wparam == TRUE) return 0;
        return DefWindowProcW(window, message, wparam, lparam);
    }
    try {
        return app->handle(message, wparam, lparam);
    } catch (const std::exception& e) {
        // §9: an error in one click must not take the recorder down with it.
        log::error("Window error: {}", e.what());
        return DefWindowProcW(window, message, wparam, lparam);
    } catch (...) {
        log::error("Window error");
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

LRESULT App::handle(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == taskbar_created_ && taskbar_created_ != 0) {
        tray_added_ = false;  // Explorer restarted: the icon is gone
        add_tray_icon();
        return 0;
    }
    if (message == show_message_ && show_message_ != 0) {
        log::info("Backtrack was started again; showing the window");
        show(SW_SHOW);
        SetForegroundWindow(window_);
        return 0;
    }

    switch (message) {
        case WM_NCCALCSIZE:
            if (wparam == TRUE) {
                // No Windows frame: the client area is the whole window. Maximised, Windows puts the
                // window partly off screen by the frame width; take that back so nothing is cut off.
                if (IsZoomed(window_)) {
                    auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
                    const UINT dpi = GetDpiForWindow(window_);
                    const int frame_x = GetSystemMetricsForDpi(SM_CXFRAME, dpi) +
                                        GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                    const int frame_y = GetSystemMetricsForDpi(SM_CYFRAME, dpi) +
                                        GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                    params->rgrc[0].left += frame_x;
                    params->rgrc[0].right -= frame_x;
                    params->rgrc[0].top += frame_y;
                    params->rgrc[0].bottom -= frame_y;
                }
                return 0;
            }
            break;
        case WM_NCHITTEST: return hit_test(POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        case WM_NCMOUSEMOVE:
            if (wparam == HTMAXBUTTON) {
                if (!max_hovered_) {
                    max_hovered_ = true;
                    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE | TME_NONCLIENT, window_, 0};
                    TrackMouseEvent(&track);
                    invalidate();
                }
            } else if (max_hovered_) {
                max_hovered_ = false;
                invalidate();
            }
            break;
        case WM_NCMOUSELEAVE:
            max_pressed_ = false;
            if (max_hovered_) {
                max_hovered_ = false;
                invalidate();
            }
            break;
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONDBLCLK:  // sent for the non-client area whatever the class style
            if (wparam == HTMAXBUTTON) {
                max_pressed_ = true;  // and no DefWindowProc: it would draw its own button over ours
                return 0;
            }
            max_pressed_ = false;
            break;
        case WM_NCLBUTTONUP:
            if (wparam == HTMAXBUTTON) {
                if (std::exchange(max_pressed_, false)) {
                    ShowWindow(window_, IsZoomed(window_) ? SW_RESTORE : SW_MAXIMIZE);
                }
                return 0;
            }
            max_pressed_ = false;
            break;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
            MONITORINFO monitor{sizeof(MONITORINFO)};
            GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
            const LONG work_w = monitor.rcWork.right - monitor.rcWork.left;
            const LONG work_h = monitor.rcWork.bottom - monitor.rcWork.top;
            info->ptMinTrackSize = POINT{std::min(static_cast<LONG>(kMinWidth * scale), work_w),
                                         std::min(static_cast<LONG>(kMinHeight * scale), work_h)};
            return 0;
        }
        case WM_SIZE:
            graphics_->resize(LOWORD(lparam), HIWORD(lparam));
            layout_search_box();
            if (editing_) end_edit(true);
            close_menu();
            invalidate();
            return 0;
        case WM_DPICHANGED: {
            graphics_->set_dpi(HIWORD(wparam));
            update_edit_font();
            const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(window_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(window_, &ps);
            struct EndPaintGuard {
                HWND window;
                PAINTSTRUCT* ps;
                ~EndPaintGuard() { EndPaint(window, ps); }
            } guard{window_, &ps};
            paint();
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!tracking_mouse_) {
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
                tracking_mouse_ = TrackMouseEvent(&track) != FALSE;
            }
            max_pressed_ = false;
            if (max_hovered_) {
                max_hovered_ = false;
                invalidate();
            }
            on_mouse_move(dip(GET_X_LPARAM(lparam)), dip(GET_Y_LPARAM(lparam)));
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_mouse_ = false;
            on_mouse_move(-1, -1);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
            cancel_hotkey_capture();  // clicking anywhere ends "press keys" (clicking the button starts it again)
            SetFocus(window_);
            on_press(dip(GET_X_LPARAM(lparam)), dip(GET_Y_LPARAM(lparam)), message == WM_RBUTTONDOWN);
            return 0;
        case WM_LBUTTONUP: on_click(dip(GET_X_LPARAM(lparam)), dip(GET_Y_LPARAM(lparam)), false); return 0;
        case WM_RBUTTONUP: on_click(dip(GET_X_LPARAM(lparam)), dip(GET_Y_LPARAM(lparam)), true); return 0;
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lparam) != window_) {
                pressed_.reset();
                if (dragging_) {
                    const std::string id = *std::exchange(dragging_, std::nullopt);
                    on_drag(id, -1.0f, true);  // ended without a position: keep where it is
                }
            }
            return 0;
        case WM_MOUSEWHEEL:
            on_wheel(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA * kWheelStep);
            return 0;
        case WM_SYSKEYDOWN:
            if (capturing_hotkey_) {
                capture_hotkey(wparam);  // Alt combinations and F10 arrive here
                return 0;
            }
            break;
        case WM_SYSCHAR:
            if (capturing_hotkey_) return 0;  // no menu beep
            break;
        case WM_KEYDOWN:
            if (capturing_hotkey_) {
                capture_hotkey(wparam);
                return 0;
            }
            if (on_key(wparam)) return 0;
            if (wparam == 'K' && GetKeyState(VK_CONTROL) < 0) {
                SetFocus(search_box_);
                SendMessageW(search_box_, EM_SETSEL, 0, -1);
                return 0;
            }
            if (wparam == 'F' && GetKeyState(VK_CONTROL) < 0) {
                SetFocus(search_box_);
                return 0;
            }
            if (wparam == VK_ESCAPE && saved_bar_) {
                saved_bar_.reset();
                invalidate();
                return 0;
            }
            if (wparam == VK_F5) {
                scan_folder();
                return 0;
            }
            break;
        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            const int code = HIWORD(wparam);
            if (code == EN_SETFOCUS) cancel_hotkey_capture();  // typing goes to the box, not the hotkey
            if (id == kSearchBoxId && (code == EN_SETFOCUS || code == EN_KILLFOCUS)) {
                invalidate();  // the box border shows focus
            } else if (id == kSearchBoxId && code == EN_CHANGE) {
                search_ = to_utf8(window_text(search_box_));
                scroll_ = 0;
                invalidate();
            } else if (id == kSearchBoxId && code == IDCANCEL) {
                SetWindowTextW(search_box_, L"");
                SetFocus(window_);
            } else if (id == kSearchBoxId && code == IDOK) {
                SetFocus(window_);
            } else if (id == kEditBoxId && (code == IDOK || code == IDCANCEL)) {
                end_edit(code == IDOK);
            } else if (id == kEditBoxId && code == EN_KILLFOCUS) {
                end_edit(true);
            } else if (id == kNotesBoxId && code == EN_KILLFOCUS) {
                commit_clip_text();
            } else if (id == kTagBoxId && code == IDOK) {
                run_clip("add-tag", Rect{});
            } else if (id == kTagBoxId && code == IDCANCEL) {
                SetWindowTextW(tag_box_, L"");
                SetFocus(window_);
            } else if (id == kTagBoxId && (code == EN_SETFOCUS || code == EN_KILLFOCUS)) {
                invalidate();
            } else if (id == kSettingsSearchId && code == EN_CHANGE) {
                settings_query_ = to_utf8(window_text(settings_search_));
                page_scroll_ = 0;
                invalidate();
            } else if (id == kSettingsSearchId && (code == EN_SETFOCUS || code == EN_KILLFOCUS)) {
                invalidate();
            } else if (id == kSettingsSearchId && code == IDCANCEL) {
                SetWindowTextW(settings_search_, L"");
                SetFocus(window_);
            } else if (id == kSettingsSearchId && code == IDOK) {
                SetFocus(window_);
            }
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            // Each box sits on a drawn background; its own must match it.
            const HDC dc = reinterpret_cast<HDC>(wparam);
            const HWND control = reinterpret_cast<HWND>(lparam);
            SetTextColor(dc, colorref(kInk));
            Color back = kRaise;
            HBRUSH brush = edit_brush_;
            if (control == notes_box_) {
                back = kBg;
                brush = bg_brush_;
            } else if (control == tag_box_) {
                back = kCard;
                brush = card_brush_;
            } else if (control == settings_search_) {
                back = kInk.mix(kBg, 0.05f);
                brush = field_brush_;
            } else if (control == edit_box_ && page_ == Page::Clip) {
                back = kBg;
                brush = bg_brush_;
            }
            SetBkColor(dc, colorref(back));
            return reinterpret_cast<LRESULT>(brush);
        }
        case WM_DISPLAYCHANGE:
            // A display was plugged in or out: the Record and Settings pages list them.
            refresh_devices();
            refresh_overlay();
            invalidate();
            break;
        case WM_DEVICECHANGE:
            // A microphone plugged in or out: the pickers and the Record page show it.
            if (wparam == 0x0007 /* DBT_DEVNODES_CHANGED */ && (page_ == Page::Settings || page_ == Page::Record)) {
                refresh_devices();
                invalidate();
            }
            break;
        case WM_MBUTTONDOWN:
            if (capturing_hotkey_ && capture_target_ == HotkeyTarget::PushToTalk) {
                capture_hotkey(VK_MBUTTON);  // the wheel button as the push-to-talk key
                return 0;
            }
            break;
        case WM_XBUTTONDOWN:
            if (capturing_hotkey_ && capture_target_ == HotkeyTarget::PushToTalk) {
                // A mouse side button as the push-to-talk key.
                capture_hotkey(GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2);
                return TRUE;
            }
            break;
        case WM_HOTKEY:
            on_shortcut(static_cast<int>(wparam));
            return 0;
        case WM_INPUT: {
            // The keyboard as the hardware sends it, also while a game holds it exclusively.
            RAWINPUT raw{};
            UINT size = sizeof(raw);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) !=
                    static_cast<UINT>(-1) &&
                raw.header.dwType == RIM_TYPEKEYBOARD) {
                on_raw_key(raw.data.keyboard);
            }
            break;  // DefWindowProc frees the input
        }
        case WM_TIMER:
            if (wparam == kPlayerTimer) {
                tick_player();
                return 0;
            }
            if (wparam == kMeterTimer) {
                // Kept while the page is open (also hidden or minimised: no repaint then), so the
                // meters move again as soon as the window is back.
                if (page_ != Page::Record) KillTimer(window_, kMeterTimer);
                else if (IsWindowVisible(window_) && !IsIconic(window_)) invalidate();
                return 0;
            }
            if (wparam == kTickTimer) {
                if (finished_) return 0;  // Windows is shutting down: everything has been stopped
                // Not during a trim (the new recorder clears unfinished files in the clips folder),
                // not while a shell operation waits on this thread.
                if (restart_due_ && !trimming_ && !shell_busy_ && !recycling_ &&
                    std::chrono::steady_clock::now() >= *restart_due_) {
                    restart_recorder();
                }
                sample_load();
                if (page_ == Page::Record) invalidate();  // buffer fill, load
                refresh_status();
                if (!tray_added_) add_tray_icon();  // NIM_ADD can time out while Explorer starts up
                if (!notice_.empty() && std::chrono::steady_clock::now() >= notice_until_) {
                    notice_.clear();
                    invalidate();
                }
                const auto now = std::chrono::steady_clock::now();
                const bool visible = IsWindowVisible(window_) && !IsIconic(window_);
                if (visible && now >= next_scan_) {
                    scan_folder();
                }
                if (now >= next_cleanup_) {
                    next_cleanup_ = now + kCleanupInterval;
                    run_cleanup();
                }
                if (visible && saved_bar_) {
                    invalidate();  // "Saved 6s ago" ticks
                }
            }
            return 0;
        case kMsgSaved: take_saved(); return 0;
        case kMsgWork: run_ui_tasks(); return 0;
        case kMsgExport:
            if (dialog_open()) invalidate();
            return 0;
        case kMsgUpdate:
            if (page_ == Page::Settings) invalidate();  // the update row shows the state
            return 0;
        case kMsgPlayer:
            if (clip_view_ && clip_view_->player && lparam == clip_view_->player_tag &&
                clip_view_->player->on_event(static_cast<DWORD>(wparam))) {
                // The file's own length beats the library's rounded one, until a handle was moved.
                if (!clip_view_->trim_touched && clip_view_->player->duration() > 0) {
                    clip_view_->out = clip_view_->player->duration();
                    clip_view_->in = 0;
                }
                invalidate();
            }
            return 0;
        case kMsgProbe: apply_media_results(); return 0;
        case kMsgTray:
            switch (LOWORD(lparam)) {
                case WM_LBUTTONUP:
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case NIN_BALLOONUSERCLICK:
                    show(SW_SHOW);
                    SetForegroundWindow(window_);
                    break;
                case WM_CONTEXTMENU: tray_menu(); break;
                default: break;
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE) cancel_hotkey_capture();  // the key must work in the game
            refresh_overlay();  // hidden while Backtrack itself is in front
            break;
        case WM_CLOSE:
            // §: the X hides; Instant Replay keeps running in the tray until "Quit".
            cancel_hotkey_capture();
            if (editing_) end_edit(true);
            commit_clip_text();
            close_menu();
            if (clip_view_ && clip_view_->player) clip_view_->player->pause();  // no sound from a hidden window
            ShowWindow(window_, SW_HIDE);
            if (!notified_hidden_ && settings_.notification && tray_added_) {
                notified_hidden_ = true;
                NOTIFYICONDATAW data{};
                data.cbSize = sizeof(data);
                data.hWnd = window_;
                data.uID = kTrayId;
                data.uFlags = NIF_INFO;
                data.dwInfoFlags = NIIF_USER | NIIF_NOSOUND | NIIF_LARGE_ICON;
                data.hBalloonIcon = icon_big_;
                wcscpy_s(data.szInfoTitle, L"Backtrack is still running");
                wcscpy_s(data.szInfo, L"Instant Replay keeps going in the background. Quit from the tray icon.");
                Shell_NotifyIconW(NIM_MODIFY, &data);
            }
            return 0;
        case WM_QUERYENDSESSION:
            if (recorder_->saving()) {
                ShutdownBlockReasonCreate(window_, L"Saving a clip");  // shown on the shutdown screen
            } else {
                ShutdownBlockReasonDestroy(window_);
            }
            return TRUE;
        case WM_ENDSESSION:
            if (wparam != FALSE) {
                // ENDSESSION_CLOSEAPP: an installer or update asks (through the Restart Manager) to
                // close Backtrack, not a logoff.
                const bool close_app = (static_cast<DWORD>(lparam) & ENDSESSION_CLOSEAPP) != 0;
                if (close_app) {
                    log::info("Asked to close (an update or installer)");
                } else {
                    log::info("Windows is shutting down or logging off");
                }
                session_ended_ = true;
                if (editing_) end_edit(true);
                commit_clip_text();
                restart_due_.reset();  // already in backtrack.ini; the next start uses it
                finish();  // §6: the clip being written is finished and goes into the library
                ShutdownBlockReasonDestroy(window_);
                // Then really end, so the installer can replace the exe without killing it.
                quitting_ = true;
                DestroyWindow(window_);
                return 0;
            }
            ShutdownBlockReasonDestroy(window_);  // cancelled: a stale reason would show next time
            return 0;
        case WM_DESTROY:
            KillTimer(window_, kTickTimer);
            KillTimer(window_, kPlayerTimer);
            clip_view_.reset();
            KillTimer(window_, kMeterTimer);
            unregister_hotkeys();
            remove_tray_icon();
            save_library();
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY: {
            const HWND window = window_;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            window_ = nullptr;
            search_box_ = nullptr;
            edit_box_ = nullptr;
            notes_box_ = nullptr;
            tag_box_ = nullptr;
            settings_search_ = nullptr;
            return DefWindowProcW(window, message, wparam, lparam);
        }
        default: break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

LRESULT App::hit_test(POINT screen) const {
    POINT client = screen;
    ScreenToClient(window_, &client);
    RECT rect{};
    GetClientRect(window_, &rect);
    const float scale = graphics_->scale();
    const float x = static_cast<float>(client.x) / scale;
    const float y = static_cast<float>(client.y) / scale;
    const float w = static_cast<float>(rect.right) / scale;
    const float h = static_cast<float>(rect.bottom) / scale;
    if (!IsZoomed(window_)) {
        const bool left = x < kResizeBorder;
        const bool right = x >= w - kResizeBorder;
        const bool top = y < kResizeBorder;
        const bool bottom = y >= h - kResizeBorder;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (bottom) return HTBOTTOM;
        if (top) return HTTOP;
    }
    if (y >= kTitleBar || menu_) {
        return HTCLIENT;  // with a menu open, a click on the title bar only closes it
    }
    for (const Hit& hit : hits_) {
        if (hit.rect.y < kTitleBar && hit.rect.contains(x, y)) {
            // The maximize button answers as one so Windows 11 shows its snap layouts on hover.
            return hit.action == Action::Maximize ? HTMAXBUTTON : HTCLIENT;
        }
    }
    return HTCAPTION;
}

void App::update_edit_font() {
    const int height = -static_cast<int>(std::lround(13.0f * graphics_->scale()));
    const HFONT font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (font != nullptr) {
        // The controls switch to the new font before the old one is deleted under them.
        for (const HWND control : {search_box_, edit_box_, notes_box_, tag_box_, settings_search_}) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        if (edit_font_ != nullptr) DeleteObject(edit_font_);
        edit_font_ = font;
    }
    layout_search_box();
}

void App::layout_search_box() {
    if (search_box_ == nullptr || graphics_ == nullptr) return;
    RECT client{};
    GetClientRect(window_, &client);
    const float scale = graphics_->scale();
    const float width = static_cast<float>(client.right) / scale;
    const float box_w = std::clamp(width * 0.3f, 220.0f, 380.0f);
    search_rect_ = Rect{(width - box_w) / 2, 6, box_w, 28};
    // The edit control sits inside the drawn box, after the magnifier, before the Ctrl K hint.
    const float text_h = 18;
    const Rect inner{search_rect_.x + 30, search_rect_.y + (search_rect_.h - text_h) / 2, search_rect_.w - 30 - 62, text_h};
    MoveWindow(search_box_, static_cast<int>(std::lround(inner.x * scale)), static_cast<int>(std::lround(inner.y * scale)),
               static_cast<int>(std::lround(inner.w * scale)), static_cast<int>(std::lround(inner.h * scale)), TRUE);
}

// ---------------------------------------------------------------------------------------------
// Data

std::int64_t App::now_unix() const {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

fs::path App::clip_path(const std::string& file) const {
    return recorder_->output_dir() / to_wide(file);
}

bool App::clips_folder_available() {
    if (!recorder_->output_dir_is_fallback()) return true;
    show_notice(L"The clips folder is not available (drive unplugged?). Restart Backtrack once it is back.");
    return false;
}

void App::keep_copy_of_library(const wchar_t* why) {
    // Named by date and time so an older copy is never overwritten.
    SYSTEMTIME t{};
    GetLocalTime(&t);
    fs::path copy = paths_.library;
    copy.replace_filename(std::format(L"library.{}-{:04}{:02}{:02}-{:02}{:02}{:02}.json", why, t.wYear, t.wMonth, t.wDay,
                                      t.wHour, t.wMinute, t.wSecond));
    std::error_code ec;
    fs::copy_file(paths_.library, copy, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        // Without the copy, the original is the only one left: it must not be saved over.
        library_writable_ = false;
        log::warn("Could not keep a copy of library.json ({}); library changes are not saved this run",
                  ec.message());
        return;
    }
    log::warn("Kept a copy of library.json as {}", path_text(copy.filename()));
}

void App::load_library() {
    std::error_code ec;
    if (!fs::exists(paths_.library, ec)) {
        if (ec) {
            // Cannot even tell whether it is there: do not risk replacing it this run.
            library_writable_ = false;
            log::warn("Cannot check {} ({}); library changes are not saved this run", path_text(paths_.library),
                      ec.message());
        }
        return;  // first run: the folder scan fills it
    }
    std::ifstream file(paths_.library, std::ios::binary);
    std::stringstream text;
    if (file) text << file.rdbuf();
    if (!file || file.bad()) {
        // There but unreadable (locked by antivirus or a sync tool): treating it as a first run
        // would overwrite the real one with an empty library.
        library_writable_ = false;
        log::warn("Cannot read {}; library changes are not saved this run so it is not overwritten",
                  path_text(paths_.library));
        return;
    }
    file.close();
    const JsonParseResult parsed = parse_json(text.str());
    if (!parsed.value) {
        // Never throw away what the user set up: keep the damaged file and start over from the
        // clips folder (favorites and collections are in the copy).
        log::warn("library.json could not be read ({})", parsed.error);
        keep_copy_of_library(L"damaged");
        return;
    }
    if ((*parsed.value)["version"].as_int(1) > kLibraryFileVersion) {
        // A newer Backtrack wrote it; saving drops what this version does not know. Keep it.
        keep_copy_of_library(L"newer");
    }
    library_ = Library::from_json(*parsed.value);
}

void App::save_library() {
    if (!library_writable_) return;
    fs::path temp = paths_.library;
    temp += L".tmp";
    const std::string text = library_.to_json().dump(2) + "\n";
    {
        // Flushed to the disk before the rename: after a power cut the file is either the old one
        // or the new one, never a file full of zeros.
        const UniqueHandle out(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                           FILE_ATTRIBUTE_NORMAL, nullptr));
        DWORD written = 0;
        const bool ok = out && WriteFile(out.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                        written == text.size() && FlushFileBuffers(out.get());
        if (!ok) {
            static log::RateLimit limit(std::chrono::minutes(1));
            if (limit.allow()) {
                log::warn("Could not write {}: {}", path_text(temp), hresult_text(HRESULT_FROM_WIN32(GetLastError())));
                show_notice(L"Could not save the library (disk full or folder not writable?)");
            }
            return;
        }
    }
    if (!MoveFileExW(temp.c_str(), paths_.library.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        static log::RateLimit limit(std::chrono::minutes(1));
        if (limit.allow()) {
            log::warn("Could not save the library to {}: {}", path_text(paths_.library),
                      hresult_text(HRESULT_FROM_WIN32(GetLastError())));
        }
        std::error_code ec;
        fs::remove(temp, ec);
    }
}

void App::scan_folder() {
    next_scan_ = std::chrono::steady_clock::now() + kScanInterval;
    if (shell_busy_) {
        return;  // clips are being moved: a scan now would drop the ones already gone from the old folder
    }
    if (recorder_->output_dir_is_fallback()) {
        return;  // the real clips folder is not there this run: its library stays as it is
    }
    std::vector<FolderFile> files;
    WIN32_FIND_DATAW found{};
    const fs::path pattern = recorder_->output_dir() / L"*.mp4";
    const HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr,
                                         FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        // Only "no .mp4 in a folder that is there" means the folder is empty. Anything else (drive
        // unplugged, network gone, access denied) says nothing about the clips: the library is
        // left alone rather than emptied.
        const DWORD error = GetLastError();
        std::error_code ec;
        if (error != ERROR_FILE_NOT_FOUND || !fs::is_directory(recorder_->output_dir(), ec)) {
            static log::RateLimit limit(std::chrono::minutes(5));
            if (limit.allow()) {
                log::warn("Cannot list the clips folder {}: {}", path_text(recorder_->output_dir()),
                          hresult_text(HRESULT_FROM_WIN32(error)));
            }
            return;
        }
    } else {
        do {
            const std::wstring name = found.cFileName;
            // "*.mp4" also matches through 8.3 short names; check the real extension.
            const bool mp4 = name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".mp4") == 0;
            if (!mp4 || (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
            FolderFile file;
            file.file = to_utf8(name);
            file.size_bytes = (static_cast<std::int64_t>(found.nFileSizeHigh) << 32) | found.nFileSizeLow;
            file.modified = unix_from_filetime(found.ftLastWriteTime);
            files.push_back(std::move(file));
        } while (FindNextFileW(find, &found));
        const DWORD error = GetLastError();
        FindClose(find);
        if (error != ERROR_NO_MORE_FILES) {
            return;  // the listing broke off (network drop): a partial list would drop clips
        }
    }
    if (library_.sync_with_folder(files)) {
        if (editing_) end_edit(true);  // the cards moved under the rename box
        if (clip_view_ && library_.find(clip_view_->file) == nullptr) close_clip();  // deleted in Explorer
        save_library();
        request_missing_info();
        invalidate();
    }
}

void App::apply_media_results() {
    bool changed = false;
    for (MediaInfo& info : probe_->take_results()) {
        probing_.erase(probe_key(info.file, info.size_bytes));
        const ClipEntry* clip = library_.find(info.file);
        // A result for an older size of the file (it was still being copied, or another program
        // rewrote it) belongs to nothing on screen; the new size has its own request.
        if (clip == nullptr || clip->size_bytes != info.size_bytes) continue;
        if (info.ok && info.duration_s > 0.0 &&
            (clip->duration_s <= 0.0 || clip->width != info.width || clip->height != info.height)) {
            library_.set_media_info(info.file, clip->duration_s > 0.0 ? clip->duration_s : info.duration_s,
                                    info.width, info.height);
            changed = true;
        }
        urgent_.erase(probe_key(info.file, info.size_bytes));
        if (info.thumbnail.empty()) {
            thumbs_missing_.insert(probe_key(clip->file, clip->size_bytes));
        } else {
            pending_thumbs_[thumb_key(*clip)] = std::move(info.thumbnail);
        }
    }
    if (changed) save_library();
    invalidate();
}

void App::run_cleanup() {
    // Never against the Videos fallback (a same-named file there is not the clip), never while
    // the library is not being saved, never while a recycle is already waiting.
    if (recorder_->output_dir_is_fallback() || !library_writable_ || recycling_ || shell_busy_) return;
    const CleanupPolicy policy = cleanup_policy();
    std::vector<std::string> due = library_.due_for_removal(policy, now_unix());
    // A clip that could not be removed (in use, no Recycle Bin, the user said no) waits for the
    // next run instead of a warning or a question every hour.
    std::erase_if(due, [this](const std::string& file) {
        // Nor the clip being exported: it waits for the next run instead of ending the export.
        return cleanup_skipped_.contains(file) ||
               (export_ && export_->stage == ExportView::Stage::Running && export_->file == file) ||
               (share_ && share_->stage == ShareView::Stage::Working && share_->file == file);
    });
    if (due.empty()) return;
    if (editing_) end_edit(true);
    if (clip_view_ && std::find(due.begin(), due.end(), clip_view_->file) != due.end()) {
        close_clip();  // the player holds the file open
        if (page_ == Page::Clip) set_page(Page::Library);
    }
    std::vector<fs::path> paths;
    for (const std::string& file : due) paths.push_back(clip_path(file));
    recycling_ = true;
    const std::vector<fs::path> failed = shell::recycle(paths);
    recycling_ = false;
    if (quit_pending_) {
        quit();
        return;
    }
    if (editing_) end_edit(true);  // a rename started while waiting: the cards are about to move
    for (const std::string& file : due) {
        const fs::path path = clip_path(file);
        if (std::find(failed.begin(), failed.end(), path) == failed.end()) {
            library_.remove(file);
        } else {
            cleanup_skipped_.insert(file);
        }
    }
    save_library();
    invalidate();
}

void App::on_saved(const SaveResult& result) {
    if (!result.ok) {
        flash_overlay(L"Could not save the clip", kDanger);
        refresh_status();
        return;  // the sound and the log already said why
    }
    if (editing_) end_edit(true);  // the new card moves the others under the rename box
    const std::string file = to_utf8(result.path.filename().wstring());
    std::error_code ec;
    const bool in_library =
        !recorder_->output_dir_is_fallback() && fs::equivalent(result.path.parent_path(), recorder_->output_dir(), ec);
    std::int64_t size = static_cast<std::int64_t>(fs::file_size(result.path, ec));
    if (ec) size = 0;
    if (in_library) {
        ClipEntry clip;
        clip.file = file;
        clip.created = now_unix();
        clip.size_bytes = size;
        clip.duration_s = result.seconds;
        clip.source = std::format("Display {}", recorded_display() + 1);
        std::vector<double> marks;
        for (const Time100ns b : bookmarks_) marks.push_back(static_cast<double>(b) / kTicksPerSecond);
        // The newest frame in the buffer is a few frames behind the clock: a bookmark pressed just
        // before saving lands a moment past the end and is kept at the end instead of dropped.
        clip.bookmarks = bookmarks_within(marks, static_cast<double>(result.start) / kTicksPerSecond, result.seconds + 0.5);
        for (double& b : clip.bookmarks) b = std::min(b, result.seconds);
        library_.add_saved_clip(clip);
        new_files_.insert(file);
        save_library();
        // Not after the window is gone (finish() at quit): it would only hold up the exit.
        if (window_ != nullptr && probing_.insert(probe_key(file, size)).second) {
            probe_->request(recorder_->output_dir(), file, size, true);
        }
    } else {
        log::info("The clip went to {} because the clips folder was not available; it is not in the library",
                  path_text(result.path.parent_path()));
    }
    SavedBar bar;
    bar.headline = "Saved";
    bar.file = in_library ? file : std::string{};
    bar.path = result.path;
    bar.title = in_library ? display_title(*library_.find(file)) : file;
    bar.detail = std::format("Display {}, {}, {}", recorded_display() + 1, format_duration(result.seconds),
                             format_size(size));
    bar.at = std::chrono::steady_clock::now();
    if (settings_.saved_bar) saved_bar_ = bar;
    flash_overlay(std::format(L"Clip saved \u00b7 {}", to_wide(format_duration(result.seconds))), kOk);
    notify_saved(result);
    refresh_status();
    invalidate();
}

// ---------------------------------------------------------------------------------------------
// Actions

void App::save_clip() {
    if (finished_) return;
    std::string reason;
    if (recorder_->save_clip(reason, saved_seconds(settings_))) {
        log::info("Saving the last {} seconds...", saved_seconds(settings_));
    } else {
        log::warn("{}", reason);
        if (settings_.sound) {
            PlaySoundW(L"SystemHand", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
        }
    }
    refresh_status();
}

void App::toggle_pause() {
    if (recorder_->paused()) {
        recorder_->resume();
    } else {
        recorder_->pause();
    }
    refresh_status();
}

void App::refresh_status() {
    const std::string status = recorder_->status_text();
    if (status != status_) {
        status_ = status;
        if (tray_added_) {
            NOTIFYICONDATAW data{};
            data.cbSize = sizeof(data);
            data.hWnd = window_;
            data.uID = kTrayId;
            data.uFlags = NIF_TIP | NIF_SHOWTIP;
            wcsncpy_s(data.szTip, (L"Backtrack - " + to_wide(status_)).c_str(), _TRUNCATE);
            Shell_NotifyIconW(NIM_MODIFY, &data);
        }
        if (window_ != nullptr) invalidate();
    }
    refresh_overlay();  // after status_, which it shows
}

std::string& App::hotkey_text(Settings& s, HotkeyTarget target) {
    switch (target) {
        case HotkeyTarget::MicMute: return s.mic_mute_hotkey;
        case HotkeyTarget::Overlay: return s.overlay_hotkey;
        case HotkeyTarget::Bookmark: return s.bookmark_hotkey;
        case HotkeyTarget::PushToTalk: return s.push_to_talk_key;
        case HotkeyTarget::Save: break;
    }
    return s.save_hotkey;
}

const std::string& App::hotkey_text(const Settings& s, HotkeyTarget target) {
    return hotkey_text(const_cast<Settings&>(s), target);
}

void App::register_hotkey() {
    // In order: a key already taken by an earlier one (the save key first) only does that one.
    static constexpr const char* kWhat[] = {"save clips", "mute the microphone", "show the overlay", "add bookmarks"};
    for (int i = 0; i < kGlobalHotkeys; ++i) {
        const auto target = static_cast<HotkeyTarget>(i);
        const std::string& text = hotkey_text(settings_, target);
        hotkey_ok_[i] = false;
        registered_keys_[i].reset();
        if (text.empty()) continue;  // only the save key cannot be empty
        bool taken = false;
        for (int j = 0; j < i; ++j) taken = taken || text == hotkey_text(settings_, static_cast<HotkeyTarget>(j));
        if (taken) {
            log::warn("{} is set for two shortcuts; it does not {}", text, kWhat[i]);
            continue;
        }
        const auto parsed = parse_hotkey(text);
        if (!parsed.hotkey) continue;
        hotkey_ok_[i] = RegisterHotKey(window_, i + 1, parsed.hotkey->modifiers | MOD_NOREPEAT,
                                       parsed.hotkey->virtual_key) != FALSE;
        registered_keys_[i] = parsed.hotkey;
        if (!hotkey_ok_[i]) {
            log::warn("Cannot use {} to {}: {}. Pick another key in Settings > Hotkeys", text, kWhat[i],
                      hresult_text(HRESULT_FROM_WIN32(GetLastError())));
        }
    }
}

void App::unregister_hotkeys() {
    for (int i = 0; i < kGlobalHotkeys; ++i) {
        if (hotkey_ok_[i]) UnregisterHotKey(window_, i + 1);
        hotkey_ok_[i] = false;
    }
}

namespace {

// Mono 16-bit PCM as a WAV file in memory, for PlaySound(SND_MEMORY).
std::vector<std::uint8_t> wav_from_pcm(const std::vector<std::int16_t>& pcm, int rate) {
    const auto data_bytes = static_cast<std::uint32_t>(pcm.size() * 2);
    std::vector<std::uint8_t> wav;
    const auto put = [&wav](std::uint32_t v, int bytes) {
        for (int b = 0; b < bytes; ++b) wav.push_back(static_cast<std::uint8_t>((v >> (8 * b)) & 0xFF));
    };
    for (const char c : std::string_view("RIFF")) wav.push_back(static_cast<std::uint8_t>(c));
    put(36 + data_bytes, 4);
    for (const char c : std::string_view("WAVEfmt ")) wav.push_back(static_cast<std::uint8_t>(c));
    put(16, 4);
    put(1, 2);  // PCM
    put(1, 2);  // mono
    put(static_cast<std::uint32_t>(rate), 4);
    put(static_cast<std::uint32_t>(rate) * 2, 4);
    put(2, 2);
    put(16, 2);
    for (const char c : std::string_view("data")) wav.push_back(static_cast<std::uint8_t>(c));
    put(data_bytes, 4);
    for (const std::int16_t s : pcm) put(static_cast<std::uint16_t>(s), 2);
    return wav;
}

// Two short tones as a WAV in memory: rising for "mic on", falling for "mic muted". Heard in a
// fullscreen game where the window is not visible (and recorded like any other sound).
const std::vector<std::uint8_t>& mute_tone(bool muted) {
    static const auto make = [](bool falling) {
        constexpr int kRate = 44100;
        constexpr int kToneMs = 70;
        constexpr int kSamples = kRate * kToneMs / 1000;
        std::vector<std::int16_t> pcm;
        const double first = falling ? 880.0 : 587.33;
        const double second = falling ? 587.33 : 880.0;
        for (const double hz : {first, second}) {
            for (int i = 0; i < kSamples; ++i) {
                const double fade = std::min({1.0, i / 220.0, (kSamples - i) / 220.0});  // no clicks
                pcm.push_back(static_cast<std::int16_t>(std::sin(2 * 3.14159265358979 * hz * i / kRate) * 6000 * fade));
            }
        }
        return wav_from_pcm(pcm, kRate);
    };
    static const std::vector<std::uint8_t> muted_tone = make(true);
    static const std::vector<std::uint8_t> unmuted_tone = make(false);
    return muted ? muted_tone : unmuted_tone;
}

// One short high blip: "marked". Different from the save sound and the mute tones.
const std::vector<std::uint8_t>& bookmark_tone() {
    static const std::vector<std::uint8_t> tone = [] {
        constexpr int kRate = 44100;
        constexpr int kSamples = kRate * 60 / 1000;
        std::vector<std::int16_t> pcm;
        for (int i = 0; i < kSamples; ++i) {
            const double fade = std::min({1.0, i / 220.0, (kSamples - i) / 600.0});
            pcm.push_back(static_cast<std::int16_t>(std::sin(2 * 3.14159265358979 * 1318.5 * i / kRate) * 5000 * fade));
        }
        return wav_from_pcm(pcm, kRate);
    }();
    return tone;
}

}  // namespace

void App::on_shortcut(int id) {
    if (id < 1 || id > kGlobalHotkeys) return;
    // UNVERIFIED: how far apart WM_HOTKEY and WM_INPUT of one press arrive; measured both within
    // the same message batch here, 250 ms leaves room and is shorter than any deliberate double press.
    constexpr auto kSamePress = std::chrono::milliseconds(250);
    const auto now = std::chrono::steady_clock::now();
    if (now - last_shortcut_[id] < kSamePress) return;
    last_shortcut_[id] = now;
    switch (static_cast<HotkeyTarget>(id - 1)) {
        case HotkeyTarget::Save: save_clip(); break;
        case HotkeyTarget::MicMute: toggle_mic_mute(); break;
        case HotkeyTarget::Overlay: toggle_overlay(); break;
        case HotkeyTarget::Bookmark: add_bookmark(); break;
        case HotkeyTarget::PushToTalk: break;
    }
}

void App::on_raw_key(const RAWKEYBOARD& key) {
    const UINT vk = key.VKey;
    if (vk == 0 || vk == 0xFF) return;  // part of an escape sequence, not a key
    const bool down = (key.Flags & RI_KEY_BREAK) == 0;
    const auto now = std::chrono::steady_clock::now();
    // One physical key: left and right Ctrl/Alt share a virtual key and differ by the E0 prefix.
    const std::uint32_t id = vk << 16 | key.MakeCode | ((key.Flags & RI_KEY_E0) != 0 ? 0x8000u : 0u);
    std::uint32_t modifier = 0;
    switch (vk) {
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: modifier = hotkey_mod::kCtrl; break;
        case VK_MENU: case VK_LMENU: case VK_RMENU: modifier = hotkey_mod::kAlt; break;
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: modifier = hotkey_mod::kShift; break;
        case VK_LWIN: case VK_RWIN: modifier = hotkey_mod::kWin; break;
        default: break;
    }
    if (!down) {
        raw_keys_down_.erase(id);
        return;
    }
    // A key held down repeats every ~30 ms (after a delay of up to a second, a Windows setting):
    // a "press" of a key that is still down is a repeat. A release can get lost (lock screen,
    // UAC), so a key not heard from for a few seconds counts as up again. Keys are never logged.
    constexpr auto kHeldExpiry = std::chrono::seconds(5);
    const auto last = raw_keys_down_.find(id);
    const bool repeat = last != raw_keys_down_.end() && now - last->second.last < kHeldExpiry;
    if (repeat) {
        last->second.last = now;
        return;
    }
    raw_keys_down_[id] = RawKey{now, now, modifier};
    if (modifier != 0 || capturing_hotkey_ || finished_) return;  // Settings is listening for a new key
    // Modifiers from this same stream, so they are right even when this message is handled late
    // (the window busy) or a game keeps Windows' own key state from updating. Windows' state as
    // well: one held down since before this ran (or whose press was missed) still counts. A
    // modifier the stream says is down but Windows has up for seconds lost its release.
    std::uint32_t modifiers = 0;
    for (auto it = raw_keys_down_.begin(); it != raw_keys_down_.end();) {
        const auto& [key_id, held] = *it;
        if (held.modifier != 0 && GetAsyncKeyState(static_cast<int>(key_id >> 16)) >= 0 &&
            now - held.since > std::chrono::seconds(2)) {
            it = raw_keys_down_.erase(it);
            continue;
        }
        modifiers |= held.modifier;
        ++it;
    }
    if (GetAsyncKeyState(VK_CONTROL) < 0) modifiers |= hotkey_mod::kCtrl;
    if (GetAsyncKeyState(VK_MENU) < 0) modifiers |= hotkey_mod::kAlt;
    if (GetAsyncKeyState(VK_SHIFT) < 0) modifiers |= hotkey_mod::kShift;
    if (GetAsyncKeyState(VK_LWIN) < 0 || GetAsyncKeyState(VK_RWIN) < 0) modifiers |= hotkey_mod::kWin;
    const Hotkey pressed{modifiers, vk};
    for (int i = 0; i < kGlobalHotkeys; ++i) {
        // Only keys Windows gave us: one another program owns stays that program's (the raw path
        // is for games that switch hotkeys off, not for taking keys from other programs).
        if (hotkey_ok_[i] && registered_keys_[i] && *registered_keys_[i] == pressed) {
            on_shortcut(i + 1);
            return;
        }
    }
}

void App::toggle_mic_mute() {
    if (finished_ || !settings_.mic_enabled) return;  // no microphone recorded: nothing to mute
    recorder_->set_mic_muted(!recorder_->mic_muted());
    const bool muted = recorder_->mic_muted();  // what the recorder really did (no sound engine: never muted)
    log::info("Microphone {}", muted ? "muted" : "unmuted");
    flash_overlay(muted ? L"Mic muted" : L"Mic on", muted ? kDanger : kOk);
    if (settings_.sound) {
        PlaySoundW(reinterpret_cast<LPCWSTR>(mute_tone(muted).data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }
    invalidate();
}

void App::toggle_overlay() {
    if (finished_) return;
    change_settings([](Settings& s) { s.overlay = !s.overlay; });
}

void App::add_bookmark() {
    if (finished_ || recorder_->paused()) {
        // Nothing is being recorded: there is no moment to mark.
        if (settings_.sound) PlaySoundW(L"SystemHand", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
        return;
    }
    const Time100ns now = recorder_->recording_time();
    // Older than the buffer reaches back: no clip can contain them any more.
    const Time100ns keep = static_cast<Time100ns>(settings_.clip_seconds + 10) * kTicksPerSecond;
    while (!bookmarks_.empty() && (bookmarks_.front() < now - keep || bookmarks_.size() >= kMaxBookmarks)) {
        bookmarks_.pop_front();
    }
    bookmarks_.push_back(now);
    log::info("Bookmark added");
    if (settings_.sound) {
        PlaySoundW(reinterpret_cast<LPCWSTR>(bookmark_tone().data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }
    flash_overlay(L"Bookmark added", kAccent);
}

RECT App::recorded_monitor_rect() const {
    // By the device name: monitors_ is in DXGI order, EnumDisplayMonitors in its own.
    struct Search {
        std::wstring name;
        RECT rect{};
        bool found = false;
    } search;
    const int index = recorded_display();
    if (index >= 0 && static_cast<std::size_t>(index) < monitors_.size()) search.name = monitors_[static_cast<std::size_t>(index)].device_name;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(data);
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (!GetMonitorInfoW(monitor, &info)) return TRUE;
            if ((!s.name.empty() && s.name == info.szDevice) || (s.name.empty() && (info.dwFlags & MONITORINFOF_PRIMARY))) {
                s.rect = info.rcMonitor;
                s.found = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    if (!search.found) {
        MONITORINFO info{sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &info);
        search.rect = info.rcMonitor;
    }
    return search.rect;
}

void App::refresh_overlay() {
    if (!overlay_) return;
    OverlayState state;
    // Not over Backtrack's own window (it would sit on the caption buttons), not after Quit.
    state.visible = settings_.overlay && !finished_ && window_ != nullptr && GetForegroundWindow() != window_;
    const bool paused = recorder_->paused();
    const bool normal = status_.starts_with("Recording") || status_.starts_with("Saving");
    state.text = paused ? L"Replay paused" : normal ? L"Replay on" : L"Replay starting";  // as the title bar says
    if (!paused && recorder_->mic_muted() && settings_.mic_enabled) state.text += L" \u00b7 Mic muted";
    state.dot = paused ? kOff : normal ? kAccent : kWarn;
    state.monitor = recorded_monitor_rect();
    state.corner = static_cast<int>(settings_.overlay_corner);
    state.opacity = static_cast<float>(settings_.overlay_opacity) / 100.0f;
    // After Quit the message loop ends before a fade could run: gone at once.
    state.reduce_motion = settings_.reduce_motion || finished_;
    state.chip = kPop.with_alpha(0.94f);
    state.ink = kInk;
    overlay_->update(state);
}

void App::flash_overlay(std::wstring text, Color dot) {
    if (overlay_ && settings_.overlay && !finished_) overlay_->flash(std::move(text), dot);
}

void App::apply_audio_live() {
    recorder_->set_volumes(settings_.system_volume, settings_.mic_volume);
    recorder_->set_noise_suppression(settings_.noise_suppression);
    const auto key = parse_hotkey(settings_.push_to_talk_key, HotkeyUse::Held);
    // A key that cannot be read keeps the mic closed (key 0 is never held): never recording the
    // voice all the time when the player asked for push-to-talk.
    recorder_->set_push_to_talk(settings_.push_to_talk, key.hotkey.value_or(Hotkey{}));
}

void App::refresh_devices() {
    try {
        monitors_ = list_monitors();
    } catch (const std::exception& e) {
        log::warn("Could not list the monitors: {}", e.what());
    }
    try {
        microphones_ = list_microphones();
    } catch (const std::exception& e) {
        log::warn("Could not list the microphones: {}", e.what());
    }
    try {
        playback_devices_ = list_playback_devices();
    } catch (const std::exception& e) {
        log::warn("Could not list the playback devices: {}", e.what());
    }
}

void App::set_view(LibraryView view, std::string collection) {
    set_page(Page::Library);  // closes the clip page (its player and its file)
    view_ = view;
    view_collection_ = std::move(collection);
    scroll_ = 0;
    if (editing_) end_edit(true);
    invalidate();
}

LibraryQuery App::current_query() const {
    LibraryQuery q;
    q.view = view_;
    q.collection = view_collection_;
    q.search = search_;
    q.sort = sort_;
    q.now = now_unix();
    return q;
}

int App::recorded_display() const {
    // An unplugged display records monitor 0 (ScreenCapture's fallback).
    const int count = static_cast<int>(monitors_.size());
    // What the running (or just stopped) recorder was made with, not a change still waiting to apply.
    const int monitor = recorder_settings_.monitor;
    return count == 0 || (monitor >= 0 && monitor < count) ? std::max(monitor, 0) : 0;
}

std::wstring App::view_title() const {
    switch (view_) {
        case LibraryView::All: return L"All clips";
        case LibraryView::Favorites: return L"Favorites";
        case LibraryView::Recent: return L"Recent";
        case LibraryView::Trash: return L"Trash";
        case LibraryView::Shared: return L"Shared";
        case LibraryView::Collection: {
            const Collection* c = library_.find_collection(view_collection_);
            return c != nullptr ? to_wide(c->name) : L"Collection";
        }
    }
    return L"All clips";
}

// ---------------------------------------------------------------------------------------------
// Tray

void App::add_tray_icon() {
    if (tray_added_) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayId;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kMsgTray;
    data.hIcon = icon_small_;
    wcsncpy_s(data.szTip, (L"Backtrack - " + to_wide(status_.empty() ? std::string("starting") : status_)).c_str(),
              _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_ADD, &data) && !Shell_NotifyIconW(NIM_MODIFY, &data)) {
        // No taskbar yet (started with Windows before Explorer): the timer and TaskbarCreated try
        // again. A busy Explorer can also time out an add that did happen: then the modify works.
        return;
    }
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
    tray_added_ = true;
}

void App::remove_tray_icon() {
    if (!tray_added_) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    tray_added_ = false;
}

void App::tray_menu() {
    const HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;
    AppendMenuW(menu, MF_STRING, kTrayOpen, L"Open Backtrack");
    const std::wstring save = std::format(L"Save last {} s\t{}", saved_seconds(settings_), to_wide(settings_.save_hotkey));
    AppendMenuW(menu, MF_STRING, kTraySave, save.c_str());
    AppendMenuW(menu, MF_STRING, kTrayPause, recorder_->paused() ? L"Resume Instant Replay" : L"Pause Instant Replay");
    if (settings_.mic_enabled) {
        const std::wstring mute = (recorder_->mic_muted() ? std::wstring(L"Unmute microphone") : std::wstring(L"Mute microphone")) +
                                  (settings_.mic_mute_hotkey.empty() ? L"" : L"\t" + to_wide(settings_.mic_mute_hotkey));
        AppendMenuW(menu, MF_STRING, kTrayMute, mute.c_str());
    }
    AppendMenuW(menu, MF_STRING, kTrayFolder, L"Open clips folder");
    AppendMenuW(menu, MF_STRING, kTraySettings, L"Settings");
    AppendMenuW(menu, MF_STRING, kTrayLog, L"Open log");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayQuit, L"Quit Backtrack");
    SetMenuDefaultItem(menu, kTrayOpen, FALSE);
    POINT at{};
    GetCursorPos(&at);
    SetForegroundWindow(window_);  // otherwise the menu does not close when clicking elsewhere
    const UINT command = static_cast<UINT>(
        TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, at.x, at.y, window_, nullptr));
    DestroyMenu(menu);
    PostMessageW(window_, WM_NULL, 0, 0);
    switch (command) {
        case kTrayOpen:
            show(SW_SHOW);
            SetForegroundWindow(window_);
            break;
        case kTraySave: save_clip(); break;
        case kTrayPause: toggle_pause(); break;
        case kTrayMute: toggle_mic_mute(); break;
        case kTrayFolder: shell::open(recorder_->output_dir()); break;
        case kTraySettings:
            show(SW_SHOW);
            SetForegroundWindow(window_);
            set_page(Page::Settings);
            break;
        case kTrayLog: shell::open(paths_.log); break;
        case kTrayQuit: quit(); break;
        default: break;
    }
}

void App::notify_saved(const SaveResult& result) {
    // Only when the window is not in front: there the Saved bar says it. Windows keeps these
    // quiet during a fullscreen game (Focus assist); the sound is ours.
    if (!settings_.notification || !tray_added_ || GetForegroundWindow() == window_) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayId;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_USER | NIIF_NOSOUND | NIIF_LARGE_ICON;
    data.hBalloonIcon = icon_big_;
    wcsncpy_s(data.szInfoTitle, std::format(L"Saved the last {} s", std::lround(result.seconds)).c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, result.path.filename().c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

// ---------------------------------------------------------------------------------------------
// Painting

void App::paint() {
    if (!graphics_->begin()) return;
    // EndDraw also when something below throws: a frame left open breaks every later one.
    struct EndDrawGuard {
        Graphics& g;
        bool done = false;
        ~EndDrawGuard() {
            if (!done) g.end();
        }
    } end_guard{*graphics_};
    hits_.clear();
    ++frame_;
    Graphics& g = *graphics_;
    RECT client{};
    GetClientRect(window_, &client);
    const float w = dip(client.right);
    const float h = dip(client.bottom);

    for (auto it = pending_thumbs_.begin(); it != pending_thumbs_.end();) {
        g.upload_bitmap(it->first, it->second.data(), kThumbWidth, kThumbHeight);
        bitmap_use_[it->first] = frame_;
        it = pending_thumbs_.erase(it);
    }

    g.clear(kBg);
    tracks_.clear();
    for (auto& [control, place] : controls_) place.second = false;  // pages say again which are shown
    const Rect main{kSidebar, kTitleBar, w - kSidebar, h - kTitleBar};
    switch (page_) {
        case Page::Library: paint_library(main); break;
        case Page::Clip: paint_clip_page(main); break;
        case Page::Record: paint_record_page(main); break;
        case Page::Uploads:
            paint_placeholder_page(main, L"Uploads",
                                   L"Clips you shared are listed under Shared in the library, with their links. "
                                   L"Connected accounts (YouTube and others) are coming later.");
            break;
        case Page::Settings: paint_settings_page(main); break;
    }
    paint_sidebar(h);
    paint_title_bar(w);
    // The title bar's search box is a window of its own above the scrim: off while the dialog is open.
    if ((IsWindowEnabled(search_box_) != FALSE) == dialog_open()) EnableWindow(search_box_, !dialog_open());
    if (dialog_open()) {
        paint_export(w, h);
        paint_share(w, h);
        // The page's text boxes are windows of their own and would show through the dialog.
        for (auto& [control, place] : controls_) place.second = false;
    }
    if (menu_) paint_menu(w, h);
    end_guard.done = true;
    g.end();

    // The page's text boxes follow what was just drawn (they are child windows on top).
    for (auto& [control, place] : controls_) {
        const bool show = place.second && place.first.w > 4 && place.first.h > 4;
        if (show) {
            const float scale = g.scale();
            RECT now{};
            GetWindowRect(control, &now);
            MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&now), 2);
            const RECT want{static_cast<LONG>(std::lround(place.first.x * scale)),
                            static_cast<LONG>(std::lround(place.first.y * scale)),
                            static_cast<LONG>(std::lround(place.first.right() * scale)),
                            static_cast<LONG>(std::lround(place.first.bottom() * scale))};
            if (!EqualRect(&now, &want)) {
                MoveWindow(control, want.left, want.top, want.right - want.left, want.bottom - want.top, TRUE);
            }
        }
        if ((IsWindowVisible(control) != FALSE) != show) {
            if (!show && GetFocus() == control) SetFocus(window_);
            ShowWindow(control, show ? SW_SHOWNA : SW_HIDE);
        }
    }

    // Thumbnails the cards asked for while drawing, and GPU memory kept in check.
    if (recorder_->output_dir_is_fallback()) to_request_.clear();
    for (const auto& [file, size] : to_request_) {
        // Also when already queued: on screen goes first (the queue may hold thousands at first start).
        probing_.insert(probe_key(file, size));
        probe_->request(recorder_->output_dir(), file, size, false, true);
    }
    to_request_.clear();
    if (bitmap_use_.size() > kMaxBitmaps) {
        std::vector<std::pair<std::uint64_t, std::wstring>> by_age;
        for (const auto& [key, used] : bitmap_use_) by_age.emplace_back(used, key);
        std::sort(by_age.begin(), by_age.end());
        for (std::size_t i = 0; i < by_age.size() - kMaxBitmaps; ++i) {
            g.forget_bitmap(by_age[i].second);
            bitmap_use_.erase(by_age[i].second);
        }
    }
}

void App::paint_title_bar(float width) {
    Graphics& g = *graphics_;
    g.fill(Rect{0, 0, width, kTitleBar}, kSide);
    g.fill(Rect{0, kTitleBar - 1, width, 1}, kLine);

    // App mark and name.
    g.logo(Rect{13, 11, 18, 18});
    g.text(L"Backtrack", Rect{38, 0, 120, kTitleBar}, TextStyle{12, Weight::Semibold}, kInk);

    // Search box (the edit control is a child window on top of it).
    const bool focused = GetFocus() == search_box_;
    g.fill_round(search_rect_, kRadius, kRaise);
    g.stroke_round(search_rect_, kRadius, focused ? kAccent : kLine);
    g.icon(glyph::kSearch, Rect{search_rect_.x + 8, search_rect_.y, 16, search_rect_.h}, 12, kMute);
    float kx = search_rect_.right() - 58;
    kx = key_chip(kx, search_rect_.y + 4, L"Ctrl");
    key_chip(kx + 3, search_rect_.y + 4, L"K");
    add_hit(search_rect_, Action::Search);

    // Caption buttons.
    const float close_x = width - kCaptionButton;
    const float max_x = close_x - kCaptionButton;
    const float min_x = max_x - kCaptionButton;
    const Rect min_rect{min_x, 0, kCaptionButton, kTitleBar - 1};
    const Rect max_rect{max_x, 0, kCaptionButton, kTitleBar - 1};
    const Rect close_rect{close_x, 0, kCaptionButton, kTitleBar - 1};
    if (hovered(Action::Minimize)) g.fill(min_rect, kHover);
    if (hovered(Action::Maximize) || max_hovered_) g.fill(max_rect, kHover);
    if (hovered(Action::Close)) g.fill(close_rect, kDanger);
    g.icon(glyph::kMinimize, min_rect, 10, kInk);
    g.icon(IsZoomed(window_) ? glyph::kRestore : glyph::kMaximize, max_rect, 10, kInk);
    g.icon(glyph::kChromeClose, close_rect, 10, hovered(Action::Close) ? Color::hex(0xffffff) : kInk);
    add_hit(min_rect, Action::Minimize);
    add_hit(max_rect, Action::Maximize);
    add_hit(close_rect, Action::Close);

    // Replay pill: what Instant Replay is doing, at a glance. Click pauses or resumes.
    const bool paused = recorder_->paused();
    const bool trouble = !paused && !status_.starts_with("Recording") && !status_.starts_with("Saving");
    const std::wstring pill = paused    ? L"Replay paused"
                              : trouble ? L"Replay starting"
                                        : std::format(L"Replay on, {} s", settings_.clip_seconds);
    const TextStyle pill_style{12, Weight::Semibold};
    const float pill_w = g.text_width(pill, pill_style) + 10 + 7 + 8 + 10;
    const Rect pill_rect{min_x - 8 - pill_w, 7, pill_w, 26};
    const Color fg = paused ? kMute : trouble ? kWarn : kAccent;
    const Color bg = paused ? kInk.with_alpha(0.07f) : trouble ? kWarn.mix(kBg, 0.15f) : kSoft;
    g.fill_round(pill_rect, 13, hovered(Action::TogglePause, "pill") ? bg.mix(kInk, 0.85f) : bg);
    g.fill_circle(pill_rect.x + 14, pill_rect.y + 13, 4, paused ? kOff : fg);
    g.text(pill, Rect{pill_rect.x + 25, pill_rect.y, pill_w - 25, pill_rect.h}, pill_style, fg);
    add_hit(pill_rect, Action::TogglePause, "pill");
}

void App::paint_sidebar(float height) {
    Graphics& g = *graphics_;
    const Rect side{0, kTitleBar, kSidebar, height - kTitleBar};
    g.fill(side, kSide);
    g.fill(Rect{kSidebar - 1, kTitleBar, 1, side.h}, kLine);

    const LibraryCounts counts = library_.counts(now_unix());
    const float x = 8;
    const float w = kSidebar - 16;
    float y = kTitleBar + 10;
    const TextStyle label{12, Weight::Semibold};
    const auto section = [&](std::wstring_view text) {
        g.text(text, Rect{x + 10, y, w - 20, 26}, label, kMute);
        y += 26;
    };
    const bool library_page = page_ == Page::Library;
    const auto view_item = [&](wchar_t glyph, std::wstring_view text, std::size_t count, LibraryView view,
                               const char* arg) {
        nav_item(Rect{x, y, w, kNavHeight}, glyph, text, count, library_page && view_ == view, Action::View, arg);
        y += kNavHeight + 1;
    };

    section(L"Library");
    view_item(glyph::kAllClips, L"All clips", counts.all, LibraryView::All, "all");
    view_item(glyph::kFavorite, L"Favorites", counts.favorites, LibraryView::Favorites, "favorites");
    view_item(glyph::kRecent, L"Recent", counts.recent, LibraryView::Recent, "recent");
    view_item(glyph::kShare, L"Shared", counts.shared, LibraryView::Shared, "shared");
    view_item(glyph::kTrash, L"Trash", counts.trash, LibraryView::Trash, "trash");

    // The bottom cards claim their space first; collections get what is left.
    const float cards_h = 66 + 8 + 70;
    const float more_h = 26 + 3 * (kNavHeight + 1);
    const float bottom_limit = height - 12 - cards_h - 10 - more_h;

    y += 14;
    g.text(L"Collections", Rect{x + 10, y, w - 50, 26}, label, kMute);
    const Rect add_rect{x + w - 30, y, 26, 26};
    if (hovered(Action::NewCollection)) g.fill_round(add_rect, 6, kHover);
    g.icon(glyph::kAdd, add_rect, 12, kMute);
    add_hit(add_rect, Action::NewCollection);
    y += 26;
    const auto& collections = library_.collections();
    // How many rows fit; when not all do, the last row is "N more" and the selected or edited
    // collection takes the last real row, so it is always on screen.
    const auto fit = static_cast<std::size_t>(std::max(0.0f, (bottom_limit - y) / (kNavHeight + 1)));
    const bool overflow = collections.size() > fit;
    const std::size_t rows = overflow ? (fit > 0 ? fit - 1 : 0) : collections.size();
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < rows; ++i) order.push_back(i);
    const std::string& keep = editing_ && editing_->kind == Editing::Kind::CollectionName ? editing_->target
                              : view_ == LibraryView::Collection                         ? view_collection_
                                                                                         : std::string();
    if (overflow && rows > 0 && !keep.empty()) {
        for (std::size_t i = rows; i < collections.size(); ++i) {
            if (collections[i].id == keep) order.back() = i;
        }
    }
    hidden_collections_.clear();
    for (std::size_t i = 0; i < collections.size(); ++i) {
        if (std::find(order.begin(), order.end(), i) == order.end()) hidden_collections_.push_back(i);
    }
    for (const std::size_t i : order) {
        const Collection& c = collections[i];
        const Color dot = kCollectionColors[std::clamp(c.color, 0, kCollectionColorCount - 1)];
        const Rect r{x, y, w, kNavHeight};
        nav_item(r, 0, to_wide(c.name.empty() ? std::string("Untitled") : c.name), library_.collection_size(c.id),
                 library_page && view_ == LibraryView::Collection && view_collection_ == c.id, Action::View, c.id, dot);
        if (editing_ && editing_->kind == Editing::Kind::CollectionName && editing_->target == c.id) {
            edit_rect_ = Rect{r.x + 32, r.y + 4, r.w - 40, r.h - 8};
        }
        y += kNavHeight + 1;
    }
    if (overflow) {
        const Rect more{x, y, w, kNavHeight};
        if (hovered(Action::MoreCollections)) g.fill_round(more, kRadius, kHover);
        g.text(std::format(L"{} more", hidden_collections_.size()), Rect{x + 35, y, w - 40, kNavHeight}, TextStyle{12.5f},
               kMute);
        add_hit(more, Action::MoreCollections);
        y += kNavHeight + 1;
    }
    if (collections.empty()) {
        g.text(L"No collections yet", Rect{x + 10, y, w - 20, 22}, TextStyle{12}, kMute);
        y += 22;
    }

    y = std::max(y + 14, bottom_limit + 4);
    section(L"More");
    const auto page_item = [&](wchar_t glyph, std::wstring_view text, Page page, const char* arg, bool soon) {
        nav_item(Rect{x, y, w, kNavHeight}, glyph, text, 0, page_ == page, Action::Page, arg, std::nullopt, soon);
        y += kNavHeight + 1;
    };
    page_item(glyph::kRecord, L"Record", Page::Record, "record", false);
    page_item(glyph::kUpload, L"Uploads", Page::Uploads, "uploads", true);
    page_item(glyph::kSettings, L"Settings", Page::Settings, "settings", false);

    // Instant Replay card.
    const float card_y = height - 12 - cards_h;
    const Rect replay{x, card_y, w, 66};
    const bool paused = recorder_->paused();
    const bool normal = status_.starts_with("Recording") || status_.starts_with("Saving");
    g.fill_round(replay, 10, kCard);
    g.stroke_round(replay, 10, kLine);
    const Color state_color = paused ? kMute : normal ? kAccent : kWarn;
    g.fill_circle(replay.x + 16, replay.y + 20, 4, paused ? kOff : state_color);
    g.text(paused ? L"Instant Replay paused" : L"Instant Replay on", Rect{replay.x + 28, replay.y + 10, replay.w - 36, 20},
           TextStyle{13, Weight::Semibold}, state_color);
    std::wstring hint = !normal && !paused ? to_wide(status_)
                                           : std::format(L"{} saves the last {} s", to_wide(settings_.save_hotkey),
                                                         saved_seconds(settings_));
    Color hint_color = kMute;
    if (!hotkey_ok_[0]) {
        // Another program has the key: say so instead of promising it.
        hint = std::format(L"{} is taken by another program", to_wide(settings_.save_hotkey));
        hint_color = kWarn;
    }
    g.text(hint, Rect{replay.x + 12, replay.y + 34, replay.w - 24, 20}, TextStyle{12}, hint_color);

    // Storage card.
    const Rect storage{x, card_y + 66 + 8, w, 70};
    g.fill_round(storage, 10, kCard);
    g.stroke_round(storage, 10, kLine);
    const std::wstring used = to_wide(format_size(counts.bytes));
    const float used_w = g.text(used, Rect{storage.x + 12, storage.y + 10, storage.w - 24, 22},
                                TextStyle{15, Weight::Bold, false, true}, kInk);
    g.text(L" used", Rect{storage.x + 12 + used_w, storage.y + 10, storage.w - 24 - used_w, 22}, TextStyle{12}, kMute);
    const std::size_t total = counts.all + counts.trash;
    if (recorder_->output_dir_is_fallback()) {
        g.text(L"Clips folder missing: saving to Videos", Rect{storage.x + 12, storage.y + 38, storage.w - 24, 20},
               TextStyle{12}, kWarn);
    } else {
        g.text(std::format(L"{} {} in {}", total, total == 1 ? L"clip" : L"clips",
                           recorder_->output_dir().filename().wstring()),
               Rect{storage.x + 12, storage.y + 38, storage.w - 24, 20}, TextStyle{12}, kMute);
    }
}

void App::paint_placeholder_page(const Rect& area, const wchar_t* title, const wchar_t* text) {
    Graphics& g = *graphics_;
    g.text(title, Rect{area.x + kPad, area.y + 26, area.w - 2 * kPad, 38}, TextStyle{28, Weight::Bold, true}, kInk);
    g.paragraph(text, Rect{area.x + kPad, area.y + 72, std::min(area.w - 2 * kPad, 560.0f), 80}, TextStyle{13.5f}, kMute);
}

void App::place_control(HWND control, const Rect& rect, bool show) {
    // Only when it is fully inside the visible page: a child window cannot be clipped by a drawn edge.
    const bool inside = rect.y >= page_view_.y && rect.bottom() <= page_view_.bottom() && !menu_;
    controls_[control] = {rect, show && inside};
}

void App::hide_page_controls() {
    for (auto& [control, place] : controls_) {
        place.second = false;
        if (GetFocus() == control) SetFocus(window_);
        ShowWindow(control, SW_HIDE);
    }
}

Rect App::begin_page(const Rect& area) {
    page_view_ = area;
    graphics_->push_clip(area);
    return Rect{area.x, area.y - page_scroll_, area.w, area.h};
}

void App::end_page(const Rect& area, float content_bottom) {
    graphics_->pop_clip();
    const float content_h = content_bottom + page_scroll_ - area.y;
    page_max_scroll_ = std::max(0.0f, content_h - area.h);
    if (page_scroll_ > page_max_scroll_) {
        page_scroll_ = page_max_scroll_;
        invalidate();
    }
    if (page_max_scroll_ > 0) {
        const float track = area.h - 8;
        const float thumb = std::max(30.0f, track * area.h / content_h);
        const float pos = area.y + 4 + (track - thumb) * (page_scroll_ / page_max_scroll_);
        graphics_->fill_round(Rect{area.right() - 6, pos, 3, thumb}, 1.5f, kInk.with_alpha(0.18f));
    }
    // Hits scrolled out of the page must not catch clicks through the title bar.
    std::erase_if(hits_, [&area](const Hit& hit) {
        return hit.rect.x >= area.x && (hit.rect.bottom() <= area.y || hit.rect.y >= area.bottom());
    });
    for (Hit& hit : hits_) {
        if (hit.rect.x < area.x) continue;  // the sidebar
        const float top = std::max(hit.rect.y, area.y);
        const float bottom = std::min(hit.rect.bottom(), area.bottom());
        hit.rect = Rect{hit.rect.x, top, hit.rect.w, bottom - top};
    }
}

void App::paint_library(const Rect& area) {
    Graphics& g = *graphics_;

    // Top strip: Instant Replay state and the save shortcut (.st-top).
    const Rect top{area.x, area.y, area.w, kTopStrip};
    g.fill(Rect{top.x, top.bottom() - 1, top.w, 1}, kLine);
    const bool paused = recorder_->paused();
    g.fill_circle(top.x + 22, top.y + top.h / 2, 4, paused ? kOff : kAccent);
    const float status_w = g.text(paused ? L"Instant Replay paused" : L"Instant Replay on",
                                  Rect{top.x + 32, top.y, 200, top.h}, TextStyle{12.5f, Weight::Semibold},
                                  paused ? kMute : kAccent);
    float bx = top.x + 32 + status_w + 14;
    {
        // Save chip: [F8] Save last 30 s
        const std::wstring label = std::format(L"Save last {} s", saved_seconds(settings_));
        const TextStyle style{12.5f, Weight::Semibold};
        std::vector<std::wstring> keys;
        std::wstring hotkey = to_wide(settings_.save_hotkey);
        for (std::size_t start = 0; start <= hotkey.size();) {
            const std::size_t plus = hotkey.find(L'+', start);
            keys.push_back(hotkey.substr(start, plus == std::wstring::npos ? std::wstring::npos : plus - start));
            if (plus == std::wstring::npos) break;
            start = plus + 1;
        }
        float keys_w = 0;
        for (const std::wstring& k : keys) keys_w += std::max(22.0f, g.text_width(k, TextStyle{11, Weight::Semibold}) + 12) + 3;
        const float chip_w = 8 + keys_w + 4 + g.text_width(label, style) + 10;
        const Rect chip{bx, top.y + 7, chip_w, 32};
        g.fill_round(chip, kRadius, hovered(Action::SaveNow) ? kInk.with_alpha(0.1f) : kInk.with_alpha(0.05f));
        g.stroke_round(chip, kRadius, kLine);
        float kx = chip.x + 8;
        for (const std::wstring& k : keys) kx = key_chip(kx, chip.y + 6, k) + 3;
        g.text(label, Rect{kx + 4, chip.y, chip_w, chip.h}, style, kInk);
        add_hit(chip, Action::SaveNow);
        bx = chip.right() + 6;
    }
    button(Rect{bx, top.y + 7, 96, 32}, paused ? L"Resume" : L"Pause", paused ? glyph::kPlay : glyph::kPause,
           Action::TogglePause, "strip");

    // Header bar: view name, clip count, sort (.st-bar).
    const Rect head{area.x, top.bottom(), area.w, kHeaderBar};
    g.fill(Rect{head.x, head.bottom() - 1, head.w, 1}, kLine);
    const std::vector<std::size_t> shown = library_.query(current_query());
    g.text(view_title(), Rect{head.x + 18, head.y, 260, head.h}, TextStyle{14, Weight::Semibold}, kInk);
    const Rect sort_rect{head.right() - 14 - 112, head.y + 8, 112, 32};
    {
        const bool hot = hovered(Action::Sort);
        g.fill_round(sort_rect, kRadius, hot ? kInk.with_alpha(0.1f) : kInk.with_alpha(0.05f));
        g.stroke_round(sort_rect, kRadius, kLine);
        g.icon(glyph::kSort, Rect{sort_rect.x + 8, sort_rect.y, 16, sort_rect.h}, 12, kMute);
        g.text(sort_label(sort_), Rect{sort_rect.x + 30, sort_rect.y, 60, sort_rect.h}, TextStyle{12.5f, Weight::Semibold}, kInk);
        g.icon(glyph::kChevronDown, Rect{sort_rect.right() - 24, sort_rect.y, 16, sort_rect.h}, 9, kMute);
        add_hit(sort_rect, Action::Sort);
    }
    const std::wstring count = shown.size() == 1 ? L"1 clip" : std::format(L"{} clips", shown.size());
    g.text(count, Rect{sort_rect.x - 130, head.y, 118, head.h}, TextStyle{12.5f, Weight::Regular, false, true}, kMute,
           Align::End);
    const std::wstring problem = !notice_.empty() ? notice_
                                 : !library_writable_
                                     ? std::wstring(L"library.json could not be read: changes are not saved this run")
                                     : std::wstring();
    if (!problem.empty()) {
        const float title_w = g.text_width(view_title(), TextStyle{14, Weight::Semibold});
        g.text(problem, Rect{head.x + 18 + title_w + 16, head.y, sort_rect.x - 140 - (head.x + 18 + title_w + 16), head.h},
               TextStyle{12.5f, Weight::Semibold}, kDanger);
    } else if (view_ == LibraryView::Trash) {
        g.text(std::format(L"Clips here go to the Recycle Bin after {} days", kTrashDays),
               Rect{head.x + 18 + g.text_width(view_title(), TextStyle{14, Weight::Semibold}) + 16, head.y, 360, head.h},
               TextStyle{12.5f}, kMute);
    }

    // Grid, above the Saved bar when it is showing.
    const float bar_space = saved_bar_ ? kSavedBar + 16 + 16 : 0;
    grid_area_ = Rect{area.x, head.bottom(), area.w, area.bottom() - head.bottom() - bar_space};
    g.push_clip(grid_area_);
    if (shown.empty()) {
        const wchar_t* title = !search_.empty()             ? L"No clips match your search"
                               : view_ == LibraryView::Trash ? L"The Trash is empty"
                               : view_ == LibraryView::All   ? L"No clips yet"
                                                             : L"Nothing here yet";
        std::wstring text;
        switch (view_) {
            case LibraryView::All:
                text = std::format(L"Press {} to save the last {} seconds. Your clips show up here.",
                                   to_wide(settings_.save_hotkey), saved_seconds(settings_));
                break;
            case LibraryView::Favorites: text = L"Click the star on a clip to keep it here."; break;
            case LibraryView::Recent: text = std::format(L"Clips from the last {} days show up here.", kRecentDays); break;
            case LibraryView::Trash: text = L"Clips you delete wait here before they go to the Recycle Bin."; break;
            case LibraryView::Collection: text = L"Right-click a clip and choose Collections to add it here."; break;
            case LibraryView::Shared: text = L"Open a clip and press Share to get a link for Discord."; break;
        }
        g.icon(glyph::kVideo, Rect{grid_area_.x, grid_area_.y + 70, grid_area_.w, 40}, 28, kOff);
        g.text(title, Rect{grid_area_.x, grid_area_.y + 116, grid_area_.w, 26}, TextStyle{16, Weight::Semibold, true},
               kInk, Align::Center);
        if (search_.empty()) {
            g.text(text, Rect{grid_area_.x, grid_area_.y + 144, grid_area_.w, 22}, TextStyle{13}, kMute, Align::Center);
        }
        max_scroll_ = 0;
        scroll_ = 0;
    } else {
        // .st-grid: the thumbnail size sets how many cards share a row (fewer when the window is
        // narrow), density the room between them.
        const bool compact = settings_.density == Density::Compact;
        const float gap_x = compact ? kGridGapCompactX : kGridGapX;
        const float gap_y = compact ? kGridGapCompactY : kGridGapY;
        const float pad_x = compact ? kGridPadCompactX : kGridPadX;
        const int max_columns = settings_.thumbnail_size == ThumbnailSize::Small   ? 5
                                : settings_.thumbnail_size == ThumbnailSize::Large ? 3
                                                                                   : 4;
        const float inner_w = grid_area_.w - 2 * pad_x;
        const int columns =
            std::clamp(static_cast<int>((inner_w + gap_x) / (kCardMin + gap_x)), 1, max_columns);
        const float card_w = (inner_w - gap_x * static_cast<float>(columns - 1)) / static_cast<float>(columns);
        const float card_h = card_w * 9.0f / 16.0f + kCardInfo;
        const auto rows = static_cast<int>((shown.size() + static_cast<std::size_t>(columns) - 1) /
                                           static_cast<std::size_t>(columns));
        const float content_h = kGridPadY + static_cast<float>(rows) * (card_h + gap_y) - gap_y + kGridPadBottom;
        max_scroll_ = std::max(0.0f, content_h - grid_area_.h);
        scroll_ = std::clamp(scroll_, 0.0f, max_scroll_);
        for (std::size_t i = 0; i < shown.size(); ++i) {
            const int row = static_cast<int>(i / static_cast<std::size_t>(columns));
            const int column = static_cast<int>(i % static_cast<std::size_t>(columns));
            const Rect card{grid_area_.x + pad_x + static_cast<float>(column) * (card_w + gap_x),
                            grid_area_.y + kGridPadY + static_cast<float>(row) * (card_h + gap_y) - scroll_, card_w,
                            card_h};
            if (card.bottom() < grid_area_.y || card.y > grid_area_.bottom()) continue;  // not visible
            paint_card(library_.clips()[shown[i]], card);
        }
        if (max_scroll_ > 0) {
            // A thin scroll indicator on the right edge.
            const float track = grid_area_.h - 8;
            const float thumb = std::max(30.0f, track * grid_area_.h / content_h);
            const float pos = grid_area_.y + 4 + (track - thumb) * (scroll_ / max_scroll_);
            g.fill_round(Rect{grid_area_.right() - 6, pos, 3, thumb}, 1.5f, kInk.with_alpha(0.18f));
        }
    }
    g.pop_clip();
    // Hits for cards outside the grid (scrolled under the header) must not catch clicks.
    std::erase_if(hits_, [this](const Hit& hit) {
        return (hit.action == Action::OpenClip || hit.action == Action::ToggleFavorite) &&
               (hit.rect.bottom() <= grid_area_.y || hit.rect.y >= grid_area_.bottom());
    });
    for (Hit& hit : hits_) {
        if (hit.action == Action::OpenClip || hit.action == Action::ToggleFavorite) {
            const float top_y = std::max(hit.rect.y, grid_area_.y);
            const float bottom_y = std::min(hit.rect.bottom(), grid_area_.bottom());
            hit.rect = Rect{hit.rect.x, top_y, hit.rect.w, bottom_y - top_y};
        }
    }

    if (saved_bar_) paint_saved_bar(area);

}

void App::paint_thumbnail(const ClipEntry& clip, const Rect& rect, float radius) {
    Graphics& g = *graphics_;
    const std::wstring key = thumb_key(clip);
    if (g.draw_bitmap(key, rect, radius)) {
        bitmap_use_[key] = frame_;
        return;
    }
    // No picture (yet): a calm gradient picked by the name, so cards do not all look the same.
    std::size_t hash = std::hash<std::string>{}(clip.file);
    const Pair& pair = kPlaceholders[hash % kPlaceholderCount];
    g.fill_gradient(rect, radius, pair.top, pair.bottom);
    const std::string pkey = probe_key(clip.file, clip.size_bytes);
    if (!pending_thumbs_.contains(key) && !thumbs_missing_.contains(pkey) && !urgent_.contains(pkey)) {
        // Asked once with priority; the probe keeps it queued until it is done.
        urgent_.insert(pkey);
        to_request_.emplace_back(clip.file, clip.size_bytes);
    }
}

void App::paint_card(const ClipEntry& clip, const Rect& rect) {
    Graphics& g = *graphics_;
    const bool hot = hovered(Action::OpenClip, clip.file) || hovered(Action::ToggleFavorite, clip.file);
    g.fill_round(rect, kCardRadius, kCard);

    const Rect thumb{rect.x, rect.y, rect.w, rect.w * 9.0f / 16.0f};
    g.push_round_clip(rect, kCardRadius);
    paint_thumbnail(clip, thumb, 0);
    g.pop_clip();
    g.stroke_round(rect, kCardRadius, hot ? kAccent.mix(kLine, 0.5f) : kLine);

    // Duration badge.
    if (clip.duration_s > 0) {
        const std::wstring length = to_wide(format_duration(clip.duration_s));
        const TextStyle style{11, Weight::Semibold, false, true};
        const float w = g.text_width(length, style) + 12;
        const Rect badge{thumb.right() - 6 - w, thumb.bottom() - 6 - 18, w, 18};
        g.fill_round(badge, 5, Color::hex(0x000000, 0.72f));
        g.text(length, badge, style, Color::hex(0xffffff), Align::Center);
    }
    // "New": saved during this run and not opened yet.
    if (new_files_.contains(clip.file)) {
        const TextStyle style{11, Weight::Bold};
        const float w = g.text_width(L"New", style) + 16;
        const Rect badge{thumb.x + 6, thumb.bottom() - 6 - 18, w, 18};
        g.fill_round(badge, 9, kAccent);
        g.text(L"New", badge, style, kOnAccent, Align::Center);
    }
    // Favorite star: always shown when set, otherwise on hover.
    const Rect star{thumb.right() - 6 - 24, thumb.y + 6, 24, 24};
    if (clip.favorite || hot) {
        const bool star_hot = hovered(Action::ToggleFavorite, clip.file);
        g.fill_round(star, 6, Color::hex(0x0a0c10, star_hot ? 0.88f : 0.62f));
        g.icon(clip.favorite ? glyph::kFavoriteFill : glyph::kFavorite, star, 12,
               clip.favorite ? kStar : Color::hex(0xffffff));
    }

    // Title and details.
    const float info_y = thumb.bottom() + 8;
    if (editing_ && editing_->kind == Editing::Kind::ClipTitle && editing_->target == clip.file && !edit_positioned_) {
        // The rename box goes over the title, if the title is fully inside the grid.
        const Rect title_box{rect.x + 6, info_y - 3, rect.w - 12, 24};
        if (title_box.y >= grid_area_.y && title_box.bottom() <= grid_area_.bottom()) edit_rect_ = title_box;
    }
    g.text(to_wide(display_title(clip)), Rect{rect.x + 10, info_y, rect.w - 20, 18}, TextStyle{13.5f, Weight::Semibold},
           kInk);
    std::wstring meta;
    const auto add = [&meta](const std::string& part) {
        if (part.empty()) return;
        if (!meta.empty()) meta += L"  ·  ";
        meta += to_wide(part);
    };
    add(format_resolution(clip.width, clip.height));
    add(format_size(clip.size_bytes));
    add(clip.trashed_at ? "In Trash" : format_age(now_unix() - clip.created));
    g.text(meta, Rect{rect.x + 10, info_y + 21, rect.w - 20, 16}, TextStyle{12}, kMute);

    // The star is checked first: hits are searched from the end.
    add_hit(rect, Action::OpenClip, clip.file);
    if (clip.favorite || hot) add_hit(star, Action::ToggleFavorite, clip.file);
}

void App::paint_saved_bar(const Rect& area) {
    Graphics& g = *graphics_;
    const SavedBar& bar = *saved_bar_;
    const Rect r{area.x + kPad, area.bottom() - 16 - kSavedBar, area.w - 2 * kPad, kSavedBar};
    // A soft shadow under the floating bar (--shadow), then the card.
    g.fill_round(Rect{r.x + 2, r.y + 8, r.w - 4, r.h}, 18, Color::hex(0x000000, 0.25f));
    g.fill_round(r, 18, kCard);
    g.stroke_round(r, 18, kLine);
    add_hit(r, Action::None);

    const Rect thumb{r.x + 10, r.y + (r.h - 49.5f) / 2, 88, 49.5f};
    const ClipEntry* clip = bar.file.empty() ? nullptr : library_.find(bar.file);
    if (clip != nullptr) {
        paint_thumbnail(*clip, thumb, 9);
    } else {
        g.fill_gradient(thumb, 9, kPlaceholders[0].top, kPlaceholders[0].bottom);
    }

    const auto age = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - bar.at).count();
    const std::wstring headline = to_wide(bar.headline);
    const std::wstring saved = age < 60 ? std::format(L"{} {}s ago", headline, age)
                                        : std::format(L"{} {}m ago", headline, age / 60);
    const float tx = thumb.right() + 16;
    // The buttons take what they need; the text gets the rest.
    const float buttons_w = 84 + 118 + 86 + 84 + 32 + 4 * 2;
    const float text_w = std::max(80.0f, r.right() - 12 - buttons_w - 16 - tx);
    g.icon(glyph::kCheck, Rect{tx, r.y + 12, 14, 18}, 11, kAccent);
    g.text(saved, Rect{tx + 18, r.y + 12, text_w - 18, 18}, TextStyle{13, Weight::Bold}, kAccent);
    g.text(to_wide(clip != nullptr ? display_title(*clip) : bar.title), Rect{tx, r.y + 30, text_w, 20},
           TextStyle{15, Weight::Semibold}, kInk);
    g.text(to_wide(bar.detail), Rect{tx, r.y + 51, text_w, 16}, TextStyle{12.5f}, kMute);

    float bx = r.right() - 12 - buttons_w;
    const float by = r.y + (r.h - kControl) / 2;
    const bool exists = clip != nullptr || bar.file.empty();
    button(Rect{bx, by, 84, kControl}, L"Open", glyph::kPlay, Action::SavedOpen, {});
    bx += 84 + 2;
    button(Rect{bx, by, 118, kControl}, L"Open folder", glyph::kFolder, Action::SavedFolder, {});
    bx += 118 + 2;
    button(Rect{bx, by, 86, kControl}, L"Share", glyph::kShare, exists && !bar.file.empty() ? Action::SavedShare
                                                                                          : Action::SavedFolder,
           "share");
    bx += 86 + 2;
    if (exists && clip != nullptr && !clip->trashed_at) {
        button(Rect{bx, by, 84, kControl}, L"Delete", glyph::kTrash, Action::SavedTrash, {}, false, true);
    }
    bx += 84 + 2;
    button(Rect{bx, by, 32, kControl}, L"", glyph::kClose, Action::SavedClose, {});
}

// ---------------------------------------------------------------------------------------------
// Input

void App::on_mouse_move(float x, float y) {
    mouse_x_ = x;
    mouse_y_ = y;
    if (dragging_) {
        const auto track = tracks_.find(*dragging_);
        if (track != tracks_.end() && track->second.w > 0) {
            on_drag(*dragging_, std::clamp((x - track->second.x) / track->second.w, 0.0f, 1.0f), false);
        }
        return;
    }
    std::optional<Hit> now;
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (it->rect.contains(x, y)) {
            if (it->action != Action::None && it->action != Action::Disabled) now = *it;
            break;
        }
    }
    const bool changed = now.has_value() != hover_.has_value() ||
                         (now && (now->action != hover_->action || now->arg != hover_->arg));
    if (changed) {
        hover_ = now;
        invalidate();
    }
}

std::optional<App::Hit> App::hit_at(float x, float y) const {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (it->rect.contains(x, y)) return *it;  // a copy: run() may repaint and clear hits_
    }
    return std::nullopt;
}

void App::on_press(float x, float y, bool right) {
    pressed_ = hit_at(x, y);
    SetCapture(window_);
    if (pressed_ && pressed_->action == Action::Drag && !right) {
        // Dragging starts on the press: the value jumps to where the track was pressed.
        dragging_ = pressed_->arg;
        pressed_.reset();
        on_mouse_move(x, y);
    }
}

void App::on_click(float x, float y, bool right) {
    if (dragging_) {
        const std::string id = *std::exchange(dragging_, std::nullopt);
        if (GetCapture() == window_) ReleaseCapture();
        const auto track = tracks_.find(id);
        const float fraction = track != tracks_.end() && track->second.w > 0
                                   ? std::clamp((x - track->second.x) / track->second.w, 0.0f, 1.0f)
                                   : -1.0f;
        on_drag(id, fraction, true);
        on_mouse_move(x, y);
        return;
    }
    // Only a press and release on the same thing is a click: a release after dragging in from
    // elsewhere (or after a title-bar double-click maximised the window) does nothing.
    const std::optional<Hit> pressed = std::exchange(pressed_, std::nullopt);
    if (GetCapture() == window_) ReleaseCapture();
    const std::optional<Hit> hit = hit_at(x, y);
    if (!hit || !pressed || hit->action != pressed->action || hit->arg != pressed->arg) return;
    if (right && editing_) end_edit(true);  // a menu action may move or remove what is being renamed
    POINT screen{static_cast<LONG>(x * graphics_->scale()), static_cast<LONG>(y * graphics_->scale())};
    ClientToScreen(window_, &screen);
    // Right-click menus open where the mouse is.
    if (right) {
        mouse_x_ = x;
        mouse_y_ = y;
    }
    run(*hit, right, screen);
}

void App::on_wheel(float delta) {
    if (dialog_open() && !menu_) return;  // the page behind the Export or Share window stays put
    if (menu_) {
        menu_->scroll = std::max(0.0f, menu_->scroll + delta / 3);  // paint_menu clamps the end
        hover_.reset();
        invalidate();
        return;
    }
    if (page_ == Page::Library) {
        const float before = scroll_;
        scroll_ = std::clamp(scroll_ + delta, 0.0f, max_scroll_);
        if (scroll_ != before) {
            if (editing_) end_edit(true);
            invalidate();
        }
        return;
    }
    const float before = page_scroll_;
    page_scroll_ = std::clamp(page_scroll_ + delta, 0.0f, page_max_scroll_);
    if (page_scroll_ != before) {
        if (editing_) end_edit(true);
        invalidate();
    }
}

bool App::on_key(WPARAM key) {
    if (menu_) {
        if (key == VK_ESCAPE) close_menu();
        return true;  // keys do not reach the page under an open menu
    }
    if (dialog_open()) {
        if (key == VK_ESCAPE) {
            close_export();
            close_share();
        }
        return true;  // the clip page under the Export window does not react
    }
    if (page_ == Page::Clip && clip_view_) {
        switch (key) {
            case VK_SPACE: run_clip("play", Rect{}); return true;
            case VK_LEFT: run_clip("back5", Rect{}); return true;
            case VK_RIGHT: run_clip("fwd5", Rect{}); return true;
            case 'I': run_clip("set-in", Rect{}); return true;
            case 'O': run_clip("set-out", Rect{}); return true;
            case 'M': run_clip("mute", Rect{}); return true;
            case VK_ESCAPE: run_clip("back", Rect{}); return true;
            default: break;
        }
    }
    if (key == VK_ESCAPE && (page_ == Page::Settings || page_ == Page::Record || page_ == Page::Uploads)) {
        set_page(Page::Library);
        return true;
    }
    return false;
}

void App::set_page(Page page) {
    if (editing_) end_edit(true);
    close_menu();
    if (page_ == Page::Clip && page != Page::Clip) close_clip();
    if (page != page_) page_scroll_ = 0;
    if (capturing_hotkey_) {
        capturing_hotkey_ = false;
        apply_hotkey();
    }
    if (page == Page::Record || page == Page::Settings) refresh_devices();
    if (page == Page::Record) SetTimer(window_, kMeterTimer, kMeterMs, nullptr);
    page_ = page;
    invalidate();
}

void App::run(const Hit& hit, bool right, POINT screen) {
    const Rect anchor = hit.rect;
    switch (hit.action) {
        case Action::None:
        case Action::Disabled:
        case Action::Drag: break;
        case Action::MenuDismiss: close_menu(); break;
        case Action::MenuItem: {
            if (!menu_) break;
            const std::size_t index = static_cast<std::size_t>(std::stoul(hit.arg));
            if (index >= menu_->items.size() || !menu_->items[index].enabled || !menu_->items[index].run) break;
            const std::function<void()> chosen = menu_->items[index].run;  // a copy: closing frees the menu
            close_menu();
            chosen();
            invalidate();
            break;
        }
        case Action::Search:
            if (!dialog_open()) SetFocus(search_box_);  // not behind the Export or Share window
            break;
        case Action::Minimize: ShowWindow(window_, SW_MINIMIZE); break;
        case Action::Maximize: ShowWindow(window_, IsZoomed(window_) ? SW_RESTORE : SW_MAXIMIZE); break;
        case Action::Close: PostMessageW(window_, WM_CLOSE, 0, 0); break;
        case Action::View:
            if (right && hit.arg.starts_with("c") && library_.find_collection(hit.arg) != nullptr) {
                collection_menu(hit.arg, screen);
            } else if (hit.arg == "all") set_view(LibraryView::All);
            else if (hit.arg == "favorites") set_view(LibraryView::Favorites);
            else if (hit.arg == "shared") set_view(LibraryView::Shared);
            else if (hit.arg == "recent") set_view(LibraryView::Recent);
            else if (hit.arg == "trash") set_view(LibraryView::Trash);
            else set_view(LibraryView::Collection, hit.arg);
            break;
        case Action::NewCollection: {
            const std::string id = library_.create_collection(
                "New collection", static_cast<int>(library_.collections().size()) % kCollectionColorCount);
            save_library();
            begin_edit(Editing{Editing::Kind::CollectionName, id}, L"New collection");
            break;
        }
        case Action::Page:
            if (hit.arg == "record") set_page(Page::Record);
            else if (hit.arg == "uploads") set_page(Page::Uploads);
            else if (hit.arg == "settings") set_page(Page::Settings);
            break;
        case Action::SaveNow: save_clip(); break;
        case Action::TogglePause: toggle_pause(); break;
        case Action::Sort: sort_menu(anchor); break;
        case Action::OpenClip:
            if (right) {
                clip_menu(hit.arg, screen);
            } else {
                open_clip(hit.arg);
            }
            break;
        case Action::ToggleFavorite: {
            const ClipEntry* clip = library_.find(hit.arg);
            if (clip != nullptr) {
                library_.set_favorite(hit.arg, !clip->favorite);
                save_library();
                invalidate();
            }
            break;
        }
        case Action::SavedOpen:
            if (saved_bar_) {
                if (!saved_bar_->file.empty() && library_.find(saved_bar_->file) != nullptr) {
                    open_clip(saved_bar_->file);
                } else {
                    shell::open(saved_bar_->path);
                }
            }
            break;
        case Action::SavedFolder:
            if (saved_bar_) shell::show_in_folder(saved_bar_->path);
            break;
        case Action::SavedShare:
            if (saved_bar_ && library_.find(saved_bar_->file) != nullptr) {
                open_clip(saved_bar_->file);
                open_share();
            }
            break;
        case Action::SavedTrash:
            if (saved_bar_ && !saved_bar_->file.empty()) {
                move_to_trash(saved_bar_->file);
            }
            break;
        case Action::SavedClose:
            saved_bar_.reset();
            invalidate();
            break;
        case Action::MoreCollections: more_collections_menu(anchor); break;
        case Action::Clip:
            if (hit.arg.starts_with("ex-")) run_export(hit.arg, anchor);
            else if (hit.arg.starts_with("sh-")) run_share(hit.arg, anchor);
            else run_clip(hit.arg, anchor);
            break;
        case Action::Record: run_record(hit.arg, anchor); break;
        case Action::Setting: run_setting(hit.arg, anchor); break;
    }
}

void App::move_to_trash(const std::string& file) {
    if (library_.find(file) == nullptr) return;
    library_.move_to_trash(file, now_unix());
    new_files_.erase(file);
    if (saved_bar_ && saved_bar_->file == file) saved_bar_.reset();
    save_library();
    invalidate();
}

void App::recycle_now(const std::string& file) {
    if (!clips_folder_available() || recycling_ || library_.find(file) == nullptr) return;
    if (clip_view_ && clip_view_->file == file) close_clip();  // the player holds the file open
    recycling_ = true;
    const std::vector<fs::path> failed = shell::recycle({clip_path(file)});
    recycling_ = false;
    if (quit_pending_) {
        quit();
        return;
    }
    if (editing_) end_edit(true);  // a rename started while waiting: the cards are about to move
    if (library_.find(file) == nullptr) return;  // gone meanwhile (a scan while waiting)
    if (failed.empty()) {
        library_.remove(file);
        if (saved_bar_ && saved_bar_->file == file) saved_bar_.reset();
        save_library();
    } else {
        show_notice(L"Could not move the clip to the Recycle Bin. It may be open in a player, or its drive has "
                    L"no Recycle Bin.");
    }
    invalidate();
}

void App::sort_menu(const Rect& anchor) {
    Menu menu;
    menu.anchor = anchor;
    menu.min_width = anchor.w;
    for (const SortName& s : kSortNames) {
        const ClipSort sort = s.sort;
        menu.items.push_back(MenuItem{s.label, [this, sort] {
                                          sort_ = sort;
                                          scroll_ = 0;
                                      },
                                      s.sort == sort_});
    }
    open_menu(std::move(menu));
}

void App::add_to_collection_menu(const std::string& file, const Rect& anchor) {
    const ClipEntry* clip = library_.find(file);
    if (clip == nullptr) return;
    Menu menu;
    menu.anchor = anchor;
    menu.min_width = std::max(anchor.w, 210.0f);
    for (const Collection& c : library_.collections()) {
        const bool member = std::find(clip->collections.begin(), clip->collections.end(), c.id) != clip->collections.end();
        const std::string id = c.id;
        MenuItem item{to_wide(c.name.empty() ? std::string("Untitled") : c.name), [this, file, id] {
                          const ClipEntry* now = library_.find(file);
                          if (now == nullptr || library_.find_collection(id) == nullptr) return;
                          const bool in = std::find(now->collections.begin(), now->collections.end(), id) !=
                                          now->collections.end();
                          library_.set_in_collection(file, id, !in);
                          save_library();
                      },
                      member};
        item.dot = kCollectionColors[std::clamp(c.color, 0, kCollectionColorCount - 1)];
        menu.items.push_back(std::move(item));
    }
    if (!menu.items.empty()) menu.items.push_back(MenuItem{});
    menu.items.push_back(MenuItem{L"New collection", [this, file] {
                                      if (library_.find(file) == nullptr) return;
                                      const std::string id = library_.create_collection(
                                          "New collection",
                                          static_cast<int>(library_.collections().size()) % kCollectionColorCount);
                                      library_.set_in_collection(file, id, true);
                                      save_library();
                                      if (page_ == Page::Clip) set_page(Page::Library);
                                      begin_edit(Editing{Editing::Kind::CollectionName, id}, L"New collection");
                                  }});
    open_menu(std::move(menu));
}

void App::clip_menu(const std::string& file, POINT) {
    const ClipEntry* clip = library_.find(file);
    if (clip == nullptr) return;
    Menu menu;
    menu.anchor = Rect{mouse_x_, mouse_y_, 0, 0};
    menu.at_point = true;
    menu.items.push_back(MenuItem{L"Open", [this, file] { open_clip(file); }});
    menu.items.push_back(MenuItem{L"Play in your video player", [this, file] {
                                      new_files_.erase(file);
                                      if (clips_folder_available()) shell::open(clip_path(file));
                                  }});
    if (const ClipEntry* c = library_.find(file); c != nullptr && share_link_live(*c, now_unix())) {
        const std::wstring url = to_wide(c->share_url);
        menu.items.push_back(MenuItem{L"Copy share link", [this, url] {
                                          if (copy_text(url)) show_notice(L"Link copied. Paste it in Discord.");
                                      }});
    }
    menu.items.push_back(MenuItem{L"Show in folder", [this, file] {
                                      if (clips_folder_available()) shell::show_in_folder(clip_path(file));
                                  }});
    menu.items.push_back(MenuItem{});
    if (!clip->trashed_at) {
        menu.items.push_back(MenuItem{clip->favorite ? L"Remove from favorites" : L"Add to favorites", [this, file] {
                                          if (const ClipEntry* c = library_.find(file)) {
                                              library_.set_favorite(file, !c->favorite);
                                              save_library();
                                          }
                                      }});
        menu.items.push_back(MenuItem{L"Rename", [this, file] {
                                          if (const ClipEntry* c = library_.find(file)) {
                                              begin_edit(Editing{Editing::Kind::ClipTitle, file}, to_wide(display_title(*c)));
                                          }
                                      }});
        const Rect at{mouse_x_, mouse_y_, 0, 0};
        menu.items.push_back(MenuItem{L"Add to collection...", [this, file, at] {
                                          Rect anchor = at;
                                          anchor.w = 1;
                                          add_to_collection_menu(file, anchor);
                                      }});
        menu.items.push_back(MenuItem{});
        MenuItem trash{L"Move to Trash", [this, file] { move_to_trash(file); }};
        trash.danger = true;
        menu.items.push_back(std::move(trash));
    } else {
        menu.items.push_back(MenuItem{L"Restore", [this, file] {
                                          library_.restore(file);
                                          save_library();
                                      }});
        MenuItem recycle{L"Move to Recycle Bin now", [this, file] { recycle_now(file); }};
        recycle.danger = true;
        menu.items.push_back(std::move(recycle));
    }
    open_menu(std::move(menu));
}

void App::collection_menu(const std::string& id, POINT) {
    if (library_.find_collection(id) == nullptr) return;
    Menu menu;
    menu.anchor = Rect{mouse_x_, mouse_y_, 0, 0};
    menu.at_point = true;
    menu.items.push_back(MenuItem{L"Rename", [this, id] {
                                      if (const Collection* c = library_.find_collection(id)) {
                                          begin_edit(Editing{Editing::Kind::CollectionName, id}, to_wide(c->name));
                                      }
                                  }});
    MenuItem remove{L"Delete collection (keeps the clips)", [this, id] {
                        library_.delete_collection(id);
                        if (view_ == LibraryView::Collection && view_collection_ == id) set_view(LibraryView::All);
                        save_library();
                    }};
    remove.danger = true;
    menu.items.push_back(std::move(remove));
    open_menu(std::move(menu));
}

void App::more_collections_menu(const Rect& anchor) {
    Menu menu;
    menu.anchor = anchor;
    menu.min_width = anchor.w;
    const auto& collections = library_.collections();
    for (const std::size_t index : hidden_collections_) {
        if (index >= collections.size()) continue;
        const Collection& c = collections[index];
        const std::string id = c.id;
        MenuItem item{to_wide(c.name.empty() ? std::string("Untitled") : c.name), [this, id] {
                          if (library_.find_collection(id) != nullptr) set_view(LibraryView::Collection, id);
                      }};
        item.dot = kCollectionColors[std::clamp(c.color, 0, kCollectionColorCount - 1)];
        menu.items.push_back(std::move(item));
    }
    open_menu(std::move(menu));
}

void App::begin_edit(Editing editing, const std::wstring& text) {
    if (editing_) end_edit(true);
    if (editing.kind == Editing::Kind::CollectionName) {
        set_view(LibraryView::Collection, editing.target);  // show what is being named
    }
    editing_ = std::move(editing);
    // The next paint finds where the item is and stores it in edit_rect_.
    edit_rect_ = Rect{};
    edit_positioned_ = false;
    invalidate();
    UpdateWindow(window_);
    if (!editing_ || edit_rect_.w <= 0) {
        editing_.reset();  // the item is not on screen (scrolled away, filtered out)
        return;
    }
    const float scale = graphics_->scale();
    MoveWindow(edit_box_, static_cast<int>(std::lround(edit_rect_.x * scale)),
               static_cast<int>(std::lround(edit_rect_.y * scale)), static_cast<int>(std::lround(edit_rect_.w * scale)),
               static_cast<int>(std::lround(edit_rect_.h * scale)), FALSE);
    edit_positioned_ = true;
    SetWindowTextW(edit_box_, text.c_str());
    ShowWindow(edit_box_, SW_SHOW);
    SetFocus(edit_box_);
    SendMessageW(edit_box_, EM_SETSEL, 0, -1);
}

void App::end_edit(bool commit) {
    if (!editing_) return;
    const Editing editing = *editing_;
    editing_.reset();  // first: hiding the box sends EN_KILLFOCUS, which calls end_edit again
    const std::string text = to_utf8(window_text(edit_box_));
    ShowWindow(edit_box_, SW_HIDE);
    if (GetFocus() == edit_box_ || GetFocus() == nullptr) SetFocus(window_);
    if (commit) {
        if (editing.kind == Editing::Kind::CollectionName) {
            if (!text.empty() && text.find_first_not_of(" \t") != std::string::npos) {
                library_.rename_collection(editing.target, text);
            }
        } else if (const ClipEntry* clip = library_.find(editing.target);
                   clip != nullptr && text != display_title(*clip)) {
            library_.set_title(editing.target, text);  // empty: back to the file name
        }
        save_library();
    }
    invalidate();
}

}  // namespace bt::ui
