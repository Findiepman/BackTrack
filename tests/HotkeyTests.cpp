#include "core/Hotkey.h"

#include "Check.h"

#include <cstdint>
#include <vector>

using namespace bt;

namespace {

Hotkey parse_ok(std::string_view text) {
    const HotkeyParseResult r = parse_hotkey(text);
    if (!r.hotkey) {
        bt_test::report_failure(__FILE__, __LINE__, std::format("'{}' should parse but failed: {}", text, r.error));
        return {};
    }
    return *r.hotkey;
}

std::string parse_error(std::string_view text) {
    const HotkeyParseResult r = parse_hotkey(text);
    if (r.hotkey) {
        bt_test::report_failure(__FILE__, __LINE__, std::format("'{}' should be rejected", text));
        return {};
    }
    return r.error;
}

void every_letter_and_digit() {
    for (char c = 'A'; c <= 'Z'; ++c) {
        const Hotkey hk = parse_ok(std::format("Ctrl+{}", c));
        CHECK_EQ(hk.virtual_key, static_cast<std::uint32_t>(c));
        CHECK_EQ(hk.modifiers, hotkey_mod::kCtrl);
        const char lower = static_cast<char>(c - 'A' + 'a');
        CHECK_EQ(parse_ok(std::format("alt+{}", lower)).virtual_key, static_cast<std::uint32_t>(c));
    }
    for (char c = '0'; c <= '9'; ++c) {
        CHECK_EQ(parse_ok(std::format("Shift+{}", c)).virtual_key, static_cast<std::uint32_t>(c));
    }
}

void every_function_key() {
    for (int n = 1; n <= 24; ++n) {
        if (n == 12) continue;  // bare F12 is reserved, tested in reserved_combinations()
        const Hotkey hk = parse_ok(std::format("F{}", n));
        CHECK_EQ(hk.virtual_key, static_cast<std::uint32_t>(0x70 + n - 1));
        CHECK_EQ(hk.modifiers, 0u);
        CHECK_EQ(hotkey_to_string(hk), std::format("F{}", n));
    }
    CHECK_EQ(parse_ok("f8").virtual_key, 0x77u);
}

void every_numpad_key() {
    for (int n = 0; n <= 9; ++n) {
        const Hotkey hk = parse_ok(std::format("Numpad{}", n));
        CHECK_EQ(hk.virtual_key, static_cast<std::uint32_t>(0x60 + n));
        CHECK_EQ(hotkey_to_string(hk), std::format("Numpad{}", n));
    }
    CHECK_EQ(parse_ok("NUMPAD3").virtual_key, 0x63u);
}

void every_named_key() {
    struct Case {
        const char* text;
        std::uint32_t vk;
        const char* canonical;
    };
    const Case cases[] = {
        {"Home", 0x24, "Home"},        {"End", 0x23, "End"},          {"Insert", 0x2D, "Insert"},
        {"Ins", 0x2D, "Insert"},       {"Delete", 0x2E, "Delete"},    {"Del", 0x2E, "Delete"},
        {"PageUp", 0x21, "PageUp"},    {"PgUp", 0x21, "PageUp"},      {"PageDown", 0x22, "PageDown"},
        {"PgDn", 0x22, "PageDown"},    {"Pause", 0x13, "Pause"},      {"PrintScreen", 0x2C, "PrintScreen"},
        {"PrtSc", 0x2C, "PrintScreen"},
    };
    for (const Case& c : cases) {
        const Hotkey hk = parse_ok(c.text);
        CHECK_EQ(hk.virtual_key, c.vk);
        CHECK_EQ(hotkey_to_string(hk), std::string(c.canonical));
    }
    CHECK_EQ(parse_ok("Ctrl+Space").virtual_key, 0x20u);
}

void modifiers() {
    const Hotkey all = parse_ok("Win+Shift+Alt+Ctrl+F12");
    CHECK_EQ(all.modifiers, hotkey_mod::kCtrl | hotkey_mod::kAlt | hotkey_mod::kShift | hotkey_mod::kWin);
    CHECK_EQ(hotkey_to_string(all), std::string("Ctrl+Alt+Shift+Win+F12"));

    CHECK(parse_ok("Control+F1").modifiers == hotkey_mod::kCtrl);
    CHECK(parse_ok("Windows+F1").modifiers == hotkey_mod::kWin);
    CHECK(parse_ok("  ctrl +  ALT + f8 ") == (Hotkey{hotkey_mod::kCtrl | hotkey_mod::kAlt, 0x77}));
    CHECK(parse_ok("F8+Ctrl") == (Hotkey{hotkey_mod::kCtrl, 0x77}));
    CHECK_EQ(hotkey_to_string(parse_ok("Ctrl+Shift+F12")), std::string("Ctrl+Shift+F12"));
}

void bare_typing_keys_need_a_modifier() {
    CHECK(bt_test::contains(parse_error("A"), "would block"));
    CHECK(bt_test::contains(parse_error("5"), "would block"));
    CHECK(bt_test::contains(parse_error("Space"), "would block"));
    parse_ok("Numpad5");
    parse_ok("Pause");
    parse_ok("Home");
}

void bad_input() {
    CHECK(bt_test::contains(parse_error(""), "empty"));
    CHECK(bt_test::contains(parse_error("   "), "empty"));
    CHECK(bt_test::contains(parse_error("Ctrl+"), "empty part"));
    CHECK(bt_test::contains(parse_error("+F8"), "empty part"));
    CHECK(bt_test::contains(parse_error("Ctrl++F8"), "empty part"));
    CHECK(bt_test::contains(parse_error("Ctrl+Ctrl+F8"), "twice"));
    CHECK(bt_test::contains(parse_error("Ctrl+Control+F8"), "twice"));
    CHECK(bt_test::contains(parse_error("F8+F9"), "two keys"));
    CHECK(bt_test::contains(parse_error("Ctrl+Alt"), "only modifiers"));
    CHECK(bt_test::contains(parse_error("F25"), "not a key"));
    CHECK(bt_test::contains(parse_error("F0"), "not a key"));
    CHECK(bt_test::contains(parse_error("F"), "would block"));  // the letter F
    CHECK(bt_test::contains(parse_error("Numpad10"), "not a key"));
    CHECK(bt_test::contains(parse_error("Numpad"), "not a key"));
    CHECK(bt_test::contains(parse_error("Hyper+F8"), "not a key"));
    CHECK(bt_test::contains(parse_error("Ctrl+Escape"), "not a key"));
    CHECK(bt_test::contains(parse_error("Ctrl+F8x"), "not a key"));
}

void round_trip_everything() {
    std::vector<std::string> keys;
    for (char c = 'A'; c <= 'Z'; ++c) keys.push_back(std::string(1, c));
    for (char c = '0'; c <= '9'; ++c) keys.push_back(std::string(1, c));
    for (int n = 1; n <= 24; ++n) keys.push_back(std::format("F{}", n));
    for (int n = 0; n <= 9; ++n) keys.push_back(std::format("Numpad{}", n));
    for (const char* k : {"Home", "End", "Insert", "Delete", "PageUp", "PageDown", "Space", "Pause", "PrintScreen"}) {
        keys.push_back(k);
    }
    for (const std::string& key : keys) {
        const std::string text = "Alt+Win+" + key;
        CHECK_EQ(hotkey_to_string(parse_ok(text)), text);
    }
}

void reserved_combinations() {
    CHECK(bt_test::contains(parse_error("F12"), "reserved"));
    parse_ok("Ctrl+Shift+F12");
    parse_ok("Alt+F12");
    CHECK(bt_test::contains(parse_error("Ctrl+Pause"), "Break"));
    CHECK(bt_test::contains(parse_error("Ctrl+Alt+Pause"), "Break"));
    parse_ok("Alt+Pause");
    CHECK(bt_test::contains(parse_error("Shift+Numpad5"), "NumLock"));
    CHECK(bt_test::contains(parse_error("Ctrl+Shift+Numpad0"), "NumLock"));

    CHECK(bt_test::contains(parse_hotkey("PrintScreen").warning, "Snipping"));
    CHECK(parse_hotkey("Ctrl+PrintScreen").warning.empty());
    CHECK(bt_test::contains(parse_hotkey("Numpad5").warning, "NumLock"));
    CHECK(parse_hotkey("F8").warning.empty());
}

}  // namespace

