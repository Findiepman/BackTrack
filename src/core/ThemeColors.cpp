#include "core/ThemeColors.h"

#include <algorithm>
#include <cmath>

namespace bt {
namespace {

int channel(double v) {
    return static_cast<int>(std::lround(std::clamp(v, 0.0, 255.0)));
}

double worst(Rgb fg, Rgb bg1, Rgb bg2) {
    return std::min(contrast(fg, bg1), contrast(fg, bg2));
}

}  // namespace

std::wstring_view theme_name(ThemeId id) {
    switch (id) {
        case ThemeId::Harbor: return L"Harbor";
        case ThemeId::Plum: return L"Plum";
        case ThemeId::Moss: return L"Moss";
        case ThemeId::Daylight: return L"Daylight";
        case ThemeId::Night: break;
    }
    return L"Night shift";
}

ThemeInput theme_input(ThemeId id) {
    switch (id) {
        case ThemeId::Harbor: return {Rgb::hex(0x5cc8ff), Rgb::hex(0x0a1420), Rgb::hex(0x111f2f), Rgb::hex(0xe4f0fa)};
        case ThemeId::Plum: return {Rgb::hex(0xff7ab6), Rgb::hex(0x150e1c), Rgb::hex(0x21172b), Rgb::hex(0xf3e8fa)};
        case ThemeId::Moss: return {Rgb::hex(0xa6e35c), Rgb::hex(0x0d140f), Rgb::hex(0x16211a), Rgb::hex(0xe8f2e6)};
        case ThemeId::Daylight: return {Rgb::hex(0x3454f0), Rgb::hex(0xe9edf4), Rgb::hex(0xffffff), Rgb::hex(0x141a26)};
        case ThemeId::Night: break;
    }
    return {Rgb::hex(0xffb547), Rgb::hex(0x0e1014), Rgb::hex(0x171a21), Rgb::hex(0xe9ecf2)};
}

ThemeColors theme_colors(ThemeId id) {
    if (id != ThemeId::Night) return derive_theme(theme_input(id));
    // The mockup's :root, which the whole window was matched against.
    ThemeColors c;
    c.bg = Rgb::hex(0x0e1014);
    c.side = Rgb::hex(0x12151b);
    c.card = Rgb::hex(0x171a21);
    c.ink = Rgb::hex(0xe9ecf2);
    c.mute = Rgb::hex(0xa3aab8);
    c.line = Rgb::hex(0x2a2f3a);
    c.raise = Rgb::hex(0x1f232c);
    c.pop = Rgb::hex(0x1d212a);
    c.off = Rgb::hex(0x4a5160);
    c.accent = Rgb::hex(0xffb547);
    c.accent_text = c.accent;
    c.on_accent = Rgb::hex(0x1a1200);
    c.danger = Rgb::hex(0xff7a6e);
    c.rec = Rgb::hex(0xff5d66);
    c.warn = Rgb::hex(0xf5b84a);
    c.ok = Rgb::hex(0x6fdc9f);
    return c;
}

Rgb mix(Rgb a, Rgb b, double t) {
    return Rgb{channel(a.r + (b.r - a.r) * t), channel(a.g + (b.g - a.g) * t), channel(a.b + (b.b - a.b) * t)};
}

double luminance(Rgb c) {
    const auto linear = [](int v) {
        const double s = v / 255.0;
        return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(c.r) + 0.7152 * linear(c.g) + 0.0722 * linear(c.b);
}

double contrast(Rgb a, Rgb b) {
    const double x = luminance(a);
    const double y = luminance(b);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}

Rgb ensure_contrast(Rgb fg, Rgb bg1, Rgb bg2, double min) {
    if (worst(fg, bg1, bg2) >= min) return fg;
    // The mockup's steps: t = 0.08, 0.16, ... up to just past 1.
    for (int step = 1; step <= 13; ++step) {
        const double t = std::min(0.08 * step, 1.0);
        const Rgb darker = mix(fg, Rgb{0, 0, 0}, t);
        const Rgb lighter = mix(fg, Rgb{255, 255, 255}, t);
        const double wd = worst(darker, bg1, bg2);
        const double wl = worst(lighter, bg1, bg2);
        if (wd >= min || wl >= min) return wd >= wl ? darker : lighter;
    }
    return worst(Rgb{0, 0, 0}, bg1, bg2) >= worst(Rgb{255, 255, 255}, bg1, bg2) ? Rgb{0, 0, 0} : Rgb{255, 255, 255};
}

ThemeColors derive_theme(const ThemeInput& in) {
    ThemeColors c;
    const Rgb text = ensure_contrast(in.text, in.bg, in.surface, 4.5);
    c.bg = in.bg;
    c.side = mix(in.bg, in.surface, 0.5);
    c.card = in.surface;
    c.ink = text;
    c.mute = ensure_contrast(mix(text, in.surface, 0.36), in.bg, in.surface, 4.5);
    c.line = mix(in.surface, text, 0.15);
    c.raise = mix(in.surface, text, 0.07);
    c.pop = mix(in.surface, text, 0.07);
    c.off = mix(c.mute, in.bg, 0.45);
    c.accent = in.accent;
    c.accent_text = ensure_contrast(in.accent, in.bg, in.surface, 4.5);
    c.on_accent = contrast(Rgb{0, 0, 0}, in.accent) >= contrast(Rgb{255, 255, 255}, in.accent) ? Rgb::hex(0x0b0b0b)
                                                                                              : Rgb{255, 255, 255};
    c.light = luminance(in.bg) > 0.35;
    // Status colours get the same correction, so no background hides them.
    const auto status = [&](std::uint32_t dark, std::uint32_t light) {
        return ensure_contrast(Rgb::hex(c.light ? light : dark), in.bg, in.surface, 4.5);
    };
    c.danger = status(0xff7a6e, 0xc0362c);
    c.rec = status(0xff5d66, 0xd92d3a);
    c.warn = status(0xf5b84a, 0x9a5708);
    c.ok = status(0x6fdc9f, 0x1d7a4a);
    return c;
}

}  // namespace bt
