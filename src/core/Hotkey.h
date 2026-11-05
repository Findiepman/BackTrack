#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bt {

// Values are the Win32 MOD_* constants so platform code hands them straight to RegisterHotKey.
// core/ cannot include <windows.h>; main.cpp static_asserts that they match.
namespace hotkey_mod {
inline constexpr std::uint32_t kAlt = 0x0001;
inline constexpr std::uint32_t kCtrl = 0x0002;
inline constexpr std::uint32_t kShift = 0x0004;
inline constexpr std::uint32_t kWin = 0x0008;
}  // namespace hotkey_mod

// Win32 VK codes of the mouse side buttons ("Mouse4" = back, "Mouse5" = forward). Only allowed for a
// held key (push-to-talk): RegisterHotKey cannot take mouse buttons.
inline constexpr std::uint32_t kVkMouse4 = 0x05;  // VK_XBUTTON1
inline constexpr std::uint32_t kVkMouse5 = 0x06;  // VK_XBUTTON2

// How the key is used. A Global hotkey is taken by RegisterHotKey for the whole of Windows, so it
// may not swallow a plain letter. A Held key (push-to-talk) is only watched while it is down and
// every program still gets it, so any key works, the mouse side buttons too.
enum class HotkeyUse { Global, Held };

struct Hotkey {
    std::uint32_t modifiers = 0;  // hotkey_mod bits
    std::uint32_t virtual_key = 0;  // Win32 VK_* code

    bool operator==(const Hotkey&) const = default;
};

struct HotkeyParseResult {
    std::optional<Hotkey> hotkey;
    std::string error;  // empty when hotkey is set
    std::string warning;  // set when the hotkey is valid but often taken by Windows
};

// Parses strings such as "F8", "Ctrl+Alt+F8" or "ctrl + numpad5" (case and spaces ignored).
// Rejected because they could never work:
//  - letters, digits and Space without a modifier: would swallow that key in every program
//  - F12 without a modifier: Windows reserves it for debuggers
//  - Ctrl+Pause: the keyboard sends Break (VK_CANCEL) instead of Pause
//  - Shift+NumpadN: with NumLock on, Shift turns numpad keys into Home/End/arrows
// A Held key also accepts plain letters, digits, Space, F12 and Mouse4/Mouse5.
HotkeyParseResult parse_hotkey(std::string_view text, HotkeyUse use = HotkeyUse::Global);

// Canonical form, e.g. "Ctrl+Alt+F8". Modifiers always in the order Ctrl, Alt, Shift, Win.
std::string hotkey_to_string(const Hotkey& hotkey);

}  // namespace bt
