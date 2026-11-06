#pragma once

// The colour themes of the Settings page (backtrack.html ST_PRESETS / stDerive / stApply). A preset
// is four colours; every other token of the window is derived from them the way the mockup does
// it, including its contrast correction (WCAG 4.5:1 for text against both the background and the
// cards). Night shift is the look the window was drawn with from the start: it keeps the
// hand-picked tokens of the mockup's :root instead of derived ones, so it stays pixel-identical.

#include "core/Config.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace bt {

struct Rgb {
    int r = 0, g = 0, b = 0;  // 0..255
    static constexpr Rgb hex(std::uint32_t v) {
        return Rgb{static_cast<int>((v >> 16) & 0xFF), static_cast<int>((v >> 8) & 0xFF), static_cast<int>(v & 0xFF)};
    }
    constexpr std::uint32_t value() const {
        return static_cast<std::uint32_t>(r) << 16 | static_cast<std::uint32_t>(g) << 8 | static_cast<std::uint32_t>(b);
    }
    friend bool operator==(const Rgb&, const Rgb&) = default;
};

struct ThemeInput {
    Rgb accent, bg, surface, text;
};

// The window's tokens (names follow the mockup's CSS variables).
struct ThemeColors {
    Rgb bg, side, card, ink, mute, line, raise, pop, off;
    Rgb accent;  // fills: buttons, toggles, meters
    Rgb accent_text;  // --acct: the accent used as text or a thin line, corrected for contrast
    Rgb on_accent;  // text on an accent fill
    Rgb danger, rec, warn, ok;
    bool light = false;  // a light background: shadows and overlays get lighter
};

inline constexpr std::array<ThemeId, 5> kThemes = {ThemeId::Night, ThemeId::Harbor, ThemeId::Plum, ThemeId::Moss,
                                                  ThemeId::Daylight};

std::wstring_view theme_name(ThemeId id);  // "Night shift", "Harbor", ...
ThemeInput theme_input(ThemeId id);
ThemeColors theme_colors(ThemeId id);

// The mockup's helpers, exposed for the tests.
Rgb mix(Rgb a, Rgb b, double t);  // a + (b - a) * t, rounded like stHex
double luminance(Rgb c);  // WCAG relative luminance
double contrast(Rgb a, Rgb b);  // WCAG contrast ratio, 1..21
// fg, darkened or lightened in steps until it reaches `min` against both backgrounds.
Rgb ensure_contrast(Rgb fg, Rgb bg1, Rgb bg2, double min);
ThemeColors derive_theme(const ThemeInput& in);

}  // namespace bt
