#include "platform/Shell.h"

#include "platform/Com.h"
#include "platform/Log.h"
#include "platform/Text.h"

#include <shellapi.h>
#include <propkey.h>
#include <propvarutil.h>
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

namespace {

// Runs `work` on its own single-threaded-apartment thread, as the shell expects, and keeps
// handling the calling thread's window messages while it waits (the window repaints, the hotkey
// keeps working). A WM_QUIT seen meanwhile is put back for the main loop. False when the thread
// could not run.
bool on_sta_thread_waiting(const std::function<void()>& work) {
    try {
        std::thread worker([&work] {
            try {
                const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
                work();
                if (SUCCEEDED(com)) CoUninitialize();
            } catch (const std::exception& e) {
                log::warn("Shell operation failed: {}", e.what());  // §9: nothing escapes a thread
            } catch (...) {
            }
        });
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
        return true;
    } catch (const std::exception& e) {
        log::warn("Could not start a shell operation: {}", e.what());
        return false;
    }
}

}  // namespace

std::vector<std::filesystem::path> recycle(const std::vector<std::filesystem::path>& files) {
    std::vector<std::filesystem::path> failed = files;  // if the thread cannot run, nothing was moved
    on_sta_thread_waiting([&files, &failed] { failed = recycle_here(files); });
    return failed;
}

std::optional<std::filesystem::path> pick_folder(HWND owner, const std::filesystem::path& start, const wchar_t* title) {
    std::optional<std::filesystem::path> chosen;
    on_sta_thread_waiting([&] {
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
        FILEOPENDIALOGOPTIONS options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(title);
        ComPtr<IShellItem> folder;
        if (!start.empty() &&
            SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder.Get());
        }
        if (dialog->Show(owner) != S_OK) return;  // cancelled
        ComPtr<IShellItem> result;
        PWSTR path = nullptr;
        if (SUCCEEDED(dialog->GetResult(&result)) && SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            chosen = std::filesystem::path(path);
            CoTaskMemFree(path);
        }
    });
    return chosen;
}

std::vector<std::filesystem::path> move_files(const std::vector<std::filesystem::path>& files,
                                              const std::filesystem::path& folder, HWND owner) {
    if (files.empty()) return {};
    std::vector<std::filesystem::path> failed = files;
    on_sta_thread_waiting([&] {
        std::wstring from;
        for (const std::filesystem::path& file : files) {
            from += file.wstring();
            from.push_back(L'\0');
        }
        from.push_back(L'\0');  // double-null terminated lists
        std::wstring to = folder.wstring();
        to.push_back(L'\0');
        to.push_back(L'\0');
        SHFILEOPSTRUCTW operation{};
        operation.hwnd = owner;
        operation.wFunc = FO_MOVE;
        operation.pFrom = from.c_str();
        operation.pTo = to.c_str();
        // Windows' own progress window for a big move; it asks before replacing a same-named file.
        operation.fFlags = FOF_NOCONFIRMMKDIR | FOF_ALLOWUNDO;
        const int result = SHFileOperationW(&operation);
        failed.clear();
        for (const std::filesystem::path& file : files) {
            std::error_code ec;
            if (std::filesystem::exists(file, ec)) failed.push_back(file);  // still at the old place
        }
        if (result != 0 || operation.fAnyOperationsAborted) {
            log::warn("Moving clips to {} stopped (error {}); {} of {} were not moved", path_text(folder), result,
                      failed.size(), files.size());
        } else {
            log::info("Moved {} clips to {}", files.size() - failed.size(), path_text(folder));
        }
    });
    return failed;
}

void ensure_start_menu_shortcut(const std::filesystem::path& exe, const wchar_t* app_id) {
    const std::wstring id = app_id;
    {
        // Run straight from a zip (Explorer unpacks it into the temp folder): that copy vanishes
        // soon, so the Start menu keeps pointing at the real one.
        wchar_t temp[MAX_PATH + 1] = {};
        const DWORD length = GetTempPathW(MAX_PATH + 1, temp);
        std::error_code ec;
        const std::filesystem::path temp_dir = std::filesystem::weakly_canonical(std::wstring(temp, length), ec);
        const std::filesystem::path exe_dir = std::filesystem::weakly_canonical(exe.parent_path(), ec);
        if (length > 0 && !temp_dir.empty()) {
            const std::wstring a = exe_dir.wstring();
            const std::wstring b = temp_dir.wstring();
            if (a.size() >= b.size() && _wcsnicmp(a.c_str(), b.c_str(), b.size()) == 0) {
                log::info("Running from the temp folder; the Start menu entry is left as it is");
                return;
            }
        }
    }
    on_shell_thread([exe, id] {
        PWSTR programs = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, KF_FLAG_CREATE, nullptr, &programs))) {
            log::warn("Could not find the Start menu folder; Backtrack is not added to it");
            return;
        }
        const std::filesystem::path link_path = std::filesystem::path(programs) / L"Backtrack.lnk";
        CoTaskMemFree(programs);

        ComPtr<IShellLinkW> link;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return;
        ComPtr<IPersistFile> file;
        ComPtr<IPropertyStore> properties;
        if (FAILED(link.As(&file)) || FAILED(link.As(&properties))) return;
        std::error_code ec;
        if (std::filesystem::exists(link_path, ec) && SUCCEEDED(file->Load(link_path.c_str(), STGM_READ))) {
            // Already there: only rewrite it when it points somewhere else (the exe was moved).
            wchar_t target[MAX_PATH * 4] = {};
            PROPVARIANT current{};
            std::error_code same_ec;
            const bool same_target =
                SUCCEEDED(link->GetPath(target, static_cast<int>(std::size(target)), nullptr, SLGP_RAWPATH)) &&
                std::filesystem::equivalent(std::filesystem::path(target), exe, same_ec);
            bool same_id = false;
            if (SUCCEEDED(properties->GetValue(PKEY_AppUserModel_ID, &current))) {
                same_id = current.vt == VT_LPWSTR && current.pwszVal != nullptr && id == current.pwszVal;
                PropVariantClear(&current);
            }
            if (same_target && same_id) return;
        }
        link->SetPath(exe.c_str());
        link->SetWorkingDirectory(exe.parent_path().c_str());
        link->SetDescription(L"Saves the last moments of your screen and sound with one key");
        link->SetIconLocation(exe.c_str(), 0);
        PROPVARIANT value{};
        if (SUCCEEDED(InitPropVariantFromString(id.c_str(), &value))) {
            properties->SetValue(PKEY_AppUserModel_ID, value);
            properties->Commit();
            PropVariantClear(&value);
        }
        const HRESULT hr = file->Save(link_path.c_str(), TRUE);
        if (FAILED(hr)) {
            log::warn("Could not add Backtrack to the Start menu: {}", hresult_text(hr));
        } else {
            log::info("Backtrack is in the Start menu ({})", path_text(link_path));
        }
    }, exe);
}

void wait_for_pending(unsigned max_ms) {
    constexpr unsigned kStepMs = 50;
    for (unsigned waited = 0; waited < max_ms && g_pending > 0; waited += kStepMs) {
        Sleep(kStepMs);
    }
}

}  // namespace bt::shell
