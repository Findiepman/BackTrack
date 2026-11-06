// The Export window of the clip page (backtrack.html exportModal): format, resolution, quality and
// sound, an estimated size, then a progress bar while platform/Exporter re-encodes the clip on the
// background worker. Exports go to an "Exports" folder inside the clips folder, so the library
// (which lists the clips folder itself) does not show them as new clips.

#include "platform/Exporter.h"
#include "platform/Log.h"
#include "platform/Shell.h"
#include "platform/Text.h"
#include "ui/App.h"
#include "ui/AppIds.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace bt::ui {

using namespace theme;
using namespace ids;

namespace {

namespace fs = std::filesystem;

constexpr int kHeights[] = {0, 1440, 1080, 720, 480};

std::wstring height_name(int height) {
    return height == 0 ? std::wstring(L"Original") : std::format(L"{}p", height);
}

std::wstring megabytes(std::uint64_t bytes) {
    const double mb = static_cast<double>(bytes) / 1e6;
    return mb < 10 ? std::format(L"{:.1f} MB", mb) : std::format(L"{:.0f} MB", mb);
}

}  // namespace

void App::open_export() {
    if (!clip_view_ || export_) return;
    const ClipEntry* clip = library_.find(clip_view_->file);
    if (clip == nullptr || !clips_folder_available()) return;
    if (clip_view_->player) clip_view_->player->pause();
    auto view = std::make_shared<ExportView>();
    view->file = clip_view_->file;
    view->title = display_title(*clip);
    const double length = clip_view_->player && clip_view_->player->duration() > 0 ? clip_view_->player->duration()
                                                                                    : clip->duration_s;
    view->options.in_seconds = clip_view_->in;
    view->options.out_seconds = clip_view_->out > 0 && clip_view_->out < length - 0.05 ? clip_view_->out : 0.0;
    view->options.height = 1080;
    // Settings > Encoder = processor: the graphics card's encoder is not used for exports either
    // (it may be the reason the player picked that).
    view->options.hardware = settings_.encoder != EncoderPreference::Software;
    export_ = view;
    // What the file is (size, frame rate, sound) for the estimate: quick, but it opens the file.
    const fs::path source = clip_path(view->file);
    run_in_background([this, view, source]() -> std::function<void()> {
        std::optional<ExportSource> info = read_export_source(source);
        return [this, view, info] {
            if (export_ != view) return;  // closed meanwhile
            view->source = info;
            if (!info) {
                view->stage = ExportView::Stage::Failed;
                view->message = "This clip cannot be read. It may be damaged or still being written.";
            }
            invalidate();
        };
    });
    invalidate();
}

void App::close_export() {
    if (!export_) return;
    if (export_->stage == ExportView::Stage::Running) {
        export_->cancel->store(true);  // finishes on the worker
        show_notice(L"The export was cancelled.");  // also when something else closed the clip page
    }
    export_.reset();
    invalidate();
}

