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

void custom_theme_uses_the_settings_colours() {
    Settings s;
    s.theme = ThemeId::Custom;
    s.custom_accent = 0x00ff88;
    s.custom_background = 0x101820;
    s.custom_surface = 0x202830;
    s.custom_text = 0xf0f0f0;
    const ThemeInput in = theme_input(s);
    CHECK(in.accent == Rgb::hex(0x00ff88) && in.bg == Rgb::hex(0x101820));
    CHECK(in.surface == Rgb::hex(0x202830) && in.text == Rgb::hex(0xf0f0f0));
    const ThemeColors c = theme_colors(s);
    CHECK(c.accent == Rgb::hex(0x00ff88) && c.bg == Rgb::hex(0x101820) && c.card == Rgb::hex(0x202830));
    CHECK(theme_name(ThemeId::Custom) == L"Custom");
    // A preset ignores the custom colours.
    s.theme = ThemeId::Harbor;
    CHECK(theme_colors(s).accent == Rgb::hex(0x5cc8ff));
    // Night shift through the settings is still the hand-picked look.
    s.theme = ThemeId::Night;
    CHECK(theme_colors(s).mute == Rgb::hex(0xa3aab8));
}

void unreadable_custom_colours_are_fixed_and_reported() {
    // Dark grey text on a dark background: the text is pushed lighter and the hint says so.
    const ThemeInput dark{Rgb::hex(0xffb547), Rgb::hex(0x101010), Rgb::hex(0x181818), Rgb::hex(0x303030)};
    const ContrastNote note = contrast_note(dark);
    CHECK(note.text_adjusted && !note.unmet);
    const ThemeColors c = derive_theme(dark);
    CHECK(contrast(c.ink, c.bg) >= 4.5 && contrast(c.ink, c.card) >= 4.5);
    // Readable colours: nothing to say.
    const ContrastNote fine = contrast_note(theme_input(ThemeId::Harbor));
    CHECK(!fine.text_adjusted && !fine.unmet);
    // Black background, near-white surface: no colour reads well on both.
    const ThemeInput split{Rgb::hex(0x808080), Rgb::hex(0x000000), Rgb::hex(0xf0f0f0), Rgb::hex(0x808080)};
    CHECK(contrast_note(split).unmet);
}

void hsv_round_trips() {
    for (const std::uint32_t v : {0x000000u, 0xffffffu, 0xff0000u, 0x00ff00u, 0x0000ffu, 0xffb547u, 0x0e1014u,
                                  0x3454f0u, 0x808080u, 0xff00ffu, 0x123456u}) {
        CHECK(from_hsv(to_hsv(Rgb::hex(v))) == Rgb::hex(v));
    }
    const Hsv red = to_hsv(Rgb::hex(0xff0000));
    CHECK(red.h == 0 && red.s == 1 && red.v == 1);
    CHECK(std::abs(to_hsv(Rgb::hex(0x0000ff)).h - 240) < 1e-9);
    CHECK(to_hsv(Rgb::hex(0x808080)).s == 0);  // grey: no saturation, hue 0
    // Out of range and broken input never makes a broken colour.
    CHECK(from_hsv(Hsv{360, 1, 1}) == Rgb::hex(0xff0000));
    CHECK(from_hsv(Hsv{-120, 1, 1}) == Rgb::hex(0x0000ff));
    CHECK(from_hsv(Hsv{0, 2, -1}) == Rgb::hex(0x000000));
    CHECK(from_hsv(Hsv{std::nan(""), std::nan(""), 1}) == Rgb::hex(0xffffff));
}

}  // namespace

int main() {
    helpers_match_the_mockup();
    night_shift_is_the_original_look();
    every_preset_reads();
    custom_theme_uses_the_settings_colours();
    unreadable_custom_colours_are_fixed_and_reported();
    hsv_round_trips();
    return bt_test::finish("ThemeColorsTests");
}
