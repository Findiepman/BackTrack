#include "core/ThemeColors.h"

#include "Check.h"

#include <cmath>

using namespace bt;

namespace {

void helpers_match_the_mockup() {
    // stMix rounds each channel half up like Math.round: 18.5 -> 19, 21 -> 21, 26.5 -> 27.
    CHECK(mix(Rgb::hex(0x0e1014), Rgb::hex(0x171a21), 0.5) == Rgb::hex(0x13151b));
    CHECK(mix(Rgb::hex(0x000000), Rgb::hex(0xffffff), 0.0) == Rgb::hex(0x000000));
    CHECK(mix(Rgb::hex(0x000000), Rgb::hex(0xffffff), 1.0) == Rgb::hex(0xffffff));
    CHECK(std::abs(contrast(Rgb::hex(0x000000), Rgb::hex(0xffffff)) - 21.0) < 1e-9);
    CHECK(std::abs(contrast(Rgb::hex(0x777777), Rgb::hex(0x777777)) - 1.0) < 1e-9);
    CHECK(contrast(Rgb::hex(0x123456), Rgb::hex(0xabcdef)) == contrast(Rgb::hex(0xabcdef), Rgb::hex(0x123456)));
    // Already readable: unchanged.
    CHECK(ensure_contrast(Rgb::hex(0xffffff), Rgb::hex(0x000000), Rgb::hex(0x111111), 4.5) == Rgb::hex(0xffffff));
    // Grey on grey gets pushed until it reads.
    const Rgb fixed = ensure_contrast(Rgb::hex(0x808080), Rgb::hex(0x707070), Rgb::hex(0x757575), 4.5);
    CHECK(contrast(fixed, Rgb::hex(0x707070)) >= 4.5 || fixed == Rgb::hex(0x000000) || fixed == Rgb::hex(0xffffff));
}

void night_shift_is_the_original_look() {
    const ThemeColors c = theme_colors(ThemeId::Night);
    CHECK(c.bg == Rgb::hex(0x0e1014) && c.card == Rgb::hex(0x171a21) && c.ink == Rgb::hex(0xe9ecf2));
    CHECK(c.mute == Rgb::hex(0xa3aab8) && c.line == Rgb::hex(0x2a2f3a) && c.accent == Rgb::hex(0xffb547));
    CHECK(c.on_accent == Rgb::hex(0x1a1200) && !c.light);
    CHECK(theme_name(ThemeId::Night) == L"Night shift");
}

void every_preset_reads() {
    for (const ThemeId id : kThemes) {
        const ThemeColors c = theme_colors(id);
        // Text and secondary text pass 4.5:1 against both surfaces, as the mockup promises.
        CHECK(contrast(c.ink, c.bg) >= 4.5 && contrast(c.ink, c.card) >= 4.5);
        CHECK(contrast(c.mute, c.bg) >= 4.5 && contrast(c.mute, c.card) >= 4.5);
        CHECK(contrast(c.accent_text, c.bg) >= 4.5 && contrast(c.accent_text, c.card) >= 4.5);
        CHECK(contrast(c.danger, c.bg) >= 4.5 && contrast(c.ok, c.card) >= 4.5);
        // Text on an accent button is the better of black and white.
        CHECK(contrast(c.on_accent, c.accent) >= 3.0);
        // Lines sit between card and text.
        CHECK(luminance(c.line) != luminance(c.card));
        CHECK(!theme_name(id).empty());
    }
    CHECK(theme_colors(ThemeId::Daylight).light);
    CHECK(!theme_colors(ThemeId::Harbor).light && !theme_colors(ThemeId::Plum).light && !theme_colors(ThemeId::Moss).light);
    // Daylight: a blue accent on white needs white text on its buttons; Moss's lime needs dark text.
    CHECK(theme_colors(ThemeId::Daylight).on_accent == Rgb::hex(0xffffff));
    CHECK(theme_colors(ThemeId::Moss).on_accent == Rgb::hex(0x0b0b0b));
    CHECK(theme_colors(ThemeId::Harbor).accent == Rgb::hex(0x5cc8ff));
    // Daylight's status colours are the light set.
    CHECK(theme_colors(ThemeId::Daylight).danger == Rgb::hex(0xc0362c));
}

}  // namespace

int main() {
    helpers_match_the_mockup();
    night_shift_is_the_original_look();
    every_preset_reads();
    return bt_test::finish("ThemeColorsTests");
}
