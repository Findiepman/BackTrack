// Controls drawn in the mockup's style (backtrack.html: .btn, .seg, .sw, select, range inputs,
// .menu). Each draws itself and records its hit; App::run and App::on_drag act on them.

#include "platform/Text.h"
#include "ui/App.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cmath>

namespace bt::ui {

using namespace theme;

void App::add_hit(const Rect& rect, Action action, std::string arg) {
    hits_.push_back(Hit{rect, action, std::move(arg)});
}

bool App::hovered(Action action, std::string_view arg) const {
    return hover_ && hover_->action == action && hover_->arg == arg;
}

float App::key_chip(float x, float y, std::wstring_view key) {
    // kbd: 600 12px, min-width 26, height 24 (20 in small places), border with a 2px bottom.
    Graphics& g = *graphics_;
    const TextStyle style{11, Weight::Semibold};
    const float w = std::max(22.0f, g.text_width(key, style) + 12);
    const Rect r{x, y, w, 20};
    g.fill_round(r, 5, kLine);
    g.fill_round(Rect{r.x, r.y, r.w, r.h - 1.5f}, 5, kCard);
    g.text(key, Rect{r.x, r.y, r.w, r.h - 1.5f}, style, kInk, Align::Center);
    return x + w;
}

void App::nav_item(const Rect& rect, wchar_t glyph, std::wstring_view label, std::size_t count, bool selected,
                   Action action, std::string arg, std::optional<Color> dot, bool soon) {
    Graphics& g = *graphics_;
    const bool hot = hovered(action, arg);
    if (selected) {
        g.fill_round(rect, kRadius, kAccent.with_alpha(0.15f));
    } else if (hot) {
        g.fill_round(rect, kRadius, kHover);
    }
    const Rect icon_rect{rect.x + 10, rect.y, 16, rect.h};
    if (dot) {
        g.fill_circle(icon_rect.x + 8, rect.y + rect.h / 2, 4.5f, *dot);
    } else {
        g.icon(glyph, icon_rect, 14, selected ? kAccent : kMute);
    }
    const Color ink = soon ? kMute : kInk;
    const float right_w = soon ? 46.0f : 32.0f;
    g.text(label, Rect{rect.x + 35, rect.y, rect.w - 35 - right_w, rect.h},
           TextStyle{13, selected ? Weight::Semibold : Weight::Regular}, ink);
    if (soon) {
        const Rect badge{rect.right() - 44, rect.y + 8, 36, 16};
        g.fill_round(badge, 8, kInk.with_alpha(0.07f));
        g.text(L"Soon", badge, TextStyle{10, Weight::Semibold}, kMute, Align::Center);
    } else if (count > 0) {
        g.text(std::to_wstring(count), Rect{rect.right() - 40, rect.y, 30, rect.h}, TextStyle{12, Weight::Regular, false, true},
               kMute, Align::End);
    }
    add_hit(rect, action, std::move(arg));
}

void App::button(const Rect& rect, std::wstring_view label, wchar_t glyph, Action action, std::string arg, bool primary,
                 bool danger) {
    Graphics& g = *graphics_;
    const bool hot = hovered(action, arg);
    if (primary) {
        g.fill_round(rect, kRadius, hot ? kAccent.mix(Color::hex(0xffffff), 0.9f) : kAccent);
    } else if (hot) {
        g.fill_round(rect, kRadius, danger ? kDanger.with_alpha(0.12f) : kHover);
    }
    const Color fg = primary ? kOnAccent : danger ? kDanger : kInk;
    const TextStyle style{13, Weight::Semibold};
    const float text_w = label.empty() ? 0 : g.text_width(label, style);
    const float content = (glyph != 0 ? 16 : 0) + (glyph != 0 && !label.empty() ? 6 : 0) + text_w;
    float cx = rect.x + (rect.w - content) / 2;
    if (glyph != 0) {
        g.icon(glyph, Rect{cx, rect.y, 16, rect.h}, 13, fg);
        cx += 16 + (label.empty() ? 0 : 6);
    }
    if (!label.empty()) g.text(label, Rect{cx, rect.y, text_w + 2, rect.h}, style, fg);
    add_hit(rect, action, std::move(arg));
}

float App::button_width(std::wstring_view label, wchar_t glyph, bool compact) const {
    const TextStyle style{compact ? 12.5f : 13.5f, Weight::Semibold};
    const float text_w = label.empty() ? 0 : graphics_->text_width(label, style);
    const float icon_w = glyph != 0 ? (compact ? 14.0f : 16.0f) : 0.0f;
    const float gap = glyph != 0 && !label.empty() ? 8.0f : 0.0f;
    const float pad = compact ? 11.0f : 14.0f;
    if (label.empty()) return compact ? 30.0f : kPageControl;  // icon only: square
    return std::ceil(pad * 2 + icon_w + gap + text_w);
}

float App::push_button(float x, float y, std::wstring_view label, wchar_t glyph, ButtonStyle style, bool compact,
                       Action action, std::string arg, bool disabled, std::optional<Color> ink) {
    Graphics& g = *graphics_;
    const float w = button_width(label, glyph, compact);
    const float h = compact ? 30.0f : kPageControl;
    const float radius = compact ? 8.0f : kPageRadius;
    const Rect r{x, y, w, h};
    const bool hot = !disabled && hovered(action, arg);
    const bool down = hot && pressed_ && pressed_->action == action && pressed_->arg == arg;
    const float alpha = disabled ? 0.42f : 1.0f;
    const Rect body = down ? Rect{r.x, r.y + 1, r.w, r.h} : r;
    Color fg = kInk;
    switch (style) {
        case ButtonStyle::Primary:
            g.fill_round(body, radius, (hot ? kAccent.mix(Color::hex(0xffffff), 0.9f) : kAccent).with_alpha(alpha));
            fg = kOnAccent;
            break;
        case ButtonStyle::DangerFill:
            g.fill_round(body, radius, (hot ? kDanger.mix(Color::hex(0xffffff), 0.9f) : kDanger).with_alpha(alpha));
            fg = Color::hex(0xffffff);
            break;
        case ButtonStyle::Line:
            g.fill_round(body, radius, kInk.with_alpha(0.07f * alpha));
            g.stroke_round(body, radius, hot ? kInk.mix(kLine, 0.38f) : kLine.with_alpha(alpha));
            break;
        case ButtonStyle::Ghost:
            if (hot) g.fill_round(body, radius, kHover);
            break;
        case ButtonStyle::Danger:
            if (hot) g.fill_round(body, radius, kDanger.with_alpha(0.12f));
            fg = kDanger;
            break;
    }
    if (ink) fg = *ink;
    fg = fg.with_alpha(fg.a * alpha);
    const TextStyle text_style{compact ? 12.5f : 13.5f, Weight::Semibold};
    const float text_w = label.empty() ? 0 : g.text_width(label, text_style);
    const float icon_w = glyph != 0 ? (compact ? 14.0f : 16.0f) : 0.0f;
    const float gap = glyph != 0 && !label.empty() ? 8.0f : 0.0f;
    float cx = body.x + (body.w - (icon_w + gap + text_w)) / 2;
    if (glyph != 0) {
        g.icon(glyph, Rect{cx, body.y, icon_w, body.h}, compact ? 12.0f : 14.0f, fg);
        cx += icon_w + gap;
    }
    if (!label.empty()) g.text(label, Rect{cx, body.y, text_w + 2, body.h}, text_style, fg);
    add_hit(r, disabled ? Action::Disabled : action, std::move(arg));
    return w;
}

void App::icon_button(const Rect& rect, wchar_t glyph, float size, Action action, std::string arg, bool disabled,
                      Color ink, Color hover) {
    Graphics& g = *graphics_;
    const bool hot = !disabled && hovered(action, arg);
    if (hot) g.fill_round(rect, 9, hover.a > 0 ? hover : kHover);
    const Color fg = ink.a > 0 ? ink : (hot ? kInk : kMute);
    g.icon(glyph, rect, size, disabled ? fg.with_alpha(0.35f) : fg);
    add_hit(rect, disabled ? Action::Disabled : action, std::move(arg));
}

void App::toggle(float right, float cy, bool on, Action action, std::string arg, bool disabled) {
    // .sw: 42x24, knob 18 with 3 px margin.
    Graphics& g = *graphics_;
    const Rect r{right - 42, cy - 12, 42, 24};
    const float alpha = disabled ? 0.42f : 1.0f;
    Color track = on ? kAccent : kOff;
    if (!disabled && hovered(action, arg)) track = track.mix(Color::hex(0xffffff), 0.92f);
    g.fill_round(r, 12, track.with_alpha(alpha));
    const float knob_x = on ? r.right() - 3 - 9 : r.x + 3 + 9;
    g.fill_circle(knob_x, cy + 0.5f, 9.5f, Color::hex(0x000000, 0.18f * alpha));
    g.fill_circle(knob_x, cy, 9, Color::hex(0xffffff, alpha));
    add_hit(r.inset(-4, -4), disabled ? Action::Disabled : action, std::move(arg));
}

float App::segmented(float right, float cy, const std::vector<std::wstring>& options, int selected, Action action,
                     const std::string& arg, bool disabled) {
    // .seg: 3 px padding, 2 px gap, buttons min 34 wide, 30 high, 10 px side padding, 13 px text.
    Graphics& g = *graphics_;
    const TextStyle style{13, Weight::Regular};
    const TextStyle on_style{13, Weight::Semibold};
    std::vector<float> widths;
    float total = 3 + 3;
    for (const std::wstring& option : options) {
        const float w = std::max(34.0f, std::ceil(g.text_width(option, on_style)) + 20);
        widths.push_back(w);
        total += w;
    }
    total += 2.0f * static_cast<float>(options.empty() ? 0 : options.size() - 1);
    const Rect r{right - total, cy - 18, total, 36};
    const float alpha = disabled ? 0.42f : 1.0f;
    g.fill_round(r, kPageRadius, kInk.with_alpha(0.06f * alpha));
    g.stroke_round(r, kPageRadius, kLine.with_alpha(alpha));
    float x = r.x + 3;
    for (std::size_t i = 0; i < options.size(); ++i) {
        const Rect b{x, r.y + 3, widths[i], 30};
        const std::string hit_arg = arg + ":" + std::to_string(i);
        const bool on = static_cast<int>(i) == selected;
        const bool hot = !disabled && hovered(action, hit_arg);
        if (on) g.fill_round(b, 7, kAccent.with_alpha(alpha));
        const Color fg = on ? kOnAccent : hot ? kInk : kMute;
        g.text(options[i], b, on ? on_style : style, fg.with_alpha(alpha), Align::Center);
        add_hit(b, disabled ? Action::Disabled : action, hit_arg);
        x += widths[i] + 2;
    }
    return total;
}

void App::select_box(const Rect& rect, std::wstring_view value, Action action, std::string arg, bool disabled) {
    // select: 36 high, --r corners, ink 5 % fill, a chevron on the right.
    Graphics& g = *graphics_;
    const bool hot = !disabled && hovered(action, arg);
    const float alpha = disabled ? 0.42f : 1.0f;
    g.fill_round(rect, kPageRadius, kInk.with_alpha((hot ? 0.09f : 0.05f) * alpha));
    g.stroke_round(rect, kPageRadius, (hot ? kInk.mix(kLine, 0.3f) : kLine).with_alpha(alpha));
    g.text(value, Rect{rect.x + 12, rect.y, rect.w - 12 - 30, rect.h}, TextStyle{13.5f}, kInk.with_alpha(alpha));
    g.icon(glyph::kChevronDown, Rect{rect.right() - 28, rect.y, 18, rect.h}, 10, kInk.with_alpha(alpha));
    add_hit(rect, disabled ? Action::Disabled : action, std::move(arg));
}

void App::slider(const Rect& rect, float value, const std::string& arg, bool disabled, Color fill) {
    // input[type=range] with accent-color: a 4 px track filled up to a 16 px knob.
    Graphics& g = *graphics_;
    value = std::clamp(value, 0.0f, 1.0f);
    const float alpha = disabled ? 0.42f : 1.0f;
    const Color accent = fill.a > 0 ? fill : kAccent;
    const float cy = rect.y + rect.h / 2;
    const Rect track{rect.x + 8, cy - 4, rect.w - 16, 8};
    const float knob_x = track.x + track.w * value;
    g.fill_round(track, 4, Color::hex(0xd5d9e0).with_alpha(alpha));
    g.fill_round(Rect{track.x, track.y, knob_x - track.x + 4, track.h}, 4, accent.with_alpha(alpha));
    const bool hot = !disabled && (hovered(Action::Drag, arg) || dragging_ == arg);
    g.fill_circle(knob_x, cy, hot ? 9.0f : 8.0f, accent.with_alpha(alpha));
    if (!disabled) tracks_[arg] = track;
    add_hit(rect, disabled ? Action::Disabled : Action::Drag, arg);
}

void App::soon_badge(float x, float cy) {
    Graphics& g = *graphics_;
    const Rect badge{x, cy - 8, 38, 16};
    g.fill_round(badge, 8, kInk.with_alpha(0.07f));
    g.text(L"Soon", badge, TextStyle{10, Weight::Semibold}, kMute, Align::Center);
}

float App::key_chips(float x, float y, const std::string& hotkey) {
    std::wstring text = to_wide(hotkey);
    for (std::size_t start = 0; start <= text.size();) {
        const std::size_t plus = text.find(L'+', start);
        x = key_chip(x, y, text.substr(start, plus == std::wstring::npos ? std::wstring::npos : plus - start)) + 3;
        if (plus == std::wstring::npos) break;
        start = plus + 1;
    }
    return x - 3;
}

// ---------------------------------------------------------------------------------------------
// Menus: drawn on top of the page; a click outside closes them.

void App::open_menu(Menu menu) {
    if (editing_) end_edit(true);
    menu_ = std::move(menu);
    hover_.reset();
    hide_page_controls();  // a child window would sit on top of the menu
    invalidate();
}

void App::close_menu() {
    if (!menu_) return;
    menu_.reset();
    invalidate();
}

void App::paint_menu(float width, float height) {
    Graphics& g = *graphics_;
    const Menu& menu = *menu_;
    add_hit(Rect{0, 0, width, height}, Action::MenuDismiss);
    // .menu: 6 px padding, items 8/10 px padding (34 high), 13.5 px text; separators 9 px.
    const TextStyle style{13.5f};
    float w = std::max(menu.min_width, 210.0f);
    float h = 12;
    for (const MenuItem& item : menu.items) {
        if (!item.run) {
            h += 9;
            continue;
        }
        const float extra = (item.dot ? 18.0f : 0.0f) + 24.0f;  // dot, check mark
        w = std::max(w, std::min(320.0f, g.text_width(item.label, item.checked ? TextStyle{13.5f, Weight::Semibold} : style) +
                                              20 + extra + 12));
        h += 34;
    }
    h = std::min(h, height * 0.7f);
    float x = menu.anchor.x;
    float y = menu.at_point ? menu.anchor.y : menu.anchor.bottom() + 6;
    if (!menu.at_point && menu.anchor.w > w) w = menu.anchor.w;
    if (!menu.at_point && x + w > width - 8) x = menu.anchor.right() - w;  // right-aligned under its button
    if (y + h > height - 8) y = menu.at_point ? y - h : menu.anchor.y - 6 - h;  // no room below: above
    x = std::clamp(x, 8.0f, std::max(8.0f, width - 8 - w));
    y = std::clamp(y, kTitleBar / 2, std::max(kTitleBar / 2, height - 8 - h));
    const Rect box{x, y, w, h};
    g.fill_round(Rect{box.x + 2, box.y + 10, box.w - 4, box.h}, 14, Color::hex(0x000000, 0.35f));
    g.fill_round(box, 12, kPop);
    g.stroke_round(box, 12, kLine);
    add_hit(box, Action::None);
    g.push_clip(box);
    // Scrolled: the wheel moves the items; never past the last one.
    float content = 12;
    for (const MenuItem& item : menu.items) content += item.run ? 34.0f : 9.0f;
    const float max_scroll = std::max(0.0f, content - box.h);
    if (menu_->scroll > max_scroll) menu_->scroll = max_scroll;
    float iy = box.y + 6 - menu_->scroll;
    for (std::size_t i = 0; i < menu.items.size(); ++i) {
        const MenuItem& item = menu.items[i];
        if (!item.run) {
            g.fill(Rect{box.x + 10, iy + 4, box.w - 20, 1}, kLine);
            iy += 9;
            continue;
        }
        const Rect r{box.x + 6, iy, box.w - 12, 34};
        if (r.y < box.y || r.bottom() > box.bottom()) {
            iy += 34;  // scrolled out of the box: not drawn, not clickable
            continue;
        }
        const std::string arg = std::to_string(i);
        if (item.enabled && hovered(Action::MenuItem, arg)) g.fill_round(r, 8, kHover);
        float tx = r.x + 10;
        if (item.dot) {
            g.fill_circle(tx + 4, r.y + r.h / 2, 4.5f, *item.dot);
            tx += 18;
        }
        Color fg = item.danger ? kDanger : item.checked ? kAccent : kInk;
        if (!item.enabled) fg = kMute.with_alpha(0.6f);
        g.text(item.label, Rect{tx, r.y, r.right() - tx - 28, r.h},
               item.checked ? TextStyle{13.5f, Weight::Semibold} : style, fg);
        if (item.checked) g.icon(glyph::kCheck, Rect{r.right() - 26, r.y, 18, r.h}, 12, kAccent);
        add_hit(r, item.enabled ? Action::MenuItem : Action::None, arg);
        iy += 34;
    }
    g.pop_clip();
}

}  // namespace bt::ui