void App::run_export(const std::string& what, const Rect& anchor) {
    if (!export_) return;
    const std::shared_ptr<ExportView> view = export_;
    ExportOptions& o = view->options;
    const bool form = view->stage == ExportView::Stage::Form;
    if (what == "ex-close" || what == "ex-cancel") {
        close_export();
    } else if (what == "ex-folder") {
        shell::show_in_folder(view->result_path);
    } else if (form && what.starts_with("ex-fmt:")) {
        o.format = what.back() == '1' ? ExportFormat::Gif : ExportFormat::Mp4;
    } else if (form && what.starts_with("ex-q:")) {
        const int q = what.back() - '0';
        if (q >= 0 && q <= 3) o.quality = static_cast<ExportQuality>(q);
    } else if (form && what == "ex-audio") {
        o.audio = !o.audio;
    } else if (form && what == "ex-res") {
        Menu menu;
        menu.anchor = anchor;
        menu.min_width = anchor.w;
        for (const int h : kHeights) {
            menu.items.push_back(MenuItem{height_name(h), [this, view, h] {
                                              view->options.height = h;
                                              invalidate();
                                          },
                                          o.height == h});
        }
        open_menu(std::move(menu));
    } else if (form && what == "ex-go" && view->source) {
        const ExportPlan plan = plan_export(o, *view->source);
        if (!plan.problem.empty()) return;
        std::error_code ec;
        const fs::path folder = recorder_->output_dir() / L"Exports";
        fs::create_directories(folder, ec);
        const fs::path destination = free_export_path(folder, clip_path(view->file).stem().wstring(), o.format);
        if (destination.empty()) {
            view->stage = ExportView::Stage::Failed;
            view->message = "There is no free file name left in the Exports folder. Remove some exports first.";
            invalidate();
            return;
        }
        view->stage = ExportView::Stage::Running;
        view->progress->store(0.0);
        const fs::path source = clip_path(view->file);
        const ExportOptions options = o;
        const std::shared_ptr<std::atomic<double>> progress = view->progress;
        const std::shared_ptr<std::atomic<bool>> cancel = view->cancel;
        HWND window = window_;
        log::info("Exporting {} as {}", view->file, to_utf8(destination.filename().wstring()));
        run_in_background([this, view, source, destination, options, progress, cancel, window]() -> std::function<void()> {
            auto last = std::chrono::steady_clock::now();
            const ExportResult result = export_clip(
                source, destination, options,
                [&](double fraction) {
                    progress->store(fraction);
                    // A repaint about ten times a second, not for every frame.
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last > std::chrono::milliseconds(100)) {
                        last = now;
                        PostMessageW(window, kMsgExport, 0, 0);
                    }
                },
                *cancel);
            return [this, view, result] {
                if (result.ok) {
                    log::info("Exported {} ({} bytes)", to_utf8(result.path.filename().wstring()), result.bytes);
                } else if (!result.cancelled) {
                    log::warn("Export failed: {}", result.message);
                }
                if (export_ != view) return;  // the window was closed (that cancelled it)
                view->stage = result.ok ? ExportView::Stage::Done : ExportView::Stage::Failed;
                view->message = result.message;
                view->result_path = result.path;
                view->result_bytes = result.bytes;
                invalidate();
            };
        });
    }
    invalidate();
}

