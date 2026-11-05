// Backtrack: the window app. Starts the recording engine (bt::Recorder), opens the window
// (bt::ui::App) and runs the message loop until "Quit" in the tray menu or a shutdown.
//
// Test options: --save-after <s> (repeatable), --pause-after <s>, --resume-after <s>,
//               --lose-capture-every <s>, --exit-after <s>, --hidden (start in the tray)

#include "core/Config.h"
#include "platform/Com.h"
#include "platform/IniFile.h"
#include "platform/Log.h"
#include "platform/Recorder.h"
#include "platform/Shell.h"
#include "platform/Text.h"
#include "ui/App.h"

#include <mfapi.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

// §8: a second copy shows the running window instead of recording twice. Per session, so two
// users can each run one.
constexpr wchar_t kInstanceMutexName[] = L"Local\\Backtrack.SingleInstance";
// The taskbar and Start group the window and the Start menu shortcut under this one name.
constexpr wchar_t kAppUserModelId[] = L"Backtrack.Backtrack";
constexpr UINT kMillisecondsPerSecond = 1000;
constexpr unsigned kShellWaitMs = 2000;

std::string path_text(const fs::path& path) {
    return bt::to_utf8(path.wstring());  // path::string() throws for names outside the ANSI code page
}

fs::path exe_path() {
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        BT_CHECK_WIN32(length != 0);
        if (length < buffer.size()) {
            buffer.resize(length);
            return fs::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
}

fs::path exe_directory() {
    return exe_path().parent_path();
}

// §5.1: written with defaults when missing; never refuses to start because of the config.
bt::Settings load_config(const fs::path& ini_path) {
    std::string text;
    std::error_code ec;
    if (!fs::exists(ini_path, ec) && !ec) {
        std::ofstream out(ini_path, std::ios::binary);
        out << bt::default_ini_text();
        if (out) {
            bt::log::info("Wrote default settings to {}", path_text(ini_path));
        } else {
            bt::log::warn("Could not write default settings to {}, using built-in defaults", path_text(ini_path));
        }
        text = std::string(bt::default_ini_text());
    } else if (const auto read = bt::read_ini_file(ini_path)) {
        text = *read;
    } else {
        // Never overwrite a file we merely failed to read (locked by a sync tool, no permission).
        bt::log::warn("Could not read {}, using built-in defaults for this run", path_text(ini_path));
    }
    bt::LoadedSettings loaded = bt::load_settings(text);
    for (const std::string& warning : loaded.warnings) {
        bt::log::warn("backtrack.ini {}", warning);
    }
    return loaded.settings;
}


enum class Command { Save, Pause, Resume, LoseCapture, Quit };

struct ScheduledCommand {
    UINT after_s = 0;
    Command command = Command::Save;
    bool repeat = false;
    UINT_PTR timer = 0;
};

struct Options {
    std::vector<ScheduledCommand> schedule;
    bool hidden = false;
};

// The test options run from timer callbacks, not from the main loop: a menu or a window being
// dragged runs its own message loop, which calls these but would drop plain timer messages.
Options* g_options = nullptr;
bt::ui::App* g_app = nullptr;

void CALLBACK on_scheduled(HWND, UINT, UINT_PTR timer, DWORD) {
    if (g_options == nullptr || g_app == nullptr) return;
    for (const ScheduledCommand& item : g_options->schedule) {
        if (item.timer == 0 || timer != item.timer) continue;
        if (!item.repeat) KillTimer(nullptr, item.timer);
        try {
            switch (item.command) {
                case Command::Save: g_app->save_clip(); break;
                case Command::Pause:
                    if (!g_app->recorder().paused()) g_app->toggle_pause();
                    break;
                case Command::Resume:
                    if (g_app->recorder().paused()) g_app->toggle_pause();
                    break;
                case Command::LoseCapture:
                    bt::log::info("Simulating a lost capture");
                    g_app->recorder().simulate_capture_loss();
                    break;
                case Command::Quit: g_app->quit(); break;
            }
        } catch (const std::exception& e) {
            bt::log::error("Test option failed: {}", e.what());  // a callback must not throw into user32
        }
        return;
    }
}

// Library and thumbnails live next to the exe like the settings, unless that folder is read-only
// (Program Files): then in %LOCALAPPDATA%\Backtrack, so favorites and collections are not lost.
fs::path data_directory(const fs::path& exe_dir) {
    const fs::path probe = exe_dir / L"backtrack.write-test";
    const HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
        return exe_dir;
    }
    wchar_t local[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return exe_dir;
    }
    const fs::path dir = fs::path(local) / L"Backtrack";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)) {
        return exe_dir;  // no better place; saving fails visibly (a notice in the window)
    }
    bt::log::info("{} is read-only; the library is kept in {}", path_text(exe_dir), path_text(dir));
    return dir;
}

