// Settings > Appearance > Customise (backtrack.html stPaintPan, opened as a dialog from Settings):
// the four colours a theme is made of, each with a colour picker, and a background picture with
// Dim and Blur. Every change applies at once; colours dragged in the picker show while dragging and
// are saved on release.
//
// The picture is copied next to library.json ("background.<ext>") so it keeps working when the
// original is moved or deleted, then read, scaled to at most 1920 px and blurred on the background
// worker. Only that copy is ever removed by Backtrack.

#include "core/Blur.h"
#include "platform/Log.h"
#include "platform/Picture.h"
#include "platform/Shell.h"
#include "platform/Text.h"
#include "ui/App.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace bt::ui {

using namespace theme;

namespace {

namespace fs = std::filesystem;

constexpr int kMaxPictureSide = 1920;
constexpr std::uintmax_t kMaxPictureBytes = 50ull * 1024 * 1024;
constexpr int kMaxDim = 90;
constexpr int kMaxBlur = 24;

// The mockup's "Surface" is called Cards here: that is what people see it on.
constexpr const wchar_t* kColourNames[] = {L"Accent", L"Background", L"Cards", L"Text"};
constexpr const wchar_t* kColourUses[] = {L"Buttons, highlights, the selected item", L"Behind everything",
                                          L"Clip cards, panels and this window", L"Titles, labels and clip names"};

std::uint32_t colour_at(const ThemeInput& in, int index) {
    switch (index) {
        case 1: return in.bg.value();
        case 2: return in.surface.value();
        case 3: return in.text.value();
        default: return in.accent.value();
    }
}

std::wstring hex_text(std::uint32_t rgb) {
    return L"#" + to_wide(colour_text(rgb));
}

std::wstring box_text(HWND box) {
    const int length = GetWindowTextLengthW(box);
    std::wstring text(static_cast<std::size_t>(std::max(length, 0)) + 1, L'\0');
    const int copied = GetWindowTextW(box, text.data(), static_cast<int>(text.size()));
    text.resize(static_cast<std::size_t>(std::max(copied, 0)));
    return text;
}

// The blur setting is in DIPs of a picture that fills a 1920 px window; a smaller picture is
// stretched more, so it needs less blur in its own pixels.
int blur_radius(int blur, int width, int height) {
    const double sigma = blur * static_cast<double>(std::max(width, height)) / 1920.0;
    return box_radius_for(sigma);
}

}  // namespace

float App::sidebar_width() const {
    return settings_.sidebar_compact ? kSidebarCompact : kSidebar;
}

Settings App::with_custom_colour(Settings s, int index, std::uint32_t rgb) {
    // Editing starts from the colours on screen, so changing one colour of a preset keeps the other three.
    const ThemeInput now = theme_input(s);
    s.custom_accent = now.accent.value();
    s.custom_background = now.bg.value();
    s.custom_surface = now.surface.value();
    s.custom_text = now.text.value();
    switch (index) {
        case 1: s.custom_background = rgb; break;
        case 2: s.custom_surface = rgb; break;
        case 3: s.custom_text = rgb; break;
        default: s.custom_accent = rgb; break;
    }
    s.theme = ThemeId::Custom;
    return s;
}

void App::open_customise() {
    if (dialog_open()) return;
    if (editing_) end_edit(true);
    close_menu();
    customise_.emplace();
    invalidate();
}

void App::close_customise() {
    if (!customise_) return;
    if (customise_->picking && customise_->colour_preview) {
        // Closed in the middle of a drag (Escape): the colour on screen is kept.
        set_custom_colour(*customise_->picking, *customise_->colour_preview, true);
    }
    if (customise_->dim_preview) {
        const int dim = *customise_->dim_preview;
        change_settings([dim](Settings& s) { s.background_dim = dim; });
    }
    if (customise_->blur_preview) {
        const int blur = *customise_->blur_preview;
        change_settings([blur](Settings& s) { s.background_blur = blur; });
    }
    customise_.reset();
    if (GetFocus() == hex_box_) SetFocus(window_);
    apply_look();
}

