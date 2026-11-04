// Backtrack console host. Until the real UI exists this window stands in for it: the title shows
// what Backtrack is doing, single keys replace the menu, and a sound confirms each saved clip.
// The recording engine is bt::Recorder; the UI will drive the same object.
//
// Keys: S save clip, P pause/resume, O open clips folder, E edit settings, L open log, Q quit.
// Test options: --save-after <s> (repeatable), --pause-after <s>, --resume-after <s>,
//               --lose-capture-every <s>, --exit-after <s>

#include "core/Config.h"
#include "core/Hotkey.h"
#include "platform/Com.h"
#include "platform/Log.h"
#include "platform/Recorder.h"

#include <mfapi.h>
#include <mmsystem.h>
#include <shellapi.h>

#include <atomic>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

// core/Hotkey uses the Win32 values without including <windows.h>; make sure they agree.
static_assert(bt::hotkey_mod::kAlt == MOD_ALT && bt::hotkey_mod::kCtrl == MOD_CONTROL &&
              bt::hotkey_mod::kShift == MOD_SHIFT && bt::hotkey_mod::kWin == MOD_WIN);
static_assert(VK_F1 == 0x70 && VK_F12 == 0x7B && VK_F24 == 0x87 && VK_NUMPAD0 == 0x60 && VK_HOME == 0x24 &&
              VK_END == 0x23 && VK_INSERT == 0x2D && VK_DELETE == 0x2E && VK_PRIOR == 0x21 && VK_NEXT == 0x22 &&
              VK_SPACE == 0x20 && VK_PAUSE == 0x13 && VK_SNAPSHOT == 0x2C);

constexpr UINT kMsgCommand = WM_APP + 1;  // wParam: a Command
constexpr UINT kMsgSaved = WM_APP + 2;
constexpr int kHotkeyId = 1;
constexpr UINT kStatusRefreshMs = 1000;
constexpr UINT kMillisecondsPerSecond = 1000;
constexpr DWORD kKeyPollMs = 200;

// Windows ends the process 5 s after a close/logoff/shutdown event; leave margin for that.
constexpr DWORD kCloseWaitMs = 4500;

// §8: a second copy exits instead of recording twice. Per session, so two users can each run one.
constexpr wchar_t kInstanceMutexName[] = L"Local\\Backtrack.SingleInstance";

enum class Command : WPARAM {
    Save = 's',
    Pause = 'p',
    OpenFolder = 'o',
    EditSettings = 'e',
    OpenLog = 'l',
    Quit = 'q',
    LoseCapture = 'x',  // test option only, no key
    Resume = 'r',  // test option only, no key
};

DWORD g_main_thread = 0;
HANDLE g_finished = nullptr;  // set when run() has cleaned up

BOOL WINAPI on_console_ctrl(DWORD event) {
    if (!PostThreadMessageW(g_main_thread, WM_QUIT, 0, 0)) {
        return FALSE;  // let Windows apply its default (end the process) rather than lose the press
    }
    if (event == CTRL_CLOSE_EVENT || event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        // Returning ends the process at once; wait so a clip being written is finished (§6).
        WaitForSingleObject(g_finished, kCloseWaitMs);
    }
    return TRUE;
}

std::string path_text(const fs::path& path) {
    return bt::to_utf8(path.wstring());  // path::string() throws for names outside the ANSI code page
}

fs::path exe_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        BT_CHECK_WIN32(length != 0);
        if (length < buffer.size()) {
            buffer.resize(length);
            return fs::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::optional<std::string> read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::stringstream text;
    text << file.rdbuf();
    if (file.bad()) {
        return std::nullopt;
    }
    return text.str();
}