Options parse_options() {
    Options options;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return options;
    }
    for (int i = 1; i < argc; ++i) {
        const std::wstring name = argv[i];
        if (name == L"--hidden") {
            options.hidden = true;
            continue;
        }
        if (i + 1 >= argc) {
            bt::log::warn("Option {} needs a number of seconds", bt::to_utf8(name));
            break;
        }
        const UINT seconds = static_cast<UINT>(std::wcstoul(argv[++i], nullptr, 10));
        if (name == L"--save-after") options.schedule.push_back({seconds, Command::Save});
        else if (name == L"--pause-after") options.schedule.push_back({seconds, Command::Pause});
        else if (name == L"--resume-after") options.schedule.push_back({seconds, Command::Resume});
        else if (name == L"--exit-after") options.schedule.push_back({seconds, Command::Quit});
        else if (name == L"--lose-capture-every") options.schedule.push_back({seconds, Command::LoseCapture, true});
        else bt::log::warn("Unknown option {}", bt::to_utf8(name));
    }
    LocalFree(argv);
    return options;
}

int run(int show_command) {
    // DuplicateOutput1 refuses processes that are not per-monitor DPI aware, and DPI virtualisation
    // would report scaled monitor sizes. The manifest says the same; this covers a stripped one.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetCurrentProcessExplicitAppUserModelID(kAppUserModelId);

    const bt::UniqueHandle instance(CreateMutexW(nullptr, FALSE, kInstanceMutexName));
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // The running copy brings its window forward; it may, because this process was just
        // started by the user and hands over its right to take the foreground.
        if (const HWND running = FindWindowW(bt::ui::App::kWindowClass, nullptr); running != nullptr) {
            DWORD process = 0;
            GetWindowThreadProcessId(running, &process);
            AllowSetForegroundWindow(process);
            PostMessageW(running, RegisterWindowMessageW(bt::ui::App::kShowMessage), 0, 0);
        }
        return 0;
    }

    const fs::path exe_dir = exe_directory();
    bt::ui::AppPaths paths;
    paths.exe_dir = exe_dir;
    paths.ini = exe_dir / L"backtrack.ini";
    // Lines logged before init() (the read-only note, settings warnings) are kept and written then.
    const fs::path data_dir = data_directory(exe_dir);
    paths.log = data_dir / L"backtrack.log";
    paths.library = data_dir / L"library.json";
    paths.cache = data_dir / L"cache" / L"thumbnails";
    const bt::Settings settings = load_config(paths.ini);
    bt::log::init(paths.log, settings.log_level);
    bt::log::info("Backtrack starting");
    Options options = parse_options();

    bt::ComApartment com(COINIT_MULTITHREADED);
    BT_CHECK_HR(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    struct MfShutdown {
        ~MfShutdown() {  // §6: after every pipeline object is gone
            const HRESULT hr = MFShutdown();
            if (FAILED(hr)) {
                bt::log::warn("MFShutdown failed: {}", bt::hresult_text(hr));
            }
        }
    } mf_shutdown;

    // Found by searching after pressing the Windows key.
    bt::shell::ensure_start_menu_shortcut(exe_path(), kAppUserModelId);

    bt::ui::SavedInbox inbox;
    // The window owns the recorder and makes a new one when a setting needs a new pipeline.
    const bt::ui::RecorderFactory make_recorder = [&exe_dir, &inbox](const bt::Settings& current) {
        return std::make_unique<bt::Recorder>(current, exe_dir, [&inbox](const bt::SaveResult& result) {
            // Writer thread. §8: the log line here, the sound in the inbox (the writer logs failures).
            if (result.ok) {
                bt::log::info("{}", result.message);
            }
            inbox.push(result);
        });
    };

    int code = 0;
    {
        bt::ui::App app(settings, paths, make_recorder, inbox);
        app.show(options.hidden ? SW_HIDE : show_command);

        g_options = &options;
        g_app = &app;
        struct ClearGlobals {
            ~ClearGlobals() {
                g_options = nullptr;
                g_app = nullptr;
            }
        } clear_globals;
        for (ScheduledCommand& item : options.schedule) {
            item.timer = SetTimer(nullptr, 0, item.after_s * kMillisecondsPerSecond, on_scheduled);
        }
        MSG msg{};
        int got = 0;
        while ((got = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (got < 0) {
            bt::log::error("The message loop failed: {}", bt::hresult_text(HRESULT_FROM_WIN32(GetLastError())));
            code = 1;
        }
        app.finish();  // §6: every queued clip is written before Backtrack ends, and goes into the library
    }
    // An Explorer window still opening must not log into a log that is being torn down.
    bt::shell::wait_for_pending(kShellWaitMs);
    bt::log::info("Backtrack stopped");
    return code;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int show_command) {
    int code = 1;
    try {
        code = run(show_command);
    } catch (const std::exception& e) {
        bt::log::error("Fatal: {}", e.what());
        bt::log::shutdown();
        // A quit message left in the queue would close the message box before it is seen.
        MSG quit{};
        while (PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {
        }
        const std::wstring text = L"Backtrack stopped because of an error:\n\n" + bt::to_wide(e.what()) +
                                  L"\n\nMore details are in backtrack.log (next to backtrack.exe, or in "
                                  L"%LOCALAPPDATA%\\Backtrack when that folder is read-only).";
        MessageBoxW(nullptr, text.c_str(), L"Backtrack", MB_OK | MB_ICONERROR);
        return 1;
    } catch (...) {
        bt::log::error("Fatal: unknown exception");
        bt::log::shutdown();
        MSG quit{};
        while (PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {
        }
        MessageBoxW(nullptr, L"Backtrack stopped because of an unknown error. See backtrack.log.", L"Backtrack",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    bt::log::shutdown();
    return code;
}