void App::open_picker(int index) {
    if (!customise_ || index < 0 || index > 3) return;
    customise_->picking = index;
    customise_->colour_preview.reset();
    const std::uint32_t rgb = colour_at(theme_input(settings_), index);
    customise_->hsv = to_hsv(Rgb::hex(rgb));
    setting_hex_ = true;
    SetWindowTextW(hex_box_, hex_text(rgb).c_str());
    setting_hex_ = false;
    invalidate();
}

void App::close_picker() {
    if (!customise_ || !customise_->picking) return;
    if (customise_->colour_preview) set_custom_colour(*customise_->picking, *customise_->colour_preview, true);
    customise_->picking.reset();
    if (GetFocus() == hex_box_) SetFocus(window_);
    invalidate();
}

void App::set_custom_colour(int index, std::uint32_t rgb, bool save) {
    if (save) {
        if (customise_) customise_->colour_preview.reset();
        change_settings([index, rgb](Settings& s) { s = with_custom_colour(s, index, rgb); });
    } else if (customise_) {
        customise_->colour_preview = rgb;
    }
    apply_look();  // also when nothing was saved: a preview may have been showing
}

void App::on_hex_changed() {
    if (setting_hex_ || !customise_ || !customise_->picking) return;
    const std::optional<std::uint32_t> rgb = parse_colour(to_utf8(box_text(hex_box_)));
    if (!rgb) return;  // still typing
    customise_->hsv = to_hsv(Rgb::hex(*rgb));
    set_custom_colour(*customise_->picking, *rgb, true);
}

void App::run_customise(const std::string& what) {
    if (what == "cus-open") {
        open_customise();
        return;
    }
    if (!customise_) return;
    if (what.starts_with("cus-col:")) {
        const int index = what.back() - '0';
        if (customise_->picking == index) {
            customise_->picking.reset();
        } else {
            open_picker(index);
        }
        invalidate();
        return;
    }
    customise_->picking.reset();  // anything else closes the picker
    if (GetFocus() == hex_box_) SetFocus(window_);
    if (what == "cus-done") {
        close_customise();
    } else if (what == "cus-img") {
        choose_background();
    } else if (what == "cus-img-rm") {
        customise_->problem.clear();
        change_settings([](Settings& s) { s.background_image.clear(); });
    } else if (what == "cus-reset") {
        customise_->problem.clear();
        const Settings defaults;
        change_settings([&defaults](Settings& s) {
            s.theme = defaults.theme;
            s.custom_accent = defaults.custom_accent;
            s.custom_background = defaults.custom_background;
            s.custom_surface = defaults.custom_surface;
            s.custom_text = defaults.custom_text;
            s.background_image = defaults.background_image;
            s.background_dim = defaults.background_dim;
            s.background_blur = defaults.background_blur;
        });
    }
    invalidate();
}

void App::drag_customise(const std::string& id, float fraction, bool done) {
    if (!customise_) return;
    CustomiseView& view = *customise_;
    const auto track = tracks_.find(id);
    if ((id == "cus:sv" || id == "cus:hue") && view.picking) {
        if (fraction >= 0 && track != tracks_.end()) {
            if (id == "cus:sv") {
                view.hsv.s = fraction;
                view.hsv.v = track->second.h > 0
                                 ? 1.0 - std::clamp((mouse_y_ - track->second.y) / track->second.h, 0.0f, 1.0f)
                                 : view.hsv.v;
            } else {
                view.hsv.h = std::min(static_cast<double>(fraction) * 360.0, 359.9);
            }
            const std::uint32_t rgb = from_hsv(view.hsv).value();
            setting_hex_ = true;
            SetWindowTextW(hex_box_, hex_text(rgb).c_str());
            setting_hex_ = false;
            set_custom_colour(*view.picking, rgb, done);
        } else if (done && view.colour_preview) {
            set_custom_colour(*view.picking, *view.colour_preview, true);
        }
    } else if (id == "cus:dim") {
        if (fraction >= 0) view.dim_preview = static_cast<int>(std::lround(fraction * kMaxDim));
        if (done && view.dim_preview) {
            const int dim = *std::exchange(view.dim_preview, std::nullopt);
            change_settings([dim](Settings& s) { s.background_dim = dim; });
        }
    } else if (id == "cus:blur") {
        if (fraction >= 0) view.blur_preview = static_cast<int>(std::lround(fraction * kMaxBlur));
        if (done && view.blur_preview) {
            const int blur = *std::exchange(view.blur_preview, std::nullopt);
            change_settings([blur](Settings& s) { s.background_blur = blur; });
        }
    }
    invalidate();
}

