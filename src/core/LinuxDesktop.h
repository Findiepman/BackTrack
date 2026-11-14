#pragma once

// The pure parts of the Linux version: key codes for X11 and the GlobalShortcuts portal, the
// portal's request paths and where clips go. Plain strings and numbers, no Linux headers, so the
// unit tests run on every platform.

#include "core/Hotkey.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace bt {

// The X11 keysym (X11/keysymdef.h) for a Win32 virtual key code that parse_hotkey produces.
// Letters map to the lower case keysym, which is what XKeysymToKeycode expects. nullopt for keys
// X11 cannot grab (the mouse buttons) or that parse_hotkey never produces.
std::optional<std::uint32_t> x11_keysym_for_vk(std::uint32_t virtual_key);

// The trigger text the GlobalShortcuts portal takes as preferred_trigger (the XDG shortcuts
// format: "CTRL+ALT+F8", "SHIFT+a", "LOGO+KP_5"). Empty when the key has no keysym.
std::string portal_shortcut_trigger(const Hotkey& hotkey);

// The object path a portal call with handle_token `token` answers on, for the caller's unique
// bus name (":1.42" -> "/org/freedesktop/portal/desktop/request/1_42/<token>").
std::string portal_request_path(std::string_view unique_name, std::string_view token);

// Looks up an environment variable; nullopt when it is not set.
using EnvLookup = std::function<std::optional<std::string>(std::string_view)>;

// XDG_VIDEOS_DIR from the text of ~/.config/user-dirs.dirs (`XDG_VIDEOS_DIR="$HOME/Videos"`),
// with $HOME replaced. nullopt when the key is missing, empty or points at $HOME itself (which
// xdg-user-dirs writes for a disabled folder).
std::optional<std::string> xdg_videos_dir(std::string_view user_dirs_text, std::string_view home);

// The clips folder on Linux for the output_dir setting. The Windows default
// ("%USERPROFILE%\Videos\Backtrack", also what a copied backtrack.ini contains) becomes
// <videos>/Backtrack, where <videos> is XDG_VIDEOS_DIR or ~/Videos, and so does a Windows drive
// path ("D:\Clips"). Otherwise the backslashes of a Windows-style path become slashes, a leading
// "~" and every known "$VAR", "${VAR}" and "%VAR%" are expanded (%USERPROFILE% means $HOME;
// unknown ones stay as typed). A relative result is taken from $HOME. Empty when $HOME is
// unknown and the result would need it.
std::string resolve_output_dir(std::string_view setting, const EnvLookup& env, std::string_view user_dirs_text);

}  // namespace bt
