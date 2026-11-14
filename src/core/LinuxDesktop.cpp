#include "core/LinuxDesktop.h"

#include <array>
#include <cctype>

namespace bt {

namespace {

constexpr std::uint32_t kVkF1 = 0x70;
constexpr std::uint32_t kVkF24 = 0x87;
constexpr std::uint32_t kVkNumpad0 = 0x60;
constexpr std::uint32_t kVkNumpad9 = 0x69;

struct NamedKeysym {
    std::uint32_t virtual_key;
    std::uint32_t keysym;
    std::string_view name;  // the keysym's name, for the portal
};

// X11/keysymdef.h values; core/ cannot include X11 headers.
constexpr std::array kNamedKeysyms{
    NamedKeysym{0x24, 0xFF50, "Home"},     NamedKeysym{0x23, 0xFF57, "End"},
    NamedKeysym{0x2D, 0xFF63, "Insert"},   NamedKeysym{0x2E, 0xFFFF, "Delete"},
    NamedKeysym{0x21, 0xFF55, "Page_Up"},  NamedKeysym{0x22, 0xFF56, "Page_Down"},
    NamedKeysym{0x20, 0x0020, "space"},    NamedKeysym{0x13, 0xFF13, "Pause"},
    NamedKeysym{0x2C, 0xFF61, "Print"},    NamedKeysym{0x14, 0xFFE5, "Caps_Lock"},
    NamedKeysym{0x09, 0xFF09, "Tab"},      NamedKeysym{0xC0, 0x0060, "grave"},
    NamedKeysym{0x26, 0xFF52, "Up"},       NamedKeysym{0x28, 0xFF54, "Down"},
    NamedKeysym{0x25, 0xFF51, "Left"},     NamedKeysym{0x27, 0xFF53, "Right"},
};

constexpr std::uint32_t kXkF1 = 0xFFBE;
constexpr std::uint32_t kXkKp0 = 0xFFB0;

std::optional<std::string> keysym_name(std::uint32_t vk) {
    if (vk >= kVkF1 && vk <= kVkF24) {
        return "F" + std::to_string(vk - kVkF1 + 1);
    }
    if (vk >= kVkNumpad0 && vk <= kVkNumpad9) {
        return "KP_" + std::to_string(vk - kVkNumpad0);
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        return std::string(1, static_cast<char>(std::tolower(static_cast<unsigned char>(vk))));
    }
    for (const NamedKeysym& k : kNamedKeysyms) {
        if (k.virtual_key == vk) {
            return std::string(k.name);
        }
    }
    return std::nullopt;
}

bool is_var_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

std::optional<std::string> lookup(const EnvLookup& env, std::string_view name) {
    if (name == "USERPROFILE") {  // a backtrack.ini copied from Windows
        return env("HOME");
    }
    return env(name);
}

// Expands $VAR, ${VAR} and %VAR% anywhere in the text. Unknown variables stay as they are.
std::string expand_variables(std::string_view text, const EnvLookup& env) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '$' && i + 1 < text.size()) {
            const bool braced = text[i + 1] == '{';
            const std::size_t start = i + (braced ? 2 : 1);
            std::size_t end = start;
            while (end < text.size() && is_var_char(text[end])) {
                ++end;
            }
            const bool closed = !braced || (end < text.size() && text[end] == '}');
            if (end > start && closed) {
                if (const auto value = lookup(env, text.substr(start, end - start))) {
                    out += *value;
                    i = end + (braced ? 1 : 0);
                    continue;
                }
            }
        } else if (c == '%') {
            const std::size_t close = text.find('%', i + 1);
            if (close != std::string_view::npos && close > i + 1) {
                const std::string_view name = text.substr(i + 1, close - i - 1);
                bool valid = true;
                for (const char n : name) {
                    valid = valid && is_var_char(n);
                }
                if (valid) {
                    if (const auto value = lookup(env, name)) {
                        out += *value;
                        i = close + 1;
                        continue;
                    }
                }
            }
        }
        out += c;
        ++i;
    }
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
        s.remove_suffix(1);
    }
    return s;
}

std::string strip_trailing_slashes(std::string path) {
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

}  // namespace