void App::choose_background() {
    // One picture at a time, also across closing and reopening the dialog: two reads would write
    // the same background.<ext>, and the older one's cleanup would delete the newer one's copy.
    if (shell_busy_ || finished_ || !customise_ || background_loading_) return;
    shell_busy_ = true;  // the picker pumps messages: a second click must not open a second one
    const std::optional<fs::path> chosen = shell::pick_picture(window_, L"Choose a background picture");
    shell_busy_ = false;
    if (quit_pending_) {
        quit();
        return;
    }
    if (!chosen || window_ == nullptr || !customise_) return;
    std::error_code ec;
    const std::uintmax_t bytes = fs::file_size(*chosen, ec);
    if (!ec && bytes > kMaxPictureBytes) {
        customise_->problem = L"That picture is over 50 MB. Choose a smaller one.";
        invalidate();
        return;
    }
    customise_->problem.clear();
    background_loading_ = true;
    std::wstring extension = chosen->extension().wstring();
    if (!extension.empty()) CharLowerBuffW(extension.data(), static_cast<DWORD>(extension.size()));
    if (extension.empty() || extension.size() > 6) extension = L".picture";
    const fs::path destination = background_path(to_utf8(L"background" + extension));
    const int blur = settings_.background_blur;
    const std::uint64_t request = ++backdrop_request_;
    const fs::path source = *chosen;
    log::info("Background picture chosen: {}", to_utf8(source.wstring()));
    run_in_background([this, source, destination, blur, request]() -> std::function<void()> {
        try {
        std::string error;
        std::optional<Picture> picture = load_picture(source, kMaxPictureSide, error);
        if (picture) {
            // Backtrack's own copy, so the picture stays when the original is moved or deleted.
            std::error_code copy_ec;
            if (!fs::equivalent(source, destination, copy_ec)) {
                fs::path temp = destination;
                temp += L".tmp";
                copy_ec.clear();
                fs::copy_file(source, temp, fs::copy_options::overwrite_existing, copy_ec);
                if (!copy_ec) fs::rename(temp, destination, copy_ec);
                if (copy_ec) {
                    log::warn("Could not copy the background picture to {}: {}", to_utf8(destination.wstring()),
                              copy_ec.message());
                    std::error_code ignored;
                    fs::remove(temp, ignored);
                    error = "Couldn't save a copy of that picture. Check that the drive has free space.";
                    picture.reset();
                }
            }
        }
        auto made = std::make_shared<Backdrop>();
        if (picture) {
            made->file = to_utf8(destination.filename().wstring());  // found next to library.json, wherever that is
            made->blur = blur;
            made->width = picture->width;
            made->height = picture->height;
            made->blurred = picture->bgra;
            box_blur_bgra(made->blurred, made->width, made->height, blur_radius(blur, made->width, made->height));
            made->original = std::make_shared<const std::vector<std::uint8_t>>(std::move(picture->bgra));
        } else {
            log::warn("The background picture cannot be used: {}", error);
        }
        return [this, made, error, request, destination] {
            background_loading_ = false;
            if (request != backdrop_request_) {
                // A removal or reset came after it: the copy just made is nobody's. (Choosing again
                // waits for this one, so no newer choice can be using the same file.)
                if (made->original != nullptr && background_path(settings_.background_image) != destination) {
                    std::error_code ignored;
                    fs::remove(destination, ignored);
                }
                invalidate();
                return;
            }
            if (made->original == nullptr) {
                if (customise_) customise_->problem = to_wide(error);
                invalidate();
                return;
            }
            const std::string file = made->file;
            backdrop_ = std::move(*made);  // runs once: the job's result is not needed after this
            backdrop_problem_.clear();
            graphics_->forget_bitmap(kBackdropKey);
            change_settings([&file](Settings& s) { s.background_image = file; });
            apply_look();  // the window's surfaces turn see-through
        };
        } catch (const std::exception& e) {
        // Out of memory on a huge picture, and the like: say so instead of "Reading..." forever.
        log::warn("Reading the background picture failed: {}", e.what());
        return [this] {
            background_loading_ = false;
            if (!customise_) return;
            customise_->problem = L"That picture could not be read. Try a smaller one.";
            invalidate();
        };
      }
    });
    invalidate();
}