void App::paint_export(float width, float height) {
    if (!export_) return;
    Graphics& g = *graphics_;
    ExportView& view = *export_;
    ExportOptions& o = view.options;
    // .ovl: everything behind is dimmed and takes no clicks.
    g.fill(Rect{0, kTitleBar, width, height - kTitleBar}, Color::hex(0x000000, kLightTheme ? 0.3f : 0.5f));
    add_hit(Rect{0, kTitleBar, width, height - kTitleBar}, Action::Disabled);
    const float w = std::min(480.0f, width - 32);
    const float x = (width - w) / 2;
    const float panel_h = view.stage == ExportView::Stage::Form ? 420.0f : 220.0f;
    const Rect panel{x, std::max(kTitleBar + 16, (height - panel_h) / 2), w, panel_h};
    g.fill_round(Rect{panel.x + 2, panel.y + 10, panel.w - 4, panel.h}, 18, Color::hex(0x000000, 0.3f));
    g.fill_round(panel, kPanelRadius, kCard);
    g.stroke_round(panel, kPanelRadius, kLine);
    add_hit(panel, Action::Disabled);
    const float px = panel.x + 24;
    const float pw = panel.w - 48;
    float y = panel.y + 22;
    g.text(L"Export clip", Rect{px, y, pw, 26}, TextStyle{18, Weight::Bold}, kInk);
    y += 28;
    const double length = view.source ? view.source->duration_s : 0.0;
    const double end = o.out_seconds > 0 ? o.out_seconds : length;
    std::wstring sub = to_wide(view.title);
    if (end > o.in_seconds) sub += L" · " + to_wide(format_duration(end - o.in_seconds));
    g.text(sub, Rect{px, y, pw, 20}, TextStyle{13}, kMute);
    y += 34;

    if (view.stage == ExportView::Stage::Form) {
        const auto field = [&](const wchar_t* label) {
            g.text(label, Rect{px, y, pw * 0.4f, 38}, TextStyle{14, Weight::Semibold}, kInk);
        };
        const bool gif = o.format == ExportFormat::Gif;
        field(L"Format");
        segmented(px + pw, y + 19, {L"MP4", L"GIF"}, gif ? 1 : 0, Action::Clip, "ex-fmt");
        y += 48;
        field(L"Resolution");
        select_box(Rect{px + pw - 170, y + 1, 170, 36}, height_name(o.height), Action::Clip, "ex-res");
        y += 48;
        field(L"Quality");
        segmented(px + pw, y + 19, {L"High", L"Balanced", L"Small", L"Fits Discord"}, static_cast<int>(o.quality),
                  Action::Clip, "ex-q", gif);
        y += 48;
        g.text(gif ? L"Sound (a GIF has none)" : L"Include sound", Rect{px, y, pw * 0.6f, 38},
               TextStyle{14, Weight::Semibold}, gif ? kMute : kInk);
        toggle(px + pw, y + 19, o.audio && !gif, Action::Clip, "ex-audio", gif);
        y += 50;
        // .panel on --bg: the estimate, and what to know about it.
        const Rect info{px, y, pw, 76};
        g.fill_round(info, kPageRadius, kBg);
        std::wstring estimate = L"...";
        std::wstring note;
        bool problem = false;
        if (view.source) {
            const ExportPlan plan = plan_export(o, *view.source);
            if (!plan.problem.empty()) {
                note = to_wide(plan.problem);
                problem = true;
                estimate = L"-";
            } else {
                estimate = L"about " + megabytes(plan.estimated_bytes);
                note = !plan.note.empty() ? to_wide(plan.note)
                       : gif             ? std::wstring(L"GIFs are large and play at most 15 frames a second.")
                       : o.quality == ExportQuality::FitsDiscord
                           ? std::wstring(L"Under 10 MB: Discord's limit without Nitro.")
                           : std::wstring(L"Plays almost everywhere, including Discord and phones.");
                // The size Fits Discord picked is in its own note already.
                if (plan.width > 0 && plan.note.empty()) {
                    note = std::format(L"{}×{}. ", plan.width, plan.height) + note;
                }
            }
        }
        g.text(L"Estimated size", Rect{info.x + 14, info.y + 10, info.w / 2, 22}, TextStyle{13}, kMute);
        g.text(estimate, Rect{info.x + info.w / 2, info.y + 10, info.w / 2 - 14, 22},
               TextStyle{14, Weight::Bold, false, true}, kInk, Align::End);
        g.paragraph(note, Rect{info.x + 14, info.y + 36, info.w - 28, 36}, TextStyle{12.5f}, problem ? kWarn : kMute);
        y = info.bottom() + 20;
        const float go_w = button_width(L"Export", glyph::kDownload, false);
        push_button(px + pw - go_w, y, L"Export", glyph::kDownload, ButtonStyle::Primary, false, Action::Clip, "ex-go",
                    !view.source || problem);
        const float cancel_w = button_width(L"Cancel", 0, false);
        push_button(px + pw - go_w - 8 - cancel_w, y, L"Cancel", 0, ButtonStyle::Line, false, Action::Clip, "ex-close");
    } else if (view.stage == ExportView::Stage::Running) {
        const double fraction = std::clamp(view.progress->load(), 0.0, 1.0);
        g.text(L"Exporting...", Rect{px, y, pw / 2, 22}, TextStyle{14, Weight::Semibold}, kInk);
        g.text(std::format(L"{}%", std::lround(fraction * 100)), Rect{px + pw / 2, y, pw / 2, 22},
               TextStyle{14, Weight::Bold, false, true}, kInk, Align::End);
        y += 32;
        g.fill_round(Rect{px, y, pw, 8}, 4, kLine);
        g.fill_round(Rect{px, y, pw * static_cast<float>(fraction), 8}, 4, kAccent);
        y += 40;
        const float cw = button_width(L"Cancel export", 0, false);
        push_button(px + pw - cw, y, L"Cancel export", 0, ButtonStyle::Line, false, Action::Clip, "ex-cancel");
    } else {
        const bool ok = view.stage == ExportView::Stage::Done;
        g.text(ok ? L"Export finished" : L"Export did not work", Rect{px, y, pw, 22}, TextStyle{14, Weight::Semibold},
               ok ? kOk : kDanger);
        y += 28;
        const std::wstring detail = ok ? std::format(L"{} · {}", view.result_path.filename().wstring(),
                                                     megabytes(view.result_bytes))
                                       : to_wide(view.message);
        g.paragraph(detail, Rect{px, y, pw, 40}, TextStyle{13}, kMute);
        y += 52;
        const float dw = button_width(L"Done", 0, false);
        push_button(px + pw - dw, y, L"Done", 0, ButtonStyle::Primary, false, Action::Clip, "ex-close");
        if (ok) {
            const float fw = button_width(L"Show in folder", glyph::kFolder, false);
            push_button(px + pw - dw - 8 - fw, y, L"Show in folder", glyph::kFolder, ButtonStyle::Line, false,
                        Action::Clip, "ex-folder");
        }
    }
}

}  // namespace bt::ui
