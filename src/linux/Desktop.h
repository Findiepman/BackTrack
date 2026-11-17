#pragma once

// Linux desktop odds and ends: the XDG folders, the logo, the menu entry of an AppImage.

#include <QIcon>
#include <QImage>
#include <QString>

#include <string>

namespace bt::linux_app {

struct AppPaths {
    std::string config_dir;  // ~/.config/Backtrack: backtrack.ini
    std::string state_dir;  // ~/.local/state/Backtrack: backtrack.log, the screen sharing token
    std::string data_dir;  // ~/.local/share/Backtrack: library.json
    std::string cache_dir;  // ~/.cache/Backtrack: thumbnails
    std::string ini_file;
    std::string log_file;
    std::string token_file;
};

AppPaths app_paths();

// The clips folder for the output_dir setting (core resolve_output_dir with this user's
// environment and ~/.config/user-dirs.dirs).
std::string clips_folder(const std::string& output_dir_setting);

// True in a Wayland session: capture and hotkeys go through the portal, also when the app itself
// runs on XWayland (X11 capture would only see other XWayland windows there).
bool wayland_session();

// Logo #1 ("Ring Rewind", the same drawing as the Windows app), painted at any size.
QImage logo_image(int size);
QIcon logo_icon();

// When running as an AppImage: puts Backtrack in the application menu with the logo, pointing at
// this AppImage file (written again on each start, since an update changes the file's name), with
// a "Save clip" action that can be given a key in KDE's Shortcuts settings.
void install_desktop_entry();

// Opens the file manager on the folder with the file selected (Dolphin, Nautilus and others take
// org.freedesktop.FileManager1); without one, opens the folder.
void show_in_folder(const std::string& path);

std::string read_text_file(const std::string& path);
bool write_text_file(const std::string& path, const std::string& text);

}  // namespace bt::linux_app