std::filesystem::path App::background_path(const std::string& setting) const {
    // Usually just "background.jpg": it sits next to library.json, so a moved Backtrack folder
    // keeps its picture. A full path (written by hand) is used as it is.
    const fs::path path(to_wide(setting));
    return path.is_absolute() ? path : paths_.library.parent_path() / path;
}

void App::load_background() {
    const std::string file = settings_.background_image;
    const int blur = settings_.background_blur;
    if (file.empty()) {
        ++backdrop_request_;  // a read still running is not wanted any more
        backdrop_problem_.clear();
        if (backdrop_) {
            backdrop_.reset();
            if (graphics_) graphics_->forget_bitmap(kBackdropKey);
            apply_look();
        }
        return;
    }
    if (backdrop_ && backdrop_->file == file && backdrop_->blur == blur) return;
    // A newly chosen picture is on its way and will be applied with the settings then; reading
    // the old one now would only make that choice look stale and be thrown away.
    if (background_loading_) return;
    const std::uint64_t request = ++backdrop_request_;
    // Only the blur changed: the picture is not read again.
    std::shared_ptr<const std::vector<std::uint8_t>> original;
    int width = 0;
    int height = 0;
    if (backdrop_ && backdrop_->file == file) {
        original = backdrop_->original;
        width = backdrop_->width;
        height = backdrop_->height;
    }
    const fs::path path = background_path(file);
    run_in_background([this, file, path, blur, request, original, width, height]() -> std::function<void()> {
        try {
        auto made = std::make_shared<Backdrop>();
        made->file = file;
        made->blur = blur;
        std::string error;
        if (original) {
            made->original = original;
            made->width = width;
            made->height = height;
        } else if (std::optional<Picture> picture = load_picture(path, kMaxPictureSide, error)) {
            made->width = picture->width;
            made->height = picture->height;
            made->original = std::make_shared<const std::vector<std::uint8_t>>(std::move(picture->bgra));
        } else {
            return [this, request, error, file] {
                if (request != backdrop_request_) return;
                log::warn("The background picture {} cannot be read: {}", file, error);
                backdrop_problem_ = L"Your background picture couldn't be found. Choose it again.";
                // Also outside Customise: otherwise the picture just vanishes without a word.
                show_notice(L"Your background picture couldn't be found. Choose it again in Settings > Appearance.");
                if (backdrop_) {
                    backdrop_.reset();
                    graphics_->forget_bitmap(kBackdropKey);
                    apply_look();
                }
                invalidate();
            };
        }
        made->blurred = *made->original;
        box_blur_bgra(made->blurred, made->width, made->height, blur_radius(blur, made->width, made->height));
        return [this, request, made] {
            if (request != backdrop_request_) return;
            backdrop_ = std::move(*made);  // runs once
            backdrop_problem_.clear();
            graphics_->forget_bitmap(kBackdropKey);
            apply_look();
        };
        } catch (const std::exception& e) {
        log::warn("Reading the background picture failed: {}", e.what());
        return {};
      }
    });
}