// Notepad saves as UTF-8, "Unicode" (UTF-16 LE), "Unicode big endian" or ANSI. The parser wants
// UTF-8, so the others are converted here instead of failing on every line.
std::string ini_text_as_utf8(const std::string& bytes) {
    const bool utf16le = bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
                         static_cast<unsigned char>(bytes[1]) == 0xFE;
    const bool utf16be = bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE &&
                         static_cast<unsigned char>(bytes[1]) == 0xFF;
    if (utf16le || utf16be) {
        std::wstring wide;
        for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
            const auto lo = static_cast<unsigned char>(bytes[utf16le ? i : i + 1]);
            const auto hi = static_cast<unsigned char>(bytes[utf16le ? i + 1 : i]);
            wide.push_back(static_cast<wchar_t>(lo | (hi << 8)));
        }
        bt::log::info("backtrack.ini is saved as UTF-16, reading it as such");
        return bt::to_utf8(wide);
    }
    const int valid_utf8 = bytes.empty() ? 1
        : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (valid_utf8 == 0) {
        // Not UTF-8: an "ANSI" save with an accented path. Read it in the system code page.
        const int length = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(), length);
        bt::log::info("backtrack.ini is not UTF-8, reading it in the system code page");
        return bt::to_utf8(wide);
    }
    return bytes;
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
    } else if (const auto bytes = read_file(ini_path)) {
        text = ini_text_as_utf8(*bytes);
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

struct ScheduledCommand {
    UINT after_s = 0;
    Command command = Command::Save;
    bool repeat = false;
    UINT_PTR timer = 0;
};

std::vector<ScheduledCommand> parse_options(int argc, wchar_t** argv) {
    std::vector<ScheduledCommand> schedule;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::wstring name = argv[i];
        const UINT seconds = static_cast<UINT>(std::wcstoul(argv[i + 1], nullptr, 10));
        if (name == L"--save-after") schedule.push_back({seconds, Command::Save});
        else if (name == L"--pause-after") schedule.push_back({seconds, Command::Pause});
        else if (name == L"--resume-after") schedule.push_back({seconds, Command::Resume});
        else if (name == L"--exit-after") schedule.push_back({seconds, Command::Quit});
        else if (name == L"--lose-capture-every") schedule.push_back({seconds, Command::LoseCapture, true});
        else bt::log::warn("Unknown option {}", bt::to_utf8(name));
    }
    return schedule;
}

void disable_quick_edit() {
    // With QuickEdit on, clicking the console pauses all output; every thread that logs would
    // then block on its next line.
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input != INVALID_HANDLE_VALUE && GetConsoleMode(input, &mode)) {
        SetConsoleMode(input, (mode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);
    }
}

// Reads single key presses from the console and posts them to the main thread as commands.
class ConsoleKeys {
public:
    ConsoleKeys() : thread_(&ConsoleKeys::run, this) {}
    ~ConsoleKeys() {
        stop_ = true;
        thread_.join();
    }
    ConsoleKeys(const ConsoleKeys&) = delete;
    ConsoleKeys& operator=(const ConsoleKeys&) = delete;

private:
    void run() {
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        if (input == INVALID_HANDLE_VALUE || !GetConsoleMode(input, &mode)) {
            return;  // input is redirected: no keys, only the hotkey and test options
        }
        while (!stop_) {
            if (WaitForSingleObject(input, kKeyPollMs) != WAIT_OBJECT_0) {
                continue;
            }
            INPUT_RECORD record{};
            DWORD read = 0;
            if (!ReadConsoleInputW(input, &record, 1, &read) || read == 0 || record.EventType != KEY_EVENT ||
                !record.Event.KeyEvent.bKeyDown) {
                continue;
            }
            const auto key = static_cast<wchar_t>(std::towlower(record.Event.KeyEvent.uChar.UnicodeChar));
            if (key == L's' || key == L'p' || key == L'o' || key == L'e' || key == L'l' || key == L'q') {
                PostThreadMessageW(g_main_thread, kMsgCommand, static_cast<WPARAM>(key), 0);
            }
        }
    }

    std::atomic<bool> stop_{false};
    std::thread thread_;  // last: starts in the constructor and uses stop_
};

// ShellExecute wants a single-threaded apartment; the main thread is multithreaded.
void open_in_shell(const fs::path& target) {
    std::thread([target] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const auto result = reinterpret_cast<INT_PTR>(
            ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        constexpr INT_PTR kShellExecuteSuccess = 32;  // ShellExecute returns > 32 on success
        if (result <= kShellExecuteSuccess) {
            bt::log::warn("Could not open {} (error {})", path_text(target), result);
        }
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
    }).join();
}

void show_status(const bt::Recorder& recorder) {
    SetConsoleTitleW((L"Backtrack - " + bt::to_wide(recorder.status_text())).c_str());
}

