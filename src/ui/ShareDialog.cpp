// The Share window of the clip page: Backtrack makes a share copy of the clip (or of the trimmed
// part) with the export engine, uploads it to catbox.moe or Litterbox (platform/Uploader) and
// copies the direct MP4 link, which Discord plays in the chat. The link is kept in the library:
// the clip shows up under Shared, and "Copy link" works later without uploading again.
//
// The share copy instead of the clip itself: a clip can be bigger than the host takes (200 MB),
// and other programs' clips may have their sound on several tracks; the copy is one track,
// H.264 at a size that loads quickly in a chat.

#include "platform/Exporter.h"
#include "platform/Log.h"
#include "platform/Shell.h"
#include "platform/Text.h"
#include "platform/Uploader.h"
#include "ui/App.h"
#include "ui/AppIds.h"
#include "ui/Theme.h"

#include <bcrypt.h>

#include <algorithm>
#include <format>

namespace bt::ui {

using namespace theme;
using namespace ids;

namespace {

namespace fs = std::filesystem;

// Where the share copy waits for its upload; deleted right after.
fs::path share_copy_path() {
    wchar_t temp[MAX_PATH + 1] = {};
    const DWORD length = GetTempPathW(MAX_PATH + 1, temp);
    const fs::path folder = (length > 0 && length <= MAX_PATH ? fs::path(temp) : fs::temp_directory_path()) / L"Backtrack";
    std::error_code ec;
    fs::create_directories(folder, ec);
    unsigned char bytes[8] = {};
    BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::wstring name = L"share-";
    for (const unsigned char b : bytes) name += std::format(L"{:02x}", b);
    return folder / (name + L".mp4");
}

// Balanced at 1080p when that fits the host; else the exporter's "fits a size" mode does.
ExportOptions share_options(const ExportOptions& range, const ExportSource& source, ShareHost host, bool hardware) {
    ExportOptions o = range;
    o.format = ExportFormat::Mp4;
    o.height = 1080;
    o.quality = ExportQuality::Balanced;
    o.audio = true;
    o.hardware = hardware;
    const ExportPlan plan = plan_export(o, source);
    if (!plan.problem.empty() || plan.estimated_bytes > share_size_limit(host)) {
        o.quality = ExportQuality::FitsDiscord;
        o.byte_budget = share_size_limit(host);
    }
    return o;
}

}  // namespace

bool App::copy_text(const std::wstring& text) {
    if (!OpenClipboard(window_)) return false;
    bool copied = false;
    EmptyClipboard();
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* data = GlobalLock(memory)) {
            memcpy(data, text.c_str(), bytes);
            GlobalUnlock(memory);
            copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
            if (!copied) GlobalFree(memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
    return copied;
}

void App::open_share() {
    if (!clip_view_ || share_ || export_) return;
    const ClipEntry* clip = library_.find(clip_view_->file);
    if (clip == nullptr || !clips_folder_available()) return;
    if (clip_view_->player) clip_view_->player->pause();
    auto view = std::make_shared<ShareView>();
    view->file = clip_view_->file;
    view->title = display_title(*clip);
    view->host = settings_.share_host;
    const double length = clip_view_->player && clip_view_->player->duration() > 0 ? clip_view_->player->duration()
                                                                                    : clip->duration_s;
    view->range.in_seconds = clip_view_->in;
    view->range.out_seconds = clip_view_->out > 0 && clip_view_->out < length - 0.05 ? clip_view_->out : 0.0;
    share_ = view;
    const fs::path source = clip_path(view->file);
    run_in_background([this, view, source]() -> std::function<void()> {
        std::optional<ExportSource> info = read_export_source(source);
        return [this, view, info] {
            if (share_ != view) return;
            view->source = info;
            if (!info) {
                view->stage = ShareView::Stage::Failed;
                view->message = "This clip cannot be read. It may be damaged or still being written.";
            }
            invalidate();
        };
    });
    invalidate();
}

void App::close_share() {
    if (!share_) return;
    if (share_->stage == ShareView::Stage::Working) {
        share_->cancel->store(true);
        show_notice(share_->finished->load() ? L"The clip was shared; its link is under Shared." : L"Sharing was cancelled.");
    }
    share_.reset();
    invalidate();
}

void App::run_share(const std::string& what, const Rect&) {
    if (!share_) return;
    const std::shared_ptr<ShareView> view = share_;
    const ClipEntry* clip = library_.find(view->file);
    if (what == "sh-close" || what == "sh-cancel") {
        close_share();
    } else if (what == "sh-copy" || what == "sh-copy-old") {
        const std::string url = what == "sh-copy" ? view->url : (clip != nullptr ? clip->share_url : std::string());
        if (!url.empty() && copy_text(to_wide(url))) show_notice(L"Link copied. Paste it in Discord.");
    } else if (what == "sh-open") {
        if (!view->url.empty()) shell::open(fs::path(to_wide(view->url)));
    } else if (view->stage == ShareView::Stage::Form && what.starts_with("sh-host:")) {
        view->host = what.back() == '1' ? ShareHost::Litterbox : ShareHost::Catbox;
        if (view->host != settings_.share_host) {
            const ShareHost host = view->host;
            change_settings([host](Settings& s) { s.share_host = host; });  // remembered for next time
        }
    } else if (view->stage == ShareView::Stage::Form && what == "sh-go" && view->source) {
        const ExportOptions options =
            share_options(view->range, *view->source, view->host, settings_.encoder != EncoderPreference::Software);
        if (!plan_export(options, *view->source).problem.empty()) return;
        view->stage = ShareView::Stage::Working;
        const fs::path source = clip_path(view->file);
        const ShareHost host = view->host;
        const HWND window = window_;
        log::info("Sharing {} on {}", view->file, to_utf8(std::wstring(share_endpoint(host).host)));
        run_in_background([this, view, source, options, host, window]() -> std::function<void()> {
            auto last = std::chrono::steady_clock::now();
            const auto report = [&](double fraction, bool uploading) {
                view->uploading->store(uploading);
                view->progress->store(fraction);
                const auto now = std::chrono::steady_clock::now();
                if (now - last > std::chrono::milliseconds(100)) {
                    last = now;
                    PostMessageW(window, kMsgExport, 0, 0);
                }
            };
            const fs::path copy = share_copy_path();
            std::string url;
            std::string message;
            bool cancelled = false;
            const ExportResult made =
                export_clip(source, copy, options, [&](double f) { report(f, false); }, *view->cancel);
            if (!made.ok) {
                cancelled = made.cancelled;
                message = made.message;
            } else {
                const UploadResult sent = upload_clip(copy, host, [&](double f) { report(f, true); }, *view->cancel);
                cancelled = sent.cancelled;
                message = sent.message;
                url = sent.url;
                if (sent.ok) view->finished->store(true);
            }
            std::error_code ec;
            fs::remove(copy, ec);  // the clip itself stays where it is; the copy is only for the upload
            return [this, view, url, message, cancelled, host] {
                if (url.empty()) {
                    if (!cancelled) log::warn("Sharing failed: {}", message);
                } else {
                    log::info("Shared {} as {}", view->file, url);
                    // Kept also when the window was closed meanwhile: the link exists now.
                    const std::int64_t now = now_unix();
                    const std::int64_t keeps = share_endpoint(host).keeps_seconds;
                    if (library_.set_shared(view->file, url, now, keeps > 0 ? now + keeps : 0)) save_library();
                    if (settings_.share_copy_link && copy_text(to_wide(url)) && share_ != view) {
                        show_notice(L"Link copied. Paste it in Discord.");
                    }
                }
                if (share_ != view) return;
                view->stage = url.empty() ? ShareView::Stage::Failed : ShareView::Stage::Done;
                view->url = url;
                view->message = message;
                view->copied = !url.empty() && settings_.share_copy_link;
                invalidate();
            };
        });
    }
    invalidate();
}

void App::paint_share(float width, float height) {
    if (!share_) return;
    Graphics& g = *graphics_;
    ShareView& view = *share_;
    const ClipEntry* clip = library_.find(view.file);
    g.fill(Rect{0, kTitleBar, width, height - kTitleBar}, Color::hex(0x000000, kLightTheme ? 0.3f : 0.5f));
    add_hit(Rect{0, kTitleBar, width, height - kTitleBar}, Action::Disabled);
    const bool already = clip != nullptr && share_link_live(*clip, now_unix()) && view.stage == ShareView::Stage::Form;
    const float w = std::min(500.0f, width - 32);
    const float x = (width - w) / 2;
    const float panel_h = view.stage == ShareView::Stage::Form ? (already ? 380.0f : 330.0f) : 230.0f;
    const Rect panel{x, std::max(kTitleBar + 16, (height - panel_h) / 2), w, panel_h};
    g.fill_round(Rect{panel.x + 2, panel.y + 10, panel.w - 4, panel.h}, 18, Color::hex(0x000000, 0.3f));
    g.fill_round(panel, kPanelRadius, kCard);
    g.stroke_round(panel, kPanelRadius, kLine);
    add_hit(panel, Action::Disabled);
    const float px = panel.x + 24;
    const float pw = panel.w - 48;
    float y = panel.y + 22;
    g.text(L"Share clip", Rect{px, y, pw, 26}, TextStyle{18, Weight::Bold}, kInk);
    y += 28;
    const double length = view.source ? view.source->duration_s : 0.0;
    const double end = view.range.out_seconds > 0 ? view.range.out_seconds : length;
    std::wstring sub = to_wide(view.title);
    if (end > view.range.in_seconds) sub += L" · " + to_wide(format_duration(end - view.range.in_seconds));
    g.text(sub, Rect{px, y, pw, 20}, TextStyle{13}, kMute);
    y += 34;

    const auto link_box = [&](const std::string& url, const char* copy_arg) {
        const Rect box{px, y, pw, 40};
        g.fill_round(box, kPageRadius, kBg);
        g.stroke_round(box, kPageRadius, kLine);
        const float cw = button_width(L"Copy link", glyph::kCopy, true);
        g.text(to_wide(url), Rect{box.x + 12, box.y, box.w - cw - 30, box.h}, TextStyle{12.5f}, kInk);
        push_button(box.right() - cw - 5, box.y + 5, L"Copy link", glyph::kCopy, ButtonStyle::Line, true, Action::Clip,
                    copy_arg);
        y += 52;
    };

    if (view.stage == ShareView::Stage::Form) {
        if (already) {
            g.text(clip->share_expires > 0 ? L"Shared before (Litterbox, deleted after 3 days)" : L"Shared before",
                   Rect{px, y, pw, 20}, TextStyle{13, Weight::Semibold}, kMute);
            y += 26;
            link_box(clip->share_url, "sh-copy-old");
        }
        g.text(L"Upload to", Rect{px, y, pw * 0.4f, 38}, TextStyle{14, Weight::Semibold}, kInk);
        segmented(px + pw, y + 19, {L"catbox.moe", L"Litterbox (3 days)"}, view.host == ShareHost::Litterbox ? 1 : 0,
                  Action::Clip, "sh-host");
        y += 50;
        const Rect info{px, y, pw, 96};
        g.fill_round(info, kPageRadius, kBg);
        std::wstring size = L"...";
        std::wstring problem;
        if (view.source) {
            const ExportOptions options = share_options(view.range, *view.source, view.host, true);
            const ExportPlan plan = plan_export(options, *view.source);
            if (!plan.problem.empty()) problem = to_wide(plan.problem);
            else size = std::format(L"about {:.0f} MB, {}×{}", static_cast<double>(plan.estimated_bytes) / 1e6,
                                    plan.width, plan.height);
        }
        g.text(L"Upload size", Rect{info.x + 14, info.y + 10, info.w / 2, 22}, TextStyle{13}, kMute);
        g.text(size, Rect{info.x + info.w / 2, info.y + 10, info.w / 2 - 14, 22}, TextStyle{14, Weight::Bold, false, true},
               kInk, Align::End);
        const std::wstring note =
            !problem.empty() ? problem
            : view.host == ShareHost::Catbox
                ? L"Discord plays the link right in the chat. Anyone with the link can watch it, and catbox.moe keeps "
                  L"it: it cannot be deleted again."
                : L"Discord plays the link right in the chat. Anyone with the link can watch it until Litterbox "
                  L"deletes it after 3 days.";
        g.paragraph(note, Rect{info.x + 14, info.y + 38, info.w - 28, 54}, TextStyle{12.5f}, problem.empty() ? kMute : kWarn);
        y = info.bottom() + 20;
        const float go_w = button_width(L"Upload and get link", glyph::kShare, false);
        push_button(px + pw - go_w, y, L"Upload and get link", glyph::kShare, ButtonStyle::Primary, false, Action::Clip,
                    "sh-go", !view.source || !problem.empty());
        const float cancel_w = button_width(L"Cancel", 0, false);
        push_button(px + pw - go_w - 8 - cancel_w, y, L"Cancel", 0, ButtonStyle::Line, false, Action::Clip, "sh-close");
    } else if (view.stage == ShareView::Stage::Working) {
        const bool uploading = view.uploading->load();
        // The copy is the first half of the bar, the upload the second.
        const double fraction = std::clamp(view.progress->load(), 0.0, 1.0) * 0.5 + (uploading ? 0.5 : 0.0);
        g.text(uploading ? std::format(L"Uploading to {}...", share_endpoint(view.host).host)
                         : std::wstring(L"Making the share copy..."),
               Rect{px, y, pw * 0.7f, 22}, TextStyle{14, Weight::Semibold}, kInk);
        g.text(std::format(L"{}%", std::lround(fraction * 100)), Rect{px + pw * 0.7f, y, pw * 0.3f, 22},
               TextStyle{14, Weight::Bold, false, true}, kInk, Align::End);
        y += 32;
        g.fill_round(Rect{px, y, pw, 8}, 4, kLine);
        g.fill_round(Rect{px, y, pw * static_cast<float>(fraction), 8}, 4, kAccent);
        y += 40;
        const float cw = button_width(L"Cancel", 0, false);
        push_button(px + pw - cw, y, L"Cancel", 0, ButtonStyle::Line, false, Action::Clip, "sh-cancel");
    } else if (view.stage == ShareView::Stage::Done) {
        g.text(view.copied ? L"Shared. The link is copied: paste it in Discord." : L"Shared. Copy the link below.",
               Rect{px, y, pw, 22}, TextStyle{14, Weight::Semibold}, kOk);
        y += 32;
        link_box(view.url, "sh-copy");
        y += 4;
        const float dw = button_width(L"Done", 0, false);
        push_button(px + pw - dw, y, L"Done", 0, ButtonStyle::Primary, false, Action::Clip, "sh-close");
        const float ow = button_width(L"Open in browser", 0, false);
        push_button(px + pw - dw - 8 - ow, y, L"Open in browser", 0, ButtonStyle::Line, false, Action::Clip, "sh-open");
    } else {
        g.text(L"Sharing did not work", Rect{px, y, pw, 22}, TextStyle{14, Weight::Semibold}, kDanger);
        y += 28;
        g.paragraph(to_wide(view.message), Rect{px, y, pw, 44}, TextStyle{13}, kMute);
        y += 56;
        const float dw = button_width(L"Close", 0, false);
        push_button(px + pw - dw, y, L"Close", 0, ButtonStyle::Primary, false, Action::Clip, "sh-close");
    }
}

}  // namespace bt::ui
