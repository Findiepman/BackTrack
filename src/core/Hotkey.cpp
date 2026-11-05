#include "core/Hotkey.h"

#include <charconv>
#include <format>
#include <vector>

namespace bt {
namespace {

// Win32 virtual key codes. Letters and digits are their ASCII upper case values.
constexpr std::uint32_t kVkF1 = 0x70;
constexpr std::uint32_t kVkNumpad0 = 0x60;
constexpr std::uint32_t kVkSpace = 0x20;
constexpr std::uint32_t kVkF12 = 0x7B;
constexpr std::uint32_t kVkPause = 0x13;
constexpr std::uint32_t kVkPrintScreen = 0x2C;
constexpr int kHighestFunctionKey = 24;
constexpr int kNumpadDigits = 10;

struct NamedKey {
    std::string_view name;  // lower case
    std::uint32_t virtual_key;
};

// First entry per key is the canonical spelling used by hotkey_to_string.
constexpr NamedKey kNamedKeys[] = {
    {"home", 0x24},     {"end", 0x23},       {"insert", 0x2D},      {"ins", 0x2D},
    {"delete", 0x2E},   {"del", 0x2E},       {"pageup", 0x21},      {"pgup", 0x21},
    {"pagedown", 0x22}, {"pgdn", 0x22},      {"space", kVkSpace},   {"pause", 0x13},
    {"printscreen", 0x2C}, {"prtsc", 0x2C},
};

constexpr NamedKey kCanonicalNames[] = {
    {"Home", 0x24},     {"End", 0x23},   {"Insert", 0x2D}, {"Delete", 0x2E},
    {"PageUp", 0x21},   {"PageDown", 0x22}, {"Space", kVkSpace}, {"Pause", 0x13},
    {"PrintScreen", 0x2C},
};

// Only for a held key (push-to-talk): watched with GetAsyncKeyState, never registered.
constexpr NamedKey kHeldOnlyNames[] = {
    {"CapsLock", 0x14}, {"Tab", 0x09}, {"Backquote", 0xC0}, {"Up", 0x26},     {"Down", 0x28},
    {"Left", 0x25},     {"Right", 0x27}, {"Mouse3", 0x04},  {"Mouse4", 0x05}, {"Mouse5", 0x06},
};

struct NamedModifier {
    std::string_view name;
    std::uint32_t bit;
};

constexpr NamedModifier kModifiers[] = {
    {"ctrl", hotkey_mod::kCtrl}, {"control", hotkey_mod::kCtrl}, {"alt", hotkey_mod::kAlt},
    {"shift", hotkey_mod::kShift}, {"win", hotkey_mod::kWin},     {"windows", hotkey_mod::kWin},
};

std::string_view trim(std::string_view s) {
    constexpr std::string_view kBlank = " \t\r\n";
    const auto first = s.find_first_not_of(kBlank);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(kBlank);
    return s.substr(first, last - first + 1);
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::optional<int> parse_small_number(std::string_view digits) {
    int value = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (digits.empty() || error != std::errc{} || end != digits.data() + digits.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::uint32_t> key_from_token(std::string_view lower) {
    if (lower.size() == 1) {
        const char c = lower[0];
        if (c >= 'a' && c <= 'z') {
            return static_cast<std::uint32_t>('A' + (c - 'a'));
        }
        if (c >= '0' && c <= '9') {
            return static_cast<std::uint32_t>(c);
        }
    }
    if (lower.size() >= 2 && lower[0] == 'f') {
        if (const auto n = parse_small_number(lower.substr(1)); n && *n >= 1 && *n <= kHighestFunctionKey) {
            return kVkF1 + static_cast<std::uint32_t>(*n - 1);
        }
        return std::nullopt;
    }
    constexpr std::string_view kNumpad = "numpad";
    if (lower.starts_with(kNumpad)) {
        if (const auto n = parse_small_number(lower.substr(kNumpad.size())); n && *n >= 0 && *n < kNumpadDigits) {
            return kVkNumpad0 + static_cast<std::uint32_t>(*n);
        }
        return std::nullopt;
    }
    for (const auto& key : kNamedKeys) {
        if (key.name == lower) {
            return key.virtual_key;
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> modifier_from_token(std::string_view lower) {
    for (const auto& mod : kModifiers) {
        if (mod.name == lower) {
            return mod.bit;
        }
    }
    return std::nullopt;
}

bool needs_modifier(std::uint32_t vk) {
    const bool letter = vk >= 'A' && vk <= 'Z';
    const bool digit = vk >= '0' && vk <= '9';
    return letter || digit || vk == kVkSpace;
}

std::string modifier_name(std::uint32_t bit) {
    switch (bit) {
        case hotkey_mod::kCtrl: return "Ctrl";
        case hotkey_mod::kAlt: return "Alt";
        case hotkey_mod::kShift: return "Shift";
        case hotkey_mod::kWin: return "Win";
        default: return "?";
    }
}

std::string key_name(std::uint32_t vk) {
    for (const auto& key : kHeldOnlyNames) {
        if (key.virtual_key == vk) return std::string(key.name);
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        return std::string(1, static_cast<char>(vk));
    }
    if (vk >= kVkF1 && vk < kVkF1 + kHighestFunctionKey) {
        return std::format("F{}", vk - kVkF1 + 1);
    }
    if (vk >= kVkNumpad0 && vk < kVkNumpad0 + kNumpadDigits) {
        return std::format("Numpad{}", vk - kVkNumpad0);
    }
    for (const auto& key : kCanonicalNames) {
        if (key.virtual_key == vk) {
            return std::string(key.name);
        }
    }
    return std::format("VK{:#04x}", vk);
}

HotkeyParseResult fail(std::string message) {
    return HotkeyParseResult{std::nullopt, std::move(message)};
}

}  // namespace

HotkeyParseResult parse_hotkey(std::string_view text, HotkeyUse use) {
    const bool held = use == HotkeyUse::Held;
    const std::string_view whole = trim(text);
    if (whole.empty()) {
        return fail("the hotkey is empty");
    }

    std::vector<std::string_view> tokens;
    std::size_t start = 0;
    while (true) {
        const std::size_t plus = whole.find('+', start);
        tokens.push_back(trim(whole.substr(start, plus == std::string_view::npos ? std::string_view::npos : plus - start)));
        if (plus == std::string_view::npos) {
            break;
        }
        start = plus + 1;
    }

    Hotkey hotkey;
    std::optional<std::string_view> key_token;
    for (const std::string_view token : tokens) {
        if (token.empty()) {
            return fail(std::format("'{}' has an empty part (a '+' at the start or end, or two in a row)", whole));
        }
        const std::string lower = to_lower(token);
        if (const auto bit = modifier_from_token(lower)) {
            if (hotkey.modifiers & *bit) {
                return fail(std::format("'{}' lists {} twice", whole, modifier_name(*bit)));
            }
            hotkey.modifiers |= *bit;
            continue;
        }
        std::optional<std::uint32_t> vk = key_from_token(lower);
        if (!vk && held) {
            for (const auto& key : kHeldOnlyNames) {
                if (to_lower(key.name) == lower) vk = key.virtual_key;
            }
        }
        if (!vk && held) {
            return fail(std::format("'{}' is not a key Backtrack knows. Use A-Z, 0-9, F1-F24, Numpad0-9, Space, Home, "
                                    "End, Insert, Delete, PageUp, PageDown, CapsLock, Tab, Backquote, Up, Down, Left, "
                                    "Right or Mouse3, Mouse4, Mouse5, optionally with Ctrl, Alt, Shift or Win",
                                    token));
        }
        if (!vk) {
            return fail(std::format("'{}' is not a key Backtrack knows. Use A-Z, 0-9, F1-F24, Numpad0-9, Home, End, "
                                    "Insert, Delete, PageUp, PageDown, Space, Pause or PrintScreen, "
                                    "optionally with Ctrl, Alt, Shift or Win",
                                    token));
        }
        if (key_token) {
            return fail(std::format("'{}' has two keys ('{}' and '{}'), only one key plus modifiers is allowed", whole,
                                    *key_token, token));
        }
        key_token = token;
        hotkey.virtual_key = *vk;
    }

    if (!key_token) {
        return fail(std::format("'{}' has only modifiers, add a key such as F8", whole));
    }
    if (!held && hotkey.modifiers == 0 && needs_modifier(hotkey.virtual_key)) {
        return fail(std::format("'{}' on its own would block that key in every program, add Ctrl, Alt, Shift or Win",
                                *key_token));
    }
    if (!held && hotkey.modifiers == 0 && hotkey.virtual_key == kVkF12) {
        return fail("F12 on its own is reserved by Windows for debuggers, add Ctrl, Alt, Shift or Win");
    }
    if ((hotkey.modifiers & hotkey_mod::kCtrl) && hotkey.virtual_key == kVkPause) {
        return fail("Ctrl+Pause never arrives: the keyboard sends Break instead. Use Pause with another modifier");
    }
    const bool numpad = hotkey.virtual_key >= kVkNumpad0 && hotkey.virtual_key < kVkNumpad0 + kNumpadDigits;
    if ((hotkey.modifiers & hotkey_mod::kShift) && numpad) {
        return fail("Shift+Numpad never arrives: with NumLock on, Shift turns numpad keys into arrow keys");
    }

    if (held && (hotkey.virtual_key == kVkPause || hotkey.virtual_key == kVkPrintScreen)) {
        // Windows sends these as a single press: there is no "still held" to watch.
        return fail(std::format("'{}' cannot be held down, pick another key for push-to-talk", *key_token));
    }

    HotkeyParseResult result{hotkey, {}, {}};
    if (!held && hotkey.modifiers == 0 && hotkey.virtual_key == kVkPrintScreen) {
        result.warning = "PrintScreen on its own opens the Snipping Tool on Windows 11 unless that is turned off "
                         "in Settings > Accessibility > Keyboard";
    } else if (numpad) {
        result.warning = "numpad keys only work while NumLock is on";
    }
    return result;
}

std::string hotkey_to_string(const Hotkey& hotkey) {
    std::string out;
    for (const std::uint32_t bit : {hotkey_mod::kCtrl, hotkey_mod::kAlt, hotkey_mod::kShift, hotkey_mod::kWin}) {
        if (hotkey.modifiers & bit) {
            out += modifier_name(bit);
            out += '+';
        }
    }
    out += key_name(hotkey.virtual_key);
    return out;
}

}  // namespace bt
