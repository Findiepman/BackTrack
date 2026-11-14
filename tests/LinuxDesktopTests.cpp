#include "core/LinuxDesktop.h"

#include "Check.h"

#include <map>

using namespace bt;

namespace {

EnvLookup env_of(std::map<std::string, std::string> vars) {
    return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
        const auto it = vars.find(std::string(name));
        if (it == vars.end()) {
            return std::nullopt;
        }
        return it->second;
    };
}

Hotkey parsed(std::string_view text, HotkeyUse use = HotkeyUse::Global) {
    const auto result = parse_hotkey(text, use);
    CHECK(result.hotkey.has_value());
    return result.hotkey.value_or(Hotkey{});
}

void keysyms() {
    CHECK_EQ(x11_keysym_for_vk(parsed("F8").virtual_key).value_or(0), 0xFFC5u);  // XK_F8
    CHECK_EQ(x11_keysym_for_vk(parsed("F1").virtual_key).value_or(0), 0xFFBEu);
    CHECK_EQ(x11_keysym_for_vk(parsed("Ctrl+F24").virtual_key).value_or(0), 0xFFD5u);
    CHECK_EQ(x11_keysym_for_vk(parsed("Alt+A").virtual_key).value_or(0), 0x61u);  // XK_a, not XK_A
    CHECK_EQ(x11_keysym_for_vk(parsed("Alt+7").virtual_key).value_or(0), 0x37u);
    CHECK_EQ(x11_keysym_for_vk(parsed("Numpad5").virtual_key).value_or(0), 0xFFB5u);  // XK_KP_5
    CHECK_EQ(x11_keysym_for_vk(parsed("Home").virtual_key).value_or(0), 0xFF50u);
    CHECK_EQ(x11_keysym_for_vk(parsed("PageDown").virtual_key).value_or(0), 0xFF56u);
    CHECK_EQ(x11_keysym_for_vk(parsed("Delete").virtual_key).value_or(0), 0xFFFFu);
    CHECK_EQ(x11_keysym_for_vk(parsed("Pause").virtual_key).value_or(0), 0xFF13u);
    CHECK_EQ(x11_keysym_for_vk(parsed("PrintScreen").virtual_key).value_or(0), 0xFF61u);
    CHECK_EQ(x11_keysym_for_vk(parsed("V", HotkeyUse::Held).virtual_key).value_or(0), 0x76u);
    CHECK(!x11_keysym_for_vk(parsed("Mouse4", HotkeyUse::Held).virtual_key).has_value());
    CHECK(!x11_keysym_for_vk(0xFFFF).has_value());

    // Every key parse_hotkey accepts for a global hotkey has a keysym.
    for (int f = 1; f <= 24; ++f) {
        CHECK(x11_keysym_for_vk(parsed(std::format("Ctrl+F{}", f)).virtual_key).has_value());
    }
    for (char c = 'A'; c <= 'Z'; ++c) {
        CHECK(x11_keysym_for_vk(parsed(std::format("Ctrl+{}", c)).virtual_key).has_value());
    }
}

void portal_triggers() {
    CHECK_EQ(portal_shortcut_trigger(parsed("F8")), std::string("F8"));
    CHECK_EQ(portal_shortcut_trigger(parsed("Alt+F7")), std::string("ALT+F7"));
    CHECK_EQ(portal_shortcut_trigger(parsed("shift+alt+ctrl+b")), std::string("CTRL+ALT+SHIFT+b"));
    CHECK_EQ(portal_shortcut_trigger(parsed("Win+Numpad0")), std::string("LOGO+KP_0"));
    CHECK_EQ(portal_shortcut_trigger(parsed("Ctrl+PageUp")), std::string("CTRL+Page_Up"));
    CHECK_EQ(portal_shortcut_trigger(parsed("Ctrl+Space")), std::string("CTRL+space"));
    CHECK_EQ(portal_shortcut_trigger(Hotkey{0, 0x05}), std::string());  // a mouse button
}

void request_paths() {
    CHECK_EQ(portal_request_path(":1.42", "bt1"), std::string("/org/freedesktop/portal/desktop/request/1_42/bt1"));
    CHECK_EQ(portal_request_path(":1.1234", "backtrack_7"),
             std::string("/org/freedesktop/portal/desktop/request/1_1234/backtrack_7"));
}