void held_keys() {
    // Push-to-talk: watched, not taken, so any key works, the mouse side buttons too.
    const auto v = parse_hotkey("v", HotkeyUse::Held);
    CHECK(v.hotkey && v.hotkey->virtual_key == 'V' && v.hotkey->modifiers == 0);
    CHECK(parse_hotkey("Space", HotkeyUse::Held).hotkey.has_value());
    CHECK(parse_hotkey("F12", HotkeyUse::Held).hotkey.has_value());
    const auto mouse = parse_hotkey("mouse5", HotkeyUse::Held);
    CHECK(mouse.hotkey && mouse.hotkey->virtual_key == kVkMouse5);
    CHECK(hotkey_to_string(*mouse.hotkey) == "Mouse5");
    CHECK(parse_hotkey("Ctrl+Mouse4", HotkeyUse::Held).hotkey.has_value());
    // Still refused as a global hotkey.
    CHECK(!parse_hotkey("v").hotkey.has_value());
    CHECK(!parse_hotkey("Mouse4").hotkey.has_value());
    // What can never arrive stays refused either way.
    CHECK(!parse_hotkey("Shift+Numpad5", HotkeyUse::Held).hotkey.has_value());
    CHECK(!parse_hotkey("Ctrl+Pause", HotkeyUse::Held).hotkey.has_value());
    CHECK(!parse_hotkey("Pause", HotkeyUse::Held).hotkey.has_value());  // only ever a single press
    for (const char* name : {"CapsLock", "Tab", "Backquote", "Up", "Down", "Left", "Right", "Mouse3"}) {
        const auto key = parse_hotkey(name, HotkeyUse::Held);
        CHECK(key.hotkey.has_value());
        if (key.hotkey) CHECK(hotkey_to_string(*key.hotkey) == name);
        CHECK(!parse_hotkey(name).hotkey.has_value());  // never a global hotkey
    }
    CHECK(bt_test::contains(parse_hotkey("Escape", HotkeyUse::Held).error, "Mouse4"));
    CHECK(!parse_hotkey("PrintScreen", HotkeyUse::Held).hotkey.has_value());
}

int main() {
    held_keys();
    every_letter_and_digit();
    every_function_key();
    every_numpad_key();
    every_named_key();
    modifiers();
    bare_typing_keys_need_a_modifier();
    bad_input();
    round_trip_everything();
    reserved_combinations();
    return bt_test::finish("HotkeyTests");
}
