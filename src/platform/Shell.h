#pragma once

// Handing files to Windows: open a clip in its default player, show it selected in Explorer, send
// it to the Recycle Bin. Opening runs on short-lived threads (the shell wants a single-threaded
// apartment and can take a moment) so the window never freezes; failures are log lines, never
// dialogs over a game.

#include <filesystem>
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

// Waits up to `max_ms` for open()/show_in_folder() threads still running, before the log goes away.
void wait_for_pending(unsigned max_ms);

}  // namespace bt::shell
