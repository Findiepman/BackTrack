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
HotkeyParseResult parse_hotkey(std::string_view text);

// Canonical form, e.g. "Ctrl+Alt+F8". Modifiers always in the order Ctrl, Alt, Shift, Win.
std::string hotkey_to_string(const Hotkey& hotkey);

}  // namespace bt
