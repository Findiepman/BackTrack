#include "platform/Shell.h"

#include "platform/Com.h"
#include "platform/Log.h"
#include "platform/Text.h"

#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <functional>
#include <optional>
#include <thread>

namespace bt::shell {
namespace {

std::atomic<int> g_pending{0};

std::string path_text(const std::filesystem::path& path) {
    return to_utf8(path.wstring());  // path::string() throws for names outside the ANSI code page
}

void on_shell_thread(std::function<void()> work, const std::filesystem::path& target) {
    ++g_pending;
    try {
        std::thread([work = std::move(work)] {
            struct Done {
                ~Done() { --g_pending; }
            } done;
            try {
                const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
                work();
                if (SUCCEEDED(com)) {
                    CoUninitialize();
                }
            } catch (...) {
                // §9: nothing escapes a thread; these only ever log.
            }
        }).detach();
    } catch (const std::exception& e) {
        --g_pending;  // the thread never started
        log::warn("Could not open {}: {}", path_text(target), e.what());
    }
}

}  // namespace

void open(const std::filesystem::path& target) {
    on_shell_thread([target] {
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
        info.lpVerb = L"open";
        info.lpFile = target.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info)) {
            log::warn("Could not open {}: {}", path_text(target), hresult_text(HRESULT_FROM_WIN32(GetLastError())));
        }
    }, target);
}

void show_in_folder(const std::filesystem::path& file) {
    on_shell_thread([file] {
        PIDLIST_ABSOLUTE item = ILCreateFromPathW(file.c_str());
        if (item == nullptr) {
            log::warn("Could not show {} in Explorer: it no longer exists", path_text(file));
            return;
        }
        const HRESULT hr = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
        ILFree(item);
        if (FAILED(hr)) {
            log::warn("Could not show {} in Explorer: {}", path_text(file), hresult_text(hr));
        }
    }, file);
}

namespace {

// Only fixed local drives have a Recycle Bin for sure; on the others "recycle" is a permanent
// delete, which Backtrack never does on its own.
bool has_recycle_bin(const std::filesystem::path& file) {
    const std::wstring root = file.root_path().wstring();
    if (root.empty() || root.starts_with(L"\\\\")) {
        return false;  // relative or a network (UNC) path
    }
    return GetDriveTypeW(root.c_str()) == DRIVE_FIXED;
}

std::vector<std::filesystem::path> recycle_here(const std::vector<std::filesystem::path>& files) {
    std::vector<std::filesystem::path> failed;
    for (const std::filesystem::path& file : files) {
        if (!has_recycle_bin(file)) {
            log::warn("Not removing {}: its drive has no Recycle Bin, so it would be deleted for good", path_text(file));
            failed.push_back(file);
            continue;
        }
        // One at a time: a single locked file (playing in a player) must not stop the others.
        std::wstring from = file.wstring();
        from.push_back(L'\0');  // the list is double-null terminated
        SHFILEOPSTRUCTW operation{};
        operation.wFunc = FO_DELETE;
        operation.pFrom = from.c_str();
        // FOF_WANTNUKEWARNING: a file too big for the Recycle Bin asks first instead of vanishing.
        operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_SILENT | FOF_NOERRORUI;
        const int result = SHFileOperationW(&operation);
        std::error_code ec;
        if (result != 0 || operation.fAnyOperationsAborted || std::filesystem::exists(file, ec)) {
            log::warn("Could not move {} to the Recycle Bin (error {}); it may be open in another program",
                      path_text(file), result);
            failed.push_back(file);
        } else {
            log::info("Moved {} to the Recycle Bin", path_text(file));
        }
    }
    return failed;
}

}  // namespace

std::vector<std::filesystem::path> recycle(const std::vector<std::filesystem::path>& files) {
    std::vector<std::filesystem::path> failed = files;  // if the thread cannot run, nothing was moved
    try {
        std::thread worker([&files, &failed] {
            try {
                const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
                failed = recycle_here(files);
                if (SUCCEEDED(com)) CoUninitialize();
            } catch (...) {
                // §9: nothing escapes a thread; `failed` keeps every file.
            }
        });
        // Wait, but keep the window and the hotkey alive. A WM_QUIT seen meanwhile is put back
        // for the main loop.
        const HANDLE thread = worker.native_handle();
        std::optional<int> quit_code;
        while (MsgWaitForMultipleObjects(1, &thread, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
            MSG msg{};
            while (!quit_code && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    quit_code = static_cast<int>(msg.wParam);
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (quit_code) {
                WaitForSingleObject(thread, INFINITE);
                break;
            }
        }
        worker.join();
        if (quit_code) PostQuitMessage(*quit_code);
    } catch (const std::exception& e) {
        log::warn("Could not move clips to the Recycle Bin: {}", e.what());
    }
    return failed;
}

void wait_for_pending(unsigned max_ms) {
    constexpr unsigned kStepMs = 50;
    for (unsigned waited = 0; waited < max_ms && g_pending > 0; waited += kStepMs) {
        Sleep(kStepMs);
    }
}

}  // namespace bt::shell
