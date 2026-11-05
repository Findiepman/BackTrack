#pragma once

// Handing files to Windows: open a clip in its default player, show it selected in Explorer, send
// it to the Recycle Bin. Opening runs on short-lived threads (the shell wants a single-threaded
// apartment and can take a moment) so the window never freezes; failures are log lines, never
// dialogs over a game.

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace bt::shell {

void open(const std::filesystem::path& target);
void show_in_folder(const std::filesystem::path& file);

// To the Recycle Bin. A file on a drive without one (USB stick, network share) is never deleted
// for good: it is left where it is and returned with the others that could not be moved (locked
// by a player, ...); the reason is logged. Runs the shell on its own single-threaded-apartment
// thread, as the shell expects, and keeps handling the calling thread's window messages while it
// waits (Windows may ask "delete permanently?" for a file too big for the Recycle Bin, and the
// hotkey must keep working meanwhile). The caller must expect its own messages during the call.
std::vector<std::filesystem::path> recycle(const std::vector<std::filesystem::path>& files);

// Windows' folder picker. Empty when cancelled. Keeps the calling thread's messages flowing.
std::optional<std::filesystem::path> pick_folder(HWND owner, const std::filesystem::path& start, const wchar_t* title);

// Moves files into `folder` with Windows' own progress window (and its question when a file of
// the same name is already there). Returns the files that are still at their old place. Keeps the
// calling thread's messages flowing.
std::vector<std::filesystem::path> move_files(const std::vector<std::filesystem::path>& files,
                                              const std::filesystem::path& folder, HWND owner);

// Keeps "Backtrack" in the Start menu (the user's own Programs folder, no admin rights needed) so
// searching for it after pressing the Windows key finds it: made if missing, pointed at `exe` if
// the exe moved. Carries `app_id`, the same AppUserModelID the process uses, so the taskbar and
// Start treat the running window and the shortcut as one app. Runs on its own thread; logs only.
void ensure_start_menu_shortcut(const std::filesystem::path& exe, const wchar_t* app_id);

// Waits up to `max_ms` for open()/show_in_folder() threads still running, before the log goes away.
void wait_for_pending(unsigned max_ms);

}  // namespace bt::shell