void App::apply_ui_scale() {
    if (!graphics_) return;
    if (editing_) end_edit(true);
    close_menu();
    graphics_->set_zoom(static_cast<float>(settings_.ui_scale) / 100.0f);
    update_edit_font();  // the text boxes' font, and the search box's place
    // Bigger everything needs a bigger window: Windows only applies the new minimum size on the
    // next resize, so the window grows to it now (never while maximised: that already fills the screen).
    if (!IsZoomed(window_) && !IsIconic(window_)) {
        MINMAXINFO limits{};
        SendMessageW(window_, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
        RECT now{};
        GetWindowRect(window_, &now);
        const LONG w = std::max(now.right - now.left, limits.ptMinTrackSize.x);
        const LONG h = std::max(now.bottom - now.top, limits.ptMinTrackSize.y);
        if (w != now.right - now.left || h != now.bottom - now.top) {
            SetWindowPos(window_, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    hover_.reset();
    invalidate();
}

void App::paint_customise(float width, float height) {
    if (!customise_) return;
    Graphics& g = *graphics_;
    CustomiseView& view = *customise_;
    // A lighter scrim than the other dialogs: the window behind is the preview.
    g.fill(Rect{0, kTitleBar, width, height - kTitleBar}, Color::hex(0x000000, kLightTheme ? 0.15f : 0.3f));
    add_hit(Rect{0, kTitleBar, width, height - kTitleBar}, Action::Disabled);
    const ThemeInput shown = [&] {
        Settings look = settings_;
        if (view.picking && view.colour_preview) look = with_custom_colour(look, *view.picking, *view.colour_preview);
        return theme_input(look);
    }();
    // .st-hint: what the contrast correction changed.
    const ContrastNote note = contrast_note(shown);
    std::wstring hint;
    if (note.unmet) {
        hint = L"These colours are too far apart for text to read well on both. Make the background and cards more alike.";
    } else if (note.text_adjusted || note.accent_adjusted) {
        hint = note.text_adjusted && note.accent_adjusted ? L"The text and accent were made a little lighter or darker "
                                                            L"so they stay readable."
               : note.text_adjusted ? L"The text was made a little lighter or darker so it stays readable."
                                    : L"Accent-coloured text was made a little lighter or darker so it stays readable.";
    }
    const bool has_picture = backdrop_.has_value();
    std::wstring problem = !view.problem.empty() ? view.problem : backdrop_problem_;
    const bool problem_is_hint = problem.empty() && !has_picture && !background_loading_;
    if (problem_is_hint) problem = L"Choose a picture to use Dim and Blur.";
    // Only as tall as what it shows, so it fits a short window (125 % on a small laptop screen).
    const float panel_h = 390 + (hint.empty() ? 0.0f : 38.0f) + (problem.empty() ? 0.0f : 22.0f);
    const float w = std::min(420.0f, width - 32);
    const float x = (width - w) / 2;
    const Rect panel{x, std::max(kTitleBar + 8, (height - panel_h) / 2), w, panel_h};
    g.fill_round(Rect{panel.x + 2, panel.y + 10, panel.w - 4, panel.h}, 18, Color::hex(0x000000, 0.3f));
    g.fill_round(panel, kPanelRadius, kCard.with_alpha(1.0f));
    g.stroke_round(panel, kPanelRadius, kLine);
    add_hit(panel, Action::Disabled);
    // With the picker open, a click anywhere else in the window closes it.
    if (view.picking) add_hit(Rect{0, kTitleBar, width, height - kTitleBar}, Action::Setting, "cus-pick-close");
    const float px = panel.x + 24;
    const float pw = panel.w - 48;
    float y = panel.y + 20;

    g.text(L"Customise", Rect{px, y, pw - 40, 28}, TextStyle{18, Weight::Bold}, kInk);
    icon_button(Rect{px + pw - 30, y - 1, 30, 30}, glyph::kClose, 12, Action::Setting, "cus-done");
    y += 42;

    // Your colours: a 2 x 2 grid of chips (.st-cols).
    const TextStyle heading{12, Weight::Semibold};
    g.text(L"Your colours", Rect{px, y, pw, 18}, heading, kMute);
    y += 24;
    const float chip_w = (pw - 6) / 2;
    for (int i = 0; i < 4; ++i) {
        const Rect chip{px + (i % 2) * (chip_w + 6), y + (i / 2) * 40.0f, chip_w, 34};
        const std::string arg = std::format("cus-col:{}", i);
        const bool open = view.picking == i;
        if (open) view.chip = chip;
        if (hovered(Action::Setting, arg) && !open) g.fill_round(chip, 8, kHover);
        g.stroke_round(chip, 8, open ? kAccent : hovered(Action::Setting, arg) ? kOff : kLine, open ? 2.0f : 1.0f);
        const std::uint32_t rgb = colour_at(shown, i);
        const Rect swatch{chip.x + 6, chip.y + 5, 24, 24};
        g.fill_round(swatch, 6, Color::hex(rgb));
        g.stroke_round(swatch, 6, kInk.with_alpha(0.3f));
        g.text(kColourNames[i], Rect{swatch.right() + 8, chip.y, chip.w - 110, chip.h}, TextStyle{12.5f}, kInk);
        g.text(hex_text(rgb), Rect{chip.right() - 72, chip.y, 64, chip.h}, TextStyle{11.5f, Weight::Regular, false, true},
               kMute, Align::End);
        add_hit(chip, Action::Setting, arg);
    }
    y += 80;
    if (!hint.empty()) {
        g.paragraph(hint, Rect{px, y, pw, 34}, TextStyle{12}, note.unmet ? kWarn : kAccent);
        y += 38;
    }
    g.fill(Rect{px, y, pw, 1}, kLine);
    y += 14;

    // Background picture. While one is being read, nothing that changes it can be used: its
    // result would be thrown away (and its copy left behind).
    const bool busy = background_loading_ || shell_busy_;
    g.text(L"Background picture", Rect{px, y, pw, 18}, heading, kMute);
    y += 26;
    const Rect preview{px, y + 3, 44, 28};
    if (!has_picture || !g.draw_bitmap(kBackdropKey, preview, 6)) g.fill_round(preview, 6, kInk.with_alpha(0.08f));
    g.stroke_round(preview, 6, kLine);
    float bx = preview.right() + 10;
    bx += 6 + push_button(bx, y + 2, background_loading_ ? L"Reading..." : has_picture ? L"Change image" : L"Choose image",
                          glyph::kPicture, ButtonStyle::Line, true, Action::Setting, "cus-img", busy);
    push_button(bx, y + 2, L"Remove image", 0, ButtonStyle::Ghost, true, Action::Setting, "cus-img-rm",
                settings_.background_image.empty() || busy);
    y += 38;
    if (!problem.empty()) {
        g.text(problem, Rect{px, y, pw, 18}, TextStyle{12}, problem_is_hint ? kMute : kWarn);
        y += 22;
    }
    const bool sliders_on = has_picture && !busy;
    const auto range = [&](const wchar_t* label, const char* id, float value, const std::wstring& value_text) {
        g.text(label, Rect{px, y, 46, 28}, TextStyle{12.5f}, sliders_on ? kMute : kOff);
        slider(Rect{px + 46, y, pw - 46 - 48, 28}, value, id, !sliders_on);
        g.text(value_text, Rect{px + pw - 44, y, 44, 28}, TextStyle{12.5f, Weight::Regular, false, true},
               sliders_on ? kInk : kOff, Align::End);
        y += 32;
    };
    const int dim = view.dim_preview.value_or(settings_.background_dim);
    const int blur = view.blur_preview.value_or(settings_.background_blur);
    range(L"Dim", "cus:dim", static_cast<float>(dim) / kMaxDim, std::format(L"{}%", dim));
    range(L"Blur", "cus:blur", static_cast<float>(blur) / kMaxBlur, std::format(L"{}px", blur));
    y += 10;
    g.fill(Rect{px, y, pw, 1}, kLine);
    y += 14;

    // .st-pf: Reset look on the left (back to Night shift, no picture), Done on the right.
    const Settings defaults;
    const bool look_is_default =
        settings_.theme == defaults.theme && settings_.custom_accent == defaults.custom_accent &&
        settings_.custom_background == defaults.custom_background && settings_.custom_surface == defaults.custom_surface &&
        settings_.custom_text == defaults.custom_text && settings_.background_image.empty() &&
        settings_.background_dim == defaults.background_dim && settings_.background_blur == defaults.background_blur;
    push_button(px, y, L"Reset look", glyph::kRestoreDefaults, ButtonStyle::Ghost, false, Action::Setting, "cus-reset",
                look_is_default || busy);
    const float done_w = button_width(L"Done", 0, false);
    push_button(px + pw - done_w, y, L"Done", 0, ButtonStyle::Primary, false, Action::Setting, "cus-done");

    if (!view.picking) {
        controls_[hex_box_] = {Rect{}, false};
        return;
    }
    // The colour picker, under its chip (above it when there is no room below).
    const float pick_w = 240;
    const float pick_h = 10 + 18 + 6 + 150 + 12 + 14 + 12 + 32 + 12;
    const float pick_x = std::clamp(view.chip.x, 8.0f, width - pick_w - 8);
    const float below = view.chip.bottom() + 6;
    const float pick_y = below + pick_h <= height - 8 ? below : std::max(kTitleBar + 8, view.chip.y - 6 - pick_h);
    const Rect pop{pick_x, pick_y, pick_w, pick_h};
    g.fill_round(Rect{pop.x + 2, pop.y + 8, pop.w - 4, pop.h}, 14, Color::hex(0x000000, 0.35f));
    g.fill_round(pop, 12, kPop);
    g.stroke_round(pop, 12, kLine);
    add_hit(pop, Action::Disabled);
    const Hsv hsv = view.hsv;
    // What this colour paints, so "Cards" or "Accent" needs no guessing.
    g.text(kColourUses[std::clamp(*view.picking, 0, 3)], Rect{pop.x + 12, pop.y + 10, pop.w - 24, 18},
           TextStyle{12}, kMute);
    // Saturation from left to right, brightness from top to bottom, over the hue.
    const Rect sv{pop.x + 12, pop.y + 34, pop.w - 24, 150};
    const Rgb pure = from_hsv(Hsv{hsv.h, 1.0, 1.0});
    g.fill_round(sv, 6, Color::hex(pure.value()));
    g.fill_gradient(sv, 6, Color::hex(0xffffff), Color::hex(0xffffff, 0.0f), true);
    g.fill_gradient(sv, 6, Color::hex(0x000000, 0.0f), Color::hex(0x000000));
    const float mx = sv.x + sv.w * static_cast<float>(hsv.s);
    const float my = sv.y + sv.h * static_cast<float>(1.0 - hsv.v);
    g.fill_circle(mx, my, 7.5f, Color::hex(0x000000, 0.45f));
    g.fill_circle(mx, my, 6.5f, Color::hex(0xffffff));
    g.fill_circle(mx, my, 5, Color::hex(from_hsv(hsv).value()));
    tracks_["cus:sv"] = sv;
    add_hit(sv, Action::Drag, "cus:sv");
    // Hue.
    const Rect hue{sv.x, sv.bottom() + 12, sv.w, 14};
    g.fill_hues(hue, 7);
    const float hx = hue.x + hue.w * static_cast<float>(hsv.h / 360.0);
    g.fill_circle(hx, hue.y + hue.h / 2, 8.5f, Color::hex(0x000000, 0.45f));
    g.fill_circle(hx, hue.y + hue.h / 2, 7.5f, Color::hex(0xffffff));
    g.fill_circle(hx, hue.y + hue.h / 2, 5.5f, Color::hex(pure.value()));
    tracks_["cus:hue"] = hue;
    add_hit(Rect{hue.x - 4, hue.y - 5, hue.w + 8, hue.h + 10}, Action::Drag, "cus:hue");
    // Hex: type or paste "#rrggbb" (the edit control sits inside the drawn box).
    const Rect hex_row{sv.x, hue.bottom() + 12, sv.w, 32};
    const Rect now_swatch{hex_row.x, hex_row.y, 32, 32};
    g.fill_round(now_swatch, 7, Color::hex(from_hsv(hsv).value()));
    g.stroke_round(now_swatch, 7, kInk.with_alpha(0.3f));
    const Rect field{now_swatch.right() + 8, hex_row.y, hex_row.w - 40, 32};
    g.fill_round(field, 7, kRaise);
    g.stroke_round(field, 7, GetFocus() == hex_box_ ? kAccent : kLine);
    g.text(L"Hex", Rect{field.right() - 44, field.y, 36, field.h}, TextStyle{11.5f}, kMute, Align::End);
    controls_[hex_box_] = {Rect{field.x + 10, field.y + 7, field.w - 60, 18}, true};
}

}  // namespace bt::ui