void videos_dir() {
    const std::string_view home = "/home/fin";
    CHECK_EQ(xdg_videos_dir("XDG_VIDEOS_DIR=\"$HOME/Videos\"\n", home).value_or(""), std::string("/home/fin/Videos"));
    CHECK_EQ(xdg_videos_dir("# comment\nXDG_MUSIC_DIR=\"$HOME/Muziek\"\nXDG_VIDEOS_DIR=\"$HOME/Video's\"\n", home)
                 .value_or(""),
             std::string("/home/fin/Video's"));
    CHECK_EQ(xdg_videos_dir("XDG_VIDEOS_DIR=\"/data/films/\"", home).value_or(""), std::string("/data/films"));
    CHECK(!xdg_videos_dir("XDG_VIDEOS_DIR=\"$HOME/\"\n", home).has_value());  // disabled folder
    CHECK(!xdg_videos_dir("", home).has_value());
    CHECK(!xdg_videos_dir("XDG_VIDEOS_DIR=\"relative\"", home).has_value());
    CHECK(!xdg_videos_dir("XDG_VIDEOS_DIRX=\"$HOME/x\"", home).has_value());
    CHECK(!xdg_videos_dir("XDG_VIDEOS_DIR=\"$HOMEX/v\"", home).has_value());  // another variable, not $HOME
}

void output_dirs() {
    const auto env = env_of({{"HOME", "/home/fin"}, {"CLIPS", "/mnt/games"}});
    const std::string_view dirs = "XDG_VIDEOS_DIR=\"$HOME/Video's\"\n";
    // The Windows default (also a backtrack.ini copied from Windows) follows the XDG folder.
    CHECK_EQ(resolve_output_dir("%USERPROFILE%\\Videos\\Backtrack", env, dirs), std::string("/home/fin/Video's/Backtrack"));
    CHECK_EQ(resolve_output_dir("%USERPROFILE%\\Videos\\Backtrack", env, ""), std::string("/home/fin/Videos/Backtrack"));
    CHECK_EQ(resolve_output_dir("", env, ""), std::string("/home/fin/Videos/Backtrack"));
    CHECK_EQ(resolve_output_dir("D:\\Clips", env, ""), std::string("/home/fin/Videos/Backtrack"));
    // Other Windows-style paths under the profile keep their shape.
    CHECK_EQ(resolve_output_dir("%USERPROFILE%\\Clips\\Games", env, ""), std::string("/home/fin/Clips/Games"));
    CHECK_EQ(resolve_output_dir("~/Clips", env, ""), std::string("/home/fin/Clips"));
    CHECK_EQ(resolve_output_dir("~", env, ""), std::string("/home/fin"));
    CHECK_EQ(resolve_output_dir("$CLIPS/backtrack/", env, ""), std::string("/mnt/games/backtrack"));
    CHECK_EQ(resolve_output_dir("${CLIPS}/b", env, ""), std::string("/mnt/games/b"));
    CHECK_EQ(resolve_output_dir("/srv/clips", env, ""), std::string("/srv/clips"));
    CHECK_EQ(resolve_output_dir("clips", env, ""), std::string("/home/fin/clips"));
    // Unknown variables stay as typed instead of becoming an empty path.
    CHECK_EQ(resolve_output_dir("/x/$NOPE/y", env, ""), std::string("/x/$NOPE/y"));
    CHECK_EQ(resolve_output_dir("/x/%NOPE%/y", env, ""), std::string("/x/%NOPE%/y"));
    CHECK_EQ(resolve_output_dir("/x/${CLIPS", env, ""), std::string("/x/${CLIPS"));
    // No $HOME: nothing that needs it.
    const auto no_home = env_of({});
    CHECK_EQ(resolve_output_dir("~/Clips", no_home, ""), std::string());
    CHECK_EQ(resolve_output_dir("%USERPROFILE%\\Videos\\Backtrack", no_home, ""), std::string());
    CHECK_EQ(resolve_output_dir("/srv/clips", no_home, ""), std::string("/srv/clips"));
}

}  // namespace

int main() {
    keysyms();
    portal_triggers();
    request_paths();
    videos_dir();
    output_dirs();
    return bt_test::finish("LinuxDesktopTests");
}