// Without a window in front of a fullscreen game, sound is the only feedback: the "asterisk" for a
// saved clip, the "critical stop" for a refused or failed one.
void play_feedback(const bt::Settings& settings, bool ok) {
    if (settings.sound) {
        PlaySoundW(ok ? L"SystemAsterisk" : L"SystemHand", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
    }
}

void request_save(bt::Recorder& recorder, const bt::Settings& settings) {
    std::string reason;
    if (recorder.save_clip(reason)) {
        bt::log::info("Saving the last {} seconds...", settings.clip_seconds);
    } else {
        bt::log::warn("{}", reason);
        play_feedback(settings, false);
    }
}

int run(int argc, wchar_t** argv) {
    // DuplicateOutput1 refuses processes that are not per-monitor DPI aware, and DPI virtualisation
    // would report scaled monitor sizes.
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        bt::log::warn("SetProcessDpiAwarenessContext failed (error {})", GetLastError());
    }
    SetConsoleOutputCP(CP_UTF8);
    disable_quick_edit();
    bt::log::set_console_echo(true);

    // The message queue must exist before Ctrl+C or a saved clip can post to it.
    MSG peek{};
    PeekMessageW(&peek, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    g_main_thread = GetCurrentThreadId();
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);

    const bt::UniqueHandle instance(CreateMutexW(nullptr, FALSE, kInstanceMutexName));
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::printf("Backtrack is already running.\n");
        return 0;
    }

    const fs::path exe_dir = exe_directory();
    const fs::path ini_path = exe_dir / L"backtrack.ini";
    const fs::path log_path = exe_dir / L"backtrack.log";
    const bt::Settings settings = load_config(ini_path);
    bt::log::init(log_path, settings.log_level);
    bt::log::info("Backtrack starting");
    std::vector<ScheduledCommand> schedule = parse_options(argc, argv);

    bt::ComApartment com(COINIT_MULTITHREADED);
    BT_CHECK_HR(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    struct MfShutdown {
        ~MfShutdown() { MFShutdown(); }  // §6: after every pipeline object is gone
    } mf_shutdown;

    bt::Recorder recorder(settings, [&settings](const bt::SaveResult& result) {
        // Writer thread. §8: a sound either way and the log line (the writer logs failures itself);
        // the UI will show it.
        if (result.ok) {
            bt::log::info("{}", result.message);
        }
        play_feedback(settings, result.ok);
        PostThreadMessageW(g_main_thread, kMsgSaved, 0, 0);
    });
    recorder.start();

    const bt::Hotkey hotkey = *bt::parse_hotkey(settings.save_hotkey).hotkey;
    const bool hotkey_ok = RegisterHotKey(nullptr, kHotkeyId, hotkey.modifiers | MOD_NOREPEAT, hotkey.virtual_key);
    if (!hotkey_ok) {
        // §5.2: still usable through the S key.
        bt::log::warn("Cannot use {} as the hotkey: {}. Press S in this window to save instead, or pick another "
                      "key in backtrack.ini",
                      settings.save_hotkey, bt::hresult_text(HRESULT_FROM_WIN32(GetLastError())));
    }
    std::printf("\nBacktrack keeps the last %d seconds. %s saves them.\n"
                "Keys in this window: S save  P pause/resume  O open clips folder  E edit settings  L open log  "
                "Q quit\n\n",
                settings.clip_seconds,
                hotkey_ok ? (settings.save_hotkey + " (anywhere) or S here").c_str() : "S here");

    const ConsoleKeys keys;
    const UINT_PTR status_timer = SetTimer(nullptr, 0, kStatusRefreshMs, nullptr);
    for (ScheduledCommand& item : schedule) {
        item.timer = SetTimer(nullptr, 0, item.after_s * kMillisecondsPerSecond, nullptr);
    }
    show_status(recorder);

    auto execute = [&](Command command) -> bool {
        switch (command) {
            case Command::Save: request_save(recorder, settings); break;
            case Command::Pause:
                if (recorder.paused()) {
                    recorder.resume();
                } else {
                    recorder.pause();
                }
                break;
            case Command::Resume: recorder.resume(); break;
            case Command::OpenFolder: open_in_shell(recorder.output_dir()); break;
            case Command::EditSettings:
                bt::log::info("Restart Backtrack after editing the settings");
                open_in_shell(ini_path);
                break;
            case Command::OpenLog: open_in_shell(log_path); break;
            case Command::LoseCapture:
                bt::log::info("Simulating a lost capture");
                recorder.simulate_capture_loss();
                break;
            case Command::Quit: return false;
        }
        show_status(recorder);
        return true;
    };

    MSG msg{};
    bool running = true;
    while (running && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY && msg.wParam == kHotkeyId) {
            running = execute(Command::Save);
        } else if (msg.message == kMsgCommand) {
            running = execute(static_cast<Command>(msg.wParam));
        } else if (msg.message == kMsgSaved || (msg.message == WM_TIMER && msg.wParam == status_timer)) {
            show_status(recorder);
        } else if (msg.message == WM_TIMER) {
            for (const ScheduledCommand& item : schedule) {
                if (item.timer != 0 && msg.wParam == item.timer) {
                    if (!item.repeat) {
                        KillTimer(nullptr, item.timer);
                    }
                    running = execute(item.command);
                    break;
                }
            }
        }
    }

    if (recorder.saving()) {
        bt::log::info("Finishing the clip that is being saved...");
        SetConsoleTitleW(L"Backtrack - Saving clip...");
    }
    recorder.stop();
    if (hotkey_ok) {
        UnregisterHotKey(nullptr, kHotkeyId);
    }
    bt::log::info("Backtrack stopped");
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    g_finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    int code = 1;
    try {
        code = run(argc, argv);
    } catch (const std::exception& e) {
        bt::log::error("Fatal: {}", e.what());
    } catch (...) {
        bt::log::error("Fatal: unknown exception");
    }
    bt::log::shutdown();
    if (g_finished != nullptr) {
        SetEvent(g_finished);
    }
    return code;
}