std::optional<std::uint32_t> x11_keysym_for_vk(std::uint32_t vk) {
    if (vk >= kVkF1 && vk <= kVkF24) {
        return kXkF1 + (vk - kVkF1);
    }
    if (vk >= kVkNumpad0 && vk <= kVkNumpad9) {
        return kXkKp0 + (vk - kVkNumpad0);
    }
    if (vk >= 'A' && vk <= 'Z') {
        return vk - 'A' + 'a';
    }
    if (vk >= '0' && vk <= '9') {
        return vk;
    }
    for (const NamedKeysym& k : kNamedKeysyms) {
        if (k.virtual_key == vk) {
            return k.keysym;
        }
    }
    return std::nullopt;
}

std::string portal_shortcut_trigger(const Hotkey& hotkey) {
    const auto key = keysym_name(hotkey.virtual_key);
    if (!key) {
        return {};
    }
    std::string out;
    if ((hotkey.modifiers & hotkey_mod::kCtrl) != 0) out += "CTRL+";
    if ((hotkey.modifiers & hotkey_mod::kAlt) != 0) out += "ALT+";
    if ((hotkey.modifiers & hotkey_mod::kShift) != 0) out += "SHIFT+";
    if ((hotkey.modifiers & hotkey_mod::kWin) != 0) out += "LOGO+";
    return out + *key;
}

std::string portal_request_path(std::string_view unique_name, std::string_view token) {
    std::string sender(unique_name);
    if (!sender.empty() && sender.front() == ':') {
        sender.erase(0, 1);
    }
    for (char& c : sender) {
        if (c == '.') {
            c = '_';
        }
    }
    return "/org/freedesktop/portal/desktop/request/" + sender + "/" + std::string(token);
}

std::optional<std::string> xdg_videos_dir(std::string_view text, std::string_view home) {
    std::optional<std::string> found;
    while (!text.empty()) {
        const std::size_t eol = text.find('\n');
        std::string_view line = trim(text.substr(0, eol));
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        constexpr std::string_view kKey = "XDG_VIDEOS_DIR";
        if (!line.starts_with(kKey)) {
            continue;
        }
        line = trim(line.substr(kKey.size()));
        if (!line.starts_with('=')) {
            continue;
        }
        line = trim(line.substr(1));
        if (line.size() >= 2 && line.front() == '"' && line.back() == '"') {
            line = line.substr(1, line.size() - 2);
        }
        std::string value;
        if (line == "$HOME" || line.starts_with("$HOME/")) {  // not $HOMEX/...
            value = std::string(home) + std::string(line.substr(5));
        } else if (line.starts_with('/')) {
            value = std::string(line);
        } else {
            continue;  // the format only allows $HOME/... or an absolute path
        }
        found = strip_trailing_slashes(value);  // the last line wins, as in xdg-user-dirs
    }
    if (!found || found->empty() || *found == strip_trailing_slashes(std::string(home))) {
        return std::nullopt;
    }
    return found;
}

std::string resolve_output_dir(std::string_view setting, const EnvLookup& env, std::string_view user_dirs_text) {
    const std::string home = env("HOME").value_or("");
    const auto default_dir = [&]() -> std::string {
        if (const auto videos = xdg_videos_dir(user_dirs_text, home)) {
            return *videos + "/Backtrack";
        }
        return home.empty() ? std::string() : home + "/Videos/Backtrack";
    };

    const std::string_view trimmed = trim(setting);
    // The Windows default, also in a backtrack.ini copied over from Windows.
    if (trimmed.empty() || trimmed == "%USERPROFILE%\\Videos\\Backtrack") {
        return default_dir();
    }
    // A Windows drive path ("D:\Clips") means nothing here.
    if (trimmed.size() >= 2 && std::isalpha(static_cast<unsigned char>(trimmed[0])) != 0 && trimmed[1] == ':') {
        return default_dir();
    }

    // A Windows-style path (backslashes, no slash) gets slashes before the variables go in, so a
    // value with a slash cannot stop the conversion.
    std::string typed(trimmed);
    if (typed.find('/') == std::string::npos) {
        for (char& c : typed) {
            if (c == '\\') {
                c = '/';
            }
        }
    }
    std::string path = expand_variables(typed, env);
    if (path == "~" || path.starts_with("~/")) {
        if (home.empty()) {
            return {};
        }
        path = home + path.substr(1);
    }
    if (!path.starts_with('/')) {
        if (home.empty()) {
            return {};
        }
        path = home + "/" + path;
    }
    return strip_trailing_slashes(path);
}

}  // namespace bt
