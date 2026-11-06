// Development tool: runs the updater (platform/Updater.h) without the window and prints each state
// it posts, so the check, the download and its verification can be tried against the real GitHub.
// Not shipped.
//
//   update_check                          checks Findiepman/BackTrack as this build's version
//   update_check --current 0.0.1          pretends to be an older Backtrack
//   update_check --repo cli/cli --asset gh_ _checksums.txt --current 0.0.1 --download --folder <dir>
//                                         downloads and verifies a real release asset of another repository
//   update_check --auto                   the automatic mode: no check() call, waits for the
//                                         scheduled one (90 s after the start without a recent check)
//   update_check --cancel-after-ms <ms>   ends the updater that long after the start (quitting mid-download)
//   update_check --install-now | --install-on-exit
//                                         runs the waiting installer like the window would (only does
//                                         something when this exe is in the installed folder)

#include "platform/Log.h"
#include "platform/Text.h"
#include "platform/Updater.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <string>

namespace {

constexpr UINT kMsgUpdate = WM_APP + 1;
constexpr UINT_PTR kCancelTimer = 1;

const char* stage_name(bt::UpdateStage stage) {
    switch (stage) {
        case bt::UpdateStage::Idle: return "Idle";
        case bt::UpdateStage::Checking: return "Checking";
        case bt::UpdateStage::UpToDate: return "UpToDate";
        case bt::UpdateStage::Available: return "Available";
        case bt::UpdateStage::Downloading: return "Downloading";
        case bt::UpdateStage::Ready: return "Ready";
        case bt::UpdateStage::Failed: return "Failed";
    }
    return "?";
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

void print(const bt::UpdateState& s) {
    if (s.stage == bt::UpdateStage::Downloading) {
        std::printf("Downloading %s: %.0f%%\n", s.version.c_str(), s.fraction * 100.0);
        return;
    }
    std::printf("%s", stage_name(s.stage));
    if (!s.version.empty()) std::printf(" %s", s.version.c_str());
    if (s.stage == bt::UpdateStage::Available || s.stage == bt::UpdateStage::Ready) {
        std::printf(" (can_install %s)", s.can_install ? "yes" : "no, show the page");
    }
    if (!s.error.empty()) std::printf(": %s", s.error.c_str());
    std::printf("\n");
    if (s.stage == bt::UpdateStage::UpToDate) std::printf("  You are up to date.\n");
    if (s.stage == bt::UpdateStage::Available || s.stage == bt::UpdateStage::Ready) {
        std::printf("  page: %s\n", s.page_url.c_str());
        if (!s.notes.empty()) std::printf("  notes: %.200s\n", s.notes.c_str());
    }
    std::fflush(stdout);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bt::UpdaterOptions options;
    options.current_version = BT_VERSION;
    options.exe_path = exe_path();
    bool download = false;
    bool install_now = false;
    bool install_on_exit = false;
    bool automatic = false;
    UINT cancel_after_ms = 0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring name = argv[i];
        const bool has_value = i + 1 < argc;
        if (name == L"--current" && has_value) {
            options.current_version = bt::to_utf8(argv[++i]);
        } else if (name == L"--repo" && has_value) {
            const std::string repo = bt::to_utf8(argv[++i]);
            const std::size_t slash = repo.find('/');
            options.source.owner = repo.substr(0, slash);
            options.source.repo = slash == std::string::npos ? std::string() : repo.substr(slash + 1);
        } else if (name == L"--asset" && i + 2 < argc) {
            options.source.asset_prefix = bt::to_utf8(argv[++i]);
            options.source.asset_suffix = bt::to_utf8(argv[++i]);
        } else if (name == L"--folder" && has_value) {
            options.folder = argv[++i];
        } else if (name == L"--download") {
            download = true;
            options.download_anywhere = true;
        } else if (name == L"--cancel-after-ms" && has_value) {
            cancel_after_ms = static_cast<UINT>(std::wcstoul(argv[++i], nullptr, 10));
        } else if (name == L"--install-now") {
            install_now = true;
        } else if (name == L"--install-on-exit") {
            install_on_exit = true;
        } else if (name == L"--auto") {
            automatic = true;
        } else {
            std::fprintf(stderr, "Unknown or incomplete option %ls\n", name.c_str());
            return 2;
        }
    }
    bt::log::set_console_echo(true);
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // in step with the log lines, also through a pipe

    // A message-only window: the updater posts to it exactly as it posts to Backtrack's window.
    const HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    auto updater = std::make_unique<bt::Updater>(options);
    updater->notify(window, kMsgUpdate);
    std::printf("Backtrack %s, %s, folder %ls\n", options.current_version.c_str(),
                updater->installed_copy() ? "the installed copy" : "not the installed copy",
                updater->folder().c_str());
    print(updater->state());

    if (install_now || install_on_exit) {
        updater->set_automatic(true);
        const bool started = install_now ? updater->install_now() : updater->install_on_exit();
        std::printf("install_%s: %s\n", install_now ? "now" : "on_exit", started ? "started the installer" : "nothing started");
        print(updater->state());
        return started ? 0 : 1;
    }

    if (automatic) {
        updater->set_automatic(true);
        std::printf("Automatic mode on; waiting for the scheduled check\n");
    } else if (!updater->check()) {
        std::printf("check() refused\n");
        return 1;
    }
    if (cancel_after_ms > 0) SetTimer(window, kCancelTimer, cancel_after_ms, nullptr);
    const auto started = std::chrono::steady_clock::now();
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER && msg.wParam == kCancelTimer) {
            KillTimer(window, kCancelTimer);
            std::printf("Ending the updater now (as Backtrack does when it quits)\n");
            const auto before = std::chrono::steady_clock::now();
            updater.reset();
            std::printf("Ended in %lld ms\n",
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   std::chrono::steady_clock::now() - before)
                                                   .count()));
            break;
        }
        if (msg.message != kMsgUpdate) {
            DispatchMessageW(&msg);
            continue;
        }
        const bt::UpdateState s = updater->state();
        print(s);
        if (s.stage == bt::UpdateStage::Available && download && s.can_install) {
            if (!updater->download()) std::printf("download() refused\n");
            continue;
        }
        if (s.stage == bt::UpdateStage::UpToDate || s.stage == bt::UpdateStage::Failed ||
            s.stage == bt::UpdateStage::Ready || s.stage == bt::UpdateStage::Available) {
            break;
        }
    }
    std::printf("Took %lld ms\n", static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                            std::chrono::steady_clock::now() - started)
                                                            .count()));
    updater.reset();
    if (const bt::UpdaterOptions& o = options; download && !o.folder.empty()) {
        // Show what is on disk, so the digest can be compared by hand (certutil -hashfile).
        WIN32_FIND_DATAW found{};
        const HANDLE find = FindFirstFileW((o.folder / L"*").c_str(), &found);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                const auto hash = bt::sha256_of_file(o.folder / found.cFileName);
                std::printf("  %ls  %s\n", found.cFileName, hash ? hash->c_str() : "(unreadable)");
            } while (FindNextFileW(find, &found));
            FindClose(find);
        }
    }
    DestroyWindow(window);
    return 0;
}
