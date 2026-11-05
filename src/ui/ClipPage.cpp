// The clip page (backtrack.html #/clip): the player with its controls over the picture, the trim
// bar with a filmstrip, and the side panels (title, favorite, collections, tags, notes, details).
//
// Trimming never re-encodes: "Save as new clip" copies the compressed frames from the keyframe at
// or before the start handle (one per second in Backtrack's clips) up to the end handle into a new
// file next to the original (platform/Trim). The original is never changed.

#include "platform/Filmstrip.h"
#include "platform/Log.h"
#include "platform/Shell.h"
#include "platform/Text.h"
#include "platform/Trim.h"
#include "ui/App.h"
#include "ui/AppIds.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace bt::ui {
namespace {

namespace fs = std::filesystem;
using namespace theme;
using namespace ids;

constexpr double kSpeeds[] = {0.5, 1.0, 1.5, 2.0};
constexpr int kSpeedCount = static_cast<int>(std::size(kSpeeds));
constexpr double kMinTrim = 1.0;  // seconds; the handles never cross closer than this
constexpr int kStripFrames = 12;
constexpr int kStripWidth = 160;
constexpr int kStripHeight = 90;
constexpr float kSideWidth = 340;  // .vw grid-template-columns
constexpr float kColumnGap = 24;
constexpr float kPlayerControls = 94;  // .pc: 26 + 18 track + 6 + 34 row + 10

// "1:05" (player) and "1:05.3" (trim times).
std::wstring clock_text(double seconds, bool tenths) {
    if (!std::isfinite(seconds) || seconds < 0) seconds = 0;
    if (tenths) {
        const auto t = static_cast<long long>(std::floor(seconds * 10 + 0.5));
        return std::format(L"{}:{:02}.{}", t / 600, (t / 10) % 60, t % 10);
    }
    const auto t = static_cast<long long>(std::floor(seconds));
    if (t >= 3600) return std::format(L"{}:{:02}:{:02}", t / 3600, (t / 60) % 60, t % 60);
    return std::format(L"{}:{:02}", t / 60, t % 60);
}

std::wstring strip_key(const std::string& file, int index) {
    return L"strip|" + to_wide(file) + L"|" + std::to_wstring(index);
}

// "Mon 2 Nov, 12:25 PM" in the user's own date and time format.
std::wstring captured_text(std::int64_t unix_seconds) {
    if (unix_seconds <= 0) return L"Unknown";
    const std::int64_t ticks = (unix_seconds + 11'644'473'600LL) * 10'000'000LL;
    FILETIME utc{static_cast<DWORD>(ticks & 0xFFFFFFFF), static_cast<DWORD>(ticks >> 32)};
    FILETIME local{};
    SYSTEMTIME t{};
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &t)) return L"Unknown";
    wchar_t date[64] = {};
    wchar_t time[64] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &t, L"ddd d MMM yyyy", date, 64, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &t, nullptr, time, 64);
    return std::format(L"{}, {}", date, time);
}

std::string notes_from_box(std::wstring text) {
    std::wstring out;
    out.reserve(text.size());
    for (const wchar_t c : text) {
        if (c != L'\r') out.push_back(c);  // the edit control uses CRLF; the library keeps LF
    }
    return to_utf8(out);
}

std::wstring notes_for_box(const std::string& notes) {
    std::wstring out;
    for (const wchar_t c : to_wide(notes)) {
        if (c == L'\n') out += L"\r\n";
        else if (c != L'\r') out.push_back(c);
    }
    return out;
}

std::wstring box_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(std::max(length, 0)) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(std::max(length, 0)));
    return text;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Opening and closing

std::vector<std::string> App::clip_order() const {
    std::vector<std::string> files;
    for (const std::size_t i : library_.query(current_query())) files.push_back(library_.clips()[i].file);
    return files;
}

void App::open_clip(const std::string& file) {
    const ClipEntry* clip = library_.find(file);
    if (clip == nullptr || !clips_folder_available()) return;
    new_files_.erase(file);
    if (clip_view_) close_clip();
    set_page(Page::Clip);
    clip_view_.emplace();
    clip_view_->file = file;
    clip_view_->out = clip->duration_s;
    SetWindowTextW(notes_box_, notes_for_box(clip->notes).c_str());
    SetWindowTextW(tag_box_, L"");
    make_player();
    SetTimer(window_, kPlayerTimer, kPlayerMs, nullptr);
    request_filmstrip();
    invalidate();
}

void App::close_clip() {
    if (!clip_view_) return;
    commit_clip_text();
    if (window_ != nullptr) KillTimer(window_, kPlayerTimer);
    for (int i = 0; i < clip_view_->strip_frames; ++i) graphics_->forget_bitmap(strip_key(clip_view_->file, i));
    clip_view_.reset();  // closes the file: it can be moved to the Recycle Bin now
    if (editing_ && editing_->kind == Editing::Kind::ClipTitle) end_edit(true);
}

void App::commit_clip_text() {
    if (!clip_view_ || notes_box_ == nullptr) return;
    const ClipEntry* clip = library_.find(clip_view_->file);
    if (clip == nullptr) return;
    const std::string notes = notes_from_box(box_text(notes_box_));
    if (notes != clip->notes) {
        library_.set_notes(clip_view_->file, notes);
        save_library();
    }
}

void App::make_player() {
    ClipView& view = *clip_view_;
    view.player.reset();
    view.error.clear();
    if (!graphics_->ensure_device()) {
        view.error = "The clip cannot be shown: no graphics device.";
        return;
    }
    try {
        view.player_tag = next_player_tag_++;
        view.player = std::make_unique<VideoPlayer>(graphics_->device(), window_, kMsgPlayer, view.player_tag,
                                                    clip_path(view.file));
        view.generation = graphics_->generation();
        view.player->set_volume(view.volume);
        view.player->set_muted(view.muted);
        view.player->set_rate(kSpeeds[view.speed]);
    } catch (const std::exception& e) {
        log::warn("Could not open the clip player: {}", e.what());
        view.error = "This clip could not be opened for playing.";
    }
}

void App::tick_player() {
    if (!clip_view_ || page_ != Page::Clip) {
        KillTimer(window_, kPlayerTimer);
        return;
    }
    ClipView& view = *clip_view_;
    if (view.player && graphics_->device() != nullptr && view.generation != graphics_->generation()) {
        // The graphics device was replaced (driver update): the player decoded on the old one,
        // and the filmstrip pictures lived on it.
        const double at = view.player->position();
        make_player();
        if (view.player) view.player->seek(at);
        view.strip_frames = 0;
        view.strip_requested = false;
        request_filmstrip();
    }
    if (!view.player) return;
    VideoPlayer& player = *view.player;
    bool redraw = player.update_frame();
    if (player.playing()) {
        // Playback stays inside the trim: it stops at the end handle.
        if (view.out > 0 && player.position() >= view.out - 0.01) {
            player.pause();
            player.seek(view.out);
        }
        redraw = true;  // the time and the seek bar move
    }
    if (redraw) invalidate();
}

void App::request_filmstrip() {
    if (!clip_view_ || clip_view_->strip_requested) return;
    clip_view_->strip_requested = true;
    const std::string file = clip_view_->file;
    const fs::path path = clip_path(file);
    run_in_background([this, file, path]() -> std::function<void()> {
        auto frames = std::make_shared<std::vector<std::vector<std::uint8_t>>>(
            extract_frames(path, kStripFrames, kStripWidth, kStripHeight));
        return [this, file, frames] {
            if (!clip_view_ || clip_view_->file != file || frames->empty()) return;
            int uploaded = 0;
            for (const auto& frame : *frames) {
                if (frame.size() != static_cast<std::size_t>(kStripWidth) * kStripHeight * 4) break;
                graphics_->upload_bitmap(strip_key(file, uploaded), frame.data(), kStripWidth, kStripHeight);
                ++uploaded;
            }
            clip_view_->strip_frames = uploaded;
            invalidate();
        };
    });
}

// ---------------------------------------------------------------------------------------------
// Actions

void App::run_clip(const std::string& what, const Rect& anchor) {
    if (!clip_view_) {
        if (what == "back") set_page(Page::Library);
        return;
    }
    ClipView& view = *clip_view_;
    const std::string file = view.file;
    const ClipEntry* clip = library_.find(file);
    if (clip == nullptr) {
        set_page(Page::Library);  // deleted meanwhile (outside Backtrack)
        return;
    }
    VideoPlayer* player = view.player.get();
    const double length = player != nullptr && player->duration() > 0 ? player->duration() : clip->duration_s;
    const double out = view.out > 0 ? view.out : length;

    if (what == "back") {
        set_page(Page::Library);
    } else if (what == "prev" || what == "next") {
        const std::vector<std::string> order = clip_order();
        const auto it = std::find(order.begin(), order.end(), file);
        if (it == order.end()) return;
        if (what == "prev" && it != order.begin()) open_clip(*(it - 1));
        if (what == "next" && it + 1 != order.end()) open_clip(*(it + 1));
    } else if (what == "play") {
        if (player == nullptr || !player->ready()) return;
        if (player->playing()) {
            player->pause();
        } else {
            const double at = player->position();
            if (at < view.in - 0.05 || at >= out - 0.05) player->seek(view.in);  // from the start of the trim
            player->play();
        }
    } else if (what == "back5" || what == "fwd5") {
        if (player != nullptr) {
            player->seek(std::clamp(player->position() + (what == "back5" ? -5.0 : 5.0), view.in, out));
        }
    } else if (what == "mute") {
        view.muted = !view.muted;
        if (!view.muted && view.volume <= 0.01) view.volume = 0.5;
        if (player != nullptr) {
            player->set_muted(view.muted);
            player->set_volume(view.volume);
        }
    } else if (what == "speed") {
        view.speed = (view.speed + 1) % kSpeedCount;
        if (player != nullptr) player->set_rate(kSpeeds[view.speed]);
    } else if (what == "wide") {
        view.wide = !view.wide;
        page_scroll_ = 0;
    } else if (what == "reset-trim") {
        view.in = 0;
        view.out = length;
        view.trim_touched = false;
    } else if (what == "set-in") {
        if (player != nullptr && length > 0) {
            view.in = std::clamp(player->position(), 0.0, std::max(0.0, out - kMinTrim));
            view.trim_touched = true;
        }
    } else if (what == "set-out") {
        if (player != nullptr && length > 0) {
            view.out = std::clamp(player->position(), std::min(length, view.in + kMinTrim), length);
            view.trim_touched = true;
        }
    } else if (what == "save-trim") {
        save_trim();
    } else if (what == "share") {
        // Sharing links come later; until then the file, ready to drag into Discord or anywhere.
        shell::show_in_folder(clip_path(file));
    } else if (what == "delete") {
        const std::vector<std::string> order = clip_order();
        const auto it = std::find(order.begin(), order.end(), file);
        std::string next;
        if (it != order.end() && it + 1 != order.end()) next = *(it + 1);
        else if (it != order.end() && it != order.begin()) next = *(it - 1);
        close_clip();
        move_to_trash(file);
        if (!next.empty()) open_clip(next);
        else set_page(Page::Library);
    } else if (what == "delete-forever") {
        set_page(Page::Library);
        recycle_now(file);
    } else if (what == "restore") {
        library_.restore(file);
        save_library();
    } else if (what == "fav") {
        library_.set_favorite(file, !clip->favorite);
        save_library();
    } else if (what == "add-collection") {
        add_to_collection_menu(file, anchor);
    } else if (what == "rename") {
        if (!clip->trashed_at) begin_edit(Editing{Editing::Kind::ClipTitle, file}, to_wide(display_title(*clip)));
    } else if (what.starts_with("tag-del:")) {
        const std::size_t index = static_cast<std::size_t>(std::stoul(what.substr(8)));
        if (index < clip->tags.size()) {
            library_.remove_tag(file, std::string(clip->tags[index]));
            save_library();
        }
    } else if (what == "add-tag") {
        const std::string tag = to_utf8(box_text(tag_box_));
        SetWindowTextW(tag_box_, L"");
        if (!tag.empty() && library_.add_tag(file, tag)) save_library();
    } else if (what.starts_with("col:")) {
        set_view(LibraryView::Collection, what.substr(4));
    } else if (what == "retry") {
        make_player();
    }
    invalidate();
}

void App::drag_clip(const std::string& id, float fraction, bool done) {
    if (!clip_view_) return;
    ClipView& view = *clip_view_;
    VideoPlayer* player = view.player.get();
    const ClipEntry* clip = library_.find(view.file);
    if (player == nullptr || clip == nullptr) return;
    const double length = player->duration() > 0 ? player->duration() : clip->duration_s;
    if (length <= 0) return;
    if (view.out <= 0) view.out = length;
    const double at = static_cast<double>(fraction) * length;
    if (fraction >= 0 && id == "clip:vol") {
        // The volume never pauses the picture.
        view.volume = fraction;
        view.muted = fraction <= 0.0f;
        player->set_volume(view.volume);
        player->set_muted(view.muted);
    } else if (fraction >= 0) {
        if (!view.scrubbing) {
            // Paused while dragging so the picture follows the mouse, playing again after.
            view.scrubbing = true;
            view.resume_after_scrub = player->playing() && (id == "clip:seek" || id == "clip:strip");
            if (player->playing()) player->pause();
        }
        if (id == "clip:seek" || id == "clip:strip") {
            player->seek(std::clamp(at, view.in, view.out));
        } else if (id == "clip:in") {
            view.in = std::clamp(at, 0.0, std::max(0.0, view.out - kMinTrim));
            view.trim_touched = true;
            player->seek(view.in);
        } else if (id == "clip:out") {
            view.out = std::clamp(at, std::min(length, view.in + kMinTrim), length);
            view.trim_touched = true;
            player->seek(view.out);
        }
    }
    if (done) {
        if (view.scrubbing && view.resume_after_scrub) player->play();
        view.scrubbing = false;
        view.resume_after_scrub = false;
    }
    invalidate();
}

void App::save_trim() {
    if (!clip_view_ || trimming_) return;
    const ClipView& view = *clip_view_;
    const ClipEntry* clip = library_.find(view.file);
    if (clip == nullptr || !clips_folder_available()) return;
    const double length = view.player && view.player->duration() > 0 ? view.player->duration() : clip->duration_s;
    const double out = view.out > 0 ? view.out : length;
    const fs::path source = clip_path(view.file);
    // Next to the original: "<name>_trim.mp4", "<name>_trim2.mp4", ...
    const std::wstring stem = source.stem().wstring();
    fs::path destination;
    for (int n = 1; n < 1000; ++n) {
        destination = source.parent_path() / (stem + (n == 1 ? L"_trim" : L"_trim" + std::to_wstring(n)) + L".mp4");
        std::error_code ec;
        fs::path temp = destination;
        temp += L".tmp";
        if (!fs::exists(destination, ec) && !fs::exists(temp, ec)) break;
    }
    const std::string title = display_title(*clip) + " (trimmed)";
    const std::vector<std::string> tags = clip->tags;
    const double in = view.in;
    trimming_ = true;
    log::info("Trimming {} from {:.1f} s to {:.1f} s", view.file, in, out);
    run_in_background([this, source, destination, in, out, title, tags]() -> std::function<void()> {
        TrimResult result;
        try {
            result = trim_clip(TrimRequest{source, destination, in, out});
        } catch (const std::exception& e) {
            result.ok = false;
            result.error = e.what();
        } catch (...) {
            result.ok = false;
            result.error = "an unexpected error";
        }
        return [this, result, destination, title, tags] {
            trimming_ = false;
            if (!result.ok) {
                log::warn("Trimming failed: {}", result.error);
                show_notice(L"Could not save the trimmed clip: " + to_wide(result.error));
                return;
            }
            log::info("Saved the trimmed clip {} ({:.1f} s)", to_utf8(destination.filename().wstring()), result.seconds);
            scan_folder();
            const std::string file = to_utf8(destination.filename().wstring());
            if (library_.find(file) != nullptr) {
                library_.set_title(file, title);
                for (const std::string& tag : tags) library_.add_tag(file, tag);
                new_files_.insert(file);
                save_library();
            }
            SavedBar bar;
            bar.headline = "Trimmed clip saved";
            bar.file = library_.find(file) != nullptr ? file : std::string{};
            bar.path = destination;
            bar.title = title;
            bar.detail = std::format("{}, {}", format_duration(result.seconds),
                                     format_size(static_cast<std::int64_t>(result.bytes)));
            bar.at = std::chrono::steady_clock::now();
            saved_bar_ = bar;  // shown on the library, where the new clip is
            invalidate();
        };
    });
    invalidate();
}

// ---------------------------------------------------------------------------------------------
// Painting

void App::paint_clip_page(const Rect& area) {
    Graphics& g = *graphics_;
    const ClipEntry* clip = clip_view_ ? library_.find(clip_view_->file) : nullptr;
    if (clip == nullptr) {
        // .empty: the clip went away (deleted outside Backtrack).
        g.text(L"That clip isn't here", Rect{area.x, area.y + 120, area.w, 26}, TextStyle{18, Weight::Bold, true}, kInk,
               Align::Center);
        g.text(L"It may have been deleted or moved.", Rect{area.x, area.y + 150, area.w, 22}, TextStyle{13.5f}, kMute,
               Align::Center);
        const float bw = button_width(L"Back to all clips", 0, false);
        push_button(area.x + (area.w - bw) / 2, area.y + 186, L"Back to all clips", 0, ButtonStyle::Primary, false,
                    Action::Clip, "back");
        return;
    }
    const ClipView& view = *clip_view_;
    const Rect page = begin_page(area);
    const float x0 = page.x + kPad;
    const float cw = page.w - 2 * kPad;
    float y = page.y + kPageTop;

    // .crumb: Library, "Clip 12 of 28", newer / older.
    float bx = x0 + push_button(x0, y + 2, L"Library", glyph::kChevronLeft, ButtonStyle::Line, true, Action::Clip, "back");
    if (clip->trashed_at) {
        const Rect pill{bx + 10, y + 4, 78, 26};
        g.fill_round(pill, 13, kInk.with_alpha(0.07f));
        g.text(L"In Trash", pill, TextStyle{12, Weight::Semibold}, kMute, Align::Center);
    }
    const std::vector<std::string> order = clip_order();
    const auto it = std::find(order.begin(), order.end(), view.file);
    const bool listed = it != order.end();
    const Rect next_rect{x0 + cw - 34, y, 34, 34};
    const Rect prev_rect{next_rect.x - 10 - 34, y, 34, 34};
    icon_button(prev_rect, glyph::kChevronLeft, 13, Action::Clip, "prev", !listed || it == order.begin());
    icon_button(next_rect, glyph::kChevronRight, 13, Action::Clip, "next", !listed || it + 1 == order.end());
    if (listed) {
        g.text(std::format(L"Clip {} of {}", it - order.begin() + 1, order.size()), Rect{prev_rect.x - 210, y, 200, 34},
               TextStyle{13}, kMute, Align::End);
    }
    y += 34 + 16;

    // .vw: player and trim on the left, the side panels in a 340 px column (below when narrow or wide view).
    // The player's control row and the trim buttons need about 560 next to the side column.
    const bool one_column = view.wide || cw - kSideWidth - kColumnGap < 560;
    const float left_w = one_column ? cw : cw - kSideWidth - kColumnGap;
    const Rect player{x0, y, left_w, std::floor(left_w * 9.0f / 16.0f)};
    paint_player(*clip, player);
    const Rect trim{x0, player.bottom() + 16, left_w, 222};
    paint_trim(*clip, trim);
    float bottom = trim.bottom();
    if (one_column) {
        bottom = paint_clip_side(*clip, Rect{x0, bottom + 16, cw, 0});
    } else {
        bottom = std::max(bottom, paint_clip_side(*clip, Rect{x0 + left_w + kColumnGap, y, kSideWidth, 0}));
    }
    end_page(area, bottom + 28);
}

void App::paint_player(const ClipEntry& clip, const Rect& rect) {
    Graphics& g = *graphics_;
    const ClipView& view = *clip_view_;
    const VideoPlayer* player = view.player.get();
    const Color white = Color::hex(0xffffff);
    g.fill_round(Rect{rect.x + 2, rect.y + 10, rect.w - 4, rect.h}, 18, Color::hex(0x000000, 0.3f));  // --shadow
    g.fill_round(rect, kPanelRadius, Color::hex(0x000000));
    g.push_round_clip(rect, kPanelRadius);
    if (player == nullptr || player->texture() == nullptr || !g.draw_texture(player->texture(), rect)) {
        paint_thumbnail(clip, rect, 0);  // until the first frame arrives
    }
    const std::string error = !view.error.empty() ? view.error : player != nullptr ? player->error() : std::string();
    const bool playing = player != nullptr && player->playing();
    // Clicking the picture plays and pauses; the controls below take their own clicks first.
    add_hit(Rect{rect.x, rect.y, rect.w, rect.h - kPlayerControls}, Action::Clip, "play");
    if (!error.empty()) {
        g.fill(rect, Color::hex(0x000000, 0.6f));
        g.text(to_wide(error), Rect{rect.x, rect.y + rect.h / 2 - 30, rect.w, 24}, TextStyle{14, Weight::Semibold}, white,
               Align::Center);
        const float bw = button_width(L"Show in folder", glyph::kFolder, true);
        push_button(rect.x + (rect.w - bw) / 2, rect.y + rect.h / 2 + 6, L"Show in folder", glyph::kFolder,
                    ButtonStyle::Line, true, Action::Clip, "share");
    } else if (!playing) {
        // .big-play: 76 px, dark glass, white triangle.
        const float cx = rect.x + rect.w / 2;
        const float cy = rect.y + rect.h / 2;
        g.fill_circle(cx, cy, 38, Color::hex(0x0a1413, hovered(Action::Clip, "play") ? 0.7f : 0.55f));
        g.icon(glyph::kPlay, Rect{cx - 38 + 3, cy - 38, 76, 76}, 28, white);
    }

    // .pc: the controls on a dark gradient at the bottom.
    const Rect pc{rect.x, rect.bottom() - kPlayerControls, rect.w, kPlayerControls};
    g.fill_gradient(pc, 0, Color::hex(0x000000, 0.0f), Color::hex(0x000000, 0.7f));
    const double length = player != nullptr && player->duration() > 0 ? player->duration() : clip.duration_s;
    const double at = player != nullptr ? player->position() : 0.0;
    const float fraction = length > 0 ? static_cast<float>(std::clamp(at / length, 0.0, 1.0)) : 0.0f;
    const Rect track{pc.x + 14, pc.y + 26, pc.w - 28, 18};
    g.fill_round(Rect{track.x, track.y + 7, track.w, 4}, 2, white.with_alpha(0.35f));
    g.fill_round(Rect{track.x, track.y + 7, track.w * fraction, 4}, 2, kAccent);
    g.fill_circle(track.x + track.w * fraction, track.y + 9, 6, white);
    tracks_["clip:seek"] = track;
    add_hit(track, Action::Drag, "clip:seek");

    const float row = track.bottom() + 6;
    const Color hover = white.with_alpha(0.18f);
    float x = pc.x + 14;
    icon_button(Rect{x, row, 34, 34}, playing ? glyph::kPause : glyph::kPlay, 14, Action::Clip, "play", false, white,
                hover);
    x += 34 + 6;
    icon_button(Rect{x, row, 34, 34}, view.muted || view.volume <= 0.01 ? glyph::kMute : glyph::kVolume, 14, Action::Clip,
                "mute", false, white, hover);
    x += 34 + 6;
    slider(Rect{x, row + 5, 90, 24}, view.muted ? 0.0f : static_cast<float>(view.volume), "clip:vol", false, white);
    x += 90 + 12;
    // The length rounded like the library card shows it ("0:09" for 8.8 s), the position counting up.
    g.text(clock_text(at, false) + L" / " + clock_text(std::round(length), false), Rect{x, row, 140, 34},
           TextStyle{12.5f, Weight::Regular, false, true}, white);
    float rx = pc.right() - 14 - 34;
    icon_button(Rect{rx, row, 34, 34}, view.wide ? glyph::kCollapse : glyph::kExpand, 14, Action::Clip, "wide", false,
                white, hover);
    const std::wstring speed = std::format(L"{}x", kSpeeds[view.speed]);
    const float sw = g.text_width(speed, TextStyle{12.5f, Weight::Bold}) + 20;
    rx -= 6 + sw;
    const Rect speed_rect{rx, row + 3, sw, 28};
    if (hovered(Action::Clip, "speed")) g.fill_round(speed_rect, 7, hover);
    g.text(speed, speed_rect, TextStyle{12.5f, Weight::Bold}, white, Align::Center);
    add_hit(speed_rect, Action::Clip, "speed");
    g.pop_clip();
}

void App::paint_trim(const ClipEntry& clip, const Rect& rect) {
    Graphics& g = *graphics_;
    const ClipView& view = *clip_view_;
    const VideoPlayer* player = view.player.get();
    const double length = player != nullptr && player->duration() > 0 ? player->duration() : clip.duration_s;
    const double in = view.in;
    const double out = view.out > 0 ? view.out : length;
    g.fill_round(rect, kPanelRadius, kCard);
    const float x = rect.x + 18;
    const float w = rect.w - 36;
    float y = rect.y + 16;

    // .tl-h: Trim, a hint, then the three buttons on the right.
    const float trim_w = g.text(L"Trim", Rect{x, y, 60, 30}, TextStyle{14, Weight::Bold}, kInk);
    const bool can_trim = player != nullptr && player->ready() && player->error().empty() && length > 0;
    const bool trimmed = length > 0 && (in > 0.05 || out < length - 0.05);
    float bx = x + w;
    bx -= button_width(L"End here", glyph::kCut, true);
    push_button(bx, y, L"End here", glyph::kCut, ButtonStyle::Line, true, Action::Clip, "set-out", !can_trim);
    bx -= 8 + button_width(L"Start here", glyph::kCut, true);
    push_button(bx, y, L"Start here", glyph::kCut, ButtonStyle::Line, true, Action::Clip, "set-in", !can_trim);
    bx -= 8 + button_width(L"Reset trim", glyph::kRestoreDefaults, true);
    push_button(bx, y, L"Reset trim", glyph::kRestoreDefaults, ButtonStyle::Line, true, Action::Clip, "reset-trim",
                !trimmed);
    g.text(L"Drag the handles to trim", Rect{x + trim_w + 10, y, bx - (x + trim_w + 10) - 8, 30}, TextStyle{13}, kMute);
    y += 30 + 12;

    // .tl: filmstrip, shade outside the selection, the handles and the playhead.
    const Rect tl{x + 7, y, w - 14, 66};
    const auto pos = [&](double t) {
        return tl.x + static_cast<float>(length > 0 ? std::clamp(t / length, 0.0, 1.0) : 0.0) * tl.w;
    };
    g.push_round_clip(tl, 8);
    const int cells = view.strip_frames > 0 ? view.strip_frames : kStripFrames;
    for (int i = 0; i < cells; ++i) {
        const float cx = tl.x + tl.w * static_cast<float>(i) / static_cast<float>(cells);
        const Rect cell{cx, tl.y, tl.w / static_cast<float>(cells) + 0.5f, tl.h};
        if (i >= view.strip_frames || !g.draw_bitmap(strip_key(view.file, i), cell, 0)) {
            const Pair& pair = kPlaceholders[std::hash<std::string>{}(clip.file) % kPlaceholderCount];
            g.fill_gradient(cell, 0, pair.top, pair.bottom);
        }
        g.fill(Rect{cell.right() - 1, cell.y, 1, cell.h}, Color::hex(0x000000, 0.25f));
    }
    const float in_x = pos(in);
    const float out_x = pos(out);
    g.fill(Rect{tl.x, tl.y, in_x - tl.x, tl.h}, Color::hex(0x080e0d, 0.62f));
    g.fill(Rect{out_x, tl.y, tl.right() - out_x, tl.h}, Color::hex(0x080e0d, 0.62f));
    g.pop_clip();
    tracks_["clip:strip"] = tl;
    tracks_["clip:in"] = tl;
    tracks_["clip:out"] = tl;
    add_hit(tl, can_trim ? Action::Drag : Action::Disabled, "clip:strip");
    // .sel-box: 3 px accent lines along the top and bottom of the kept part.
    g.fill(Rect{in_x, tl.y - 2, out_x - in_x, 3}, kAccent);
    g.fill(Rect{in_x, tl.bottom() - 1, out_x - in_x, 3}, kAccent);
    for (const auto& [hx, id] : {std::pair{in_x, "clip:in"}, std::pair{out_x, "clip:out"}}) {
        const Rect handle{hx - 7, tl.y - 4, 14, tl.h + 8};
        const bool hot = hovered(Action::Drag, id) || dragging_ == id;
        g.fill_round(handle, 6, hot ? kAccent.mix(Color::hex(0xffffff), 0.88f) : kAccent);
        g.fill_round(Rect{hx - 1, handle.y + (handle.h - 20) / 2, 2, 20}, 1, kOnAccent.with_alpha(0.7f));
        add_hit(handle.inset(-4, 0), can_trim ? Action::Drag : Action::Disabled, id);
    }
    if (player != nullptr) {
        // .ph: the playhead.
        const float px = pos(player->position());
        g.fill_round(Rect{px - 1, tl.y - 8, 2, tl.h + 16}, 1, kInk);
        g.fill_circle(px, tl.y - 3, 5, kInk);
    }
    y = tl.bottom() + 12;

    // .tl-t: in, trimmed length, out.
    const TextStyle times{12.5f, Weight::Regular, false, true};
    g.text(L"In " + clock_text(in, true), Rect{x, y, w / 3, 18}, times, kMute);
    g.text(L"Trimmed length " + clock_text(std::max(0.0, out - in), true), Rect{x + w / 3, y, w / 3, 18}, times, kMute,
           Align::Center);
    g.text(L"Out " + clock_text(out, true), Rect{x + 2 * w / 3, y, w / 3, 18}, times, kMute, Align::End);
    y += 18 + 14;

    // .vtools.
    float tx = x;
    tx += 8 + push_button(tx, y, trimming_ ? L"Saving..." : L"Save as new clip", glyph::kCut, ButtonStyle::Primary, false,
                          Action::Clip, "save-trim", !can_trim || !trimmed || trimming_ || clip.trashed_at.has_value());
    if (!clip.trashed_at) {  // in the Trash, Restore and Delete forever need the room
        tx += push_button(tx, y, L"Export...", glyph::kDownload, ButtonStyle::Line, false, Action::Clip, "export", true);
        soon_badge(tx + 4, y + kPageControl / 2);
        tx += 4 + 38 + 8;
    }
    push_button(tx, y, L"Share...", glyph::kShare, ButtonStyle::Line, false, Action::Clip, "share");
    if (clip.trashed_at) {
        const float dw = button_width(L"Delete forever", glyph::kTrash, false);
        const float rw = button_width(L"Restore", glyph::kRestoreDefaults, false);
        push_button(x + w - dw, y, L"Delete forever", glyph::kTrash, ButtonStyle::Danger, false, Action::Clip,
                    "delete-forever");
        push_button(x + w - dw - 8 - rw, y, L"Restore", glyph::kRestoreDefaults, ButtonStyle::Line, false, Action::Clip,
                    "restore");
    } else {
        const float dw = button_width(L"Delete", glyph::kTrash, false);
        push_button(x + w - dw, y, L"Delete", glyph::kTrash, ButtonStyle::Danger, false, Action::Clip, "delete");
    }
}

float App::paint_clip_side(const ClipEntry& clip, const Rect& column) {
    Graphics& g = *graphics_;
    const float x = column.x;
    const float w = column.w;
    float y = column.y;
    const auto panel_title = [&](float px, float py, wchar_t glyph, const wchar_t* text) {
        // .vside h3: 12.5 px, muted, with an icon.
        g.icon(glyph, Rect{px, py, 15, 18}, 12, kMute);
        g.text(text, Rect{px + 23, py, 200, 18}, TextStyle{12.5f, Weight::Semibold}, kMute);
    };
    const auto chip = [&](float& cx, float& cy, float left, float right, const std::wstring& text, std::optional<Color> dot,
                          const std::string& remove_arg, const std::string& click_arg) {
        // .tag: 26 high, pill, soft background, accent text; an x to remove.
        const TextStyle style{12.5f, Weight::Semibold};
        const float text_w = std::min(g.text_width(text, style), right - left - 60);
        const float cw = (dot ? 14.0f : 0.0f) + text_w + 11 + (remove_arg.empty() ? 11.0f : 4.0f + 18.0f + 6.0f);
        if (cx + cw > right && cx > left) {
            cx = left;
            cy += 26 + 6;
        }
        const Rect r{cx, cy, cw, 26};
        g.fill_round(r, 13, kSoft);
        float tx = r.x + 11;
        if (dot) {
            g.fill_circle(tx + 4, r.y + 13, 4, *dot);
            tx += 14;
        }
        g.text(text, Rect{tx, r.y, text_w + 1, r.h}, style, kAccent);
        if (!click_arg.empty()) add_hit(r, Action::Clip, click_arg);
        if (!remove_arg.empty()) {
            const Rect x_rect{r.right() - 6 - 18, r.y + 4, 18, 18};
            if (hovered(Action::Clip, remove_arg)) g.fill_circle(x_rect.x + 9, x_rect.y + 9, 9, kAccent.with_alpha(0.25f));
            g.icon(glyph::kClose, x_rect, 8, kAccent);
            add_hit(x_rect, Action::Clip, remove_arg);
        }
        cx += cw + 6;
    };

    // Title, favorite, collections.
    {
        const float top = y;
        const float px = x + 18;
        const float pw = w - 36;
        // Measured first so the panel can be drawn behind it.
        const std::wstring title = to_wide(display_title(clip));
        const TextStyle title_style{20, Weight::Bold, true};
        const bool two_lines = g.text_width(title, title_style) > pw;
        const float title_h = two_lines ? 52.0f : 27.0f;
        const auto& all = library_.collections();
        std::vector<const Collection*> member;
        for (const Collection& c : all) {
            if (std::find(clip.collections.begin(), clip.collections.end(), c.id) != clip.collections.end()) {
                member.push_back(&c);
            }
        }
        // Collection chips wrap; count the rows by laying them out without drawing.
        float rows_h = 0;
        if (!member.empty()) {
            float cx = px;
            float rows = 1;
            for (const Collection* c : member) {
                const float cw = 14 + std::min(g.text_width(to_wide(c->name), TextStyle{12.5f, Weight::Semibold}), pw - 60) + 22;
                if (cx + cw > px + pw && cx > px) {
                    cx = px;
                    ++rows;
                }
                cx += cw + 6;
            }
            rows_h = 12 + rows * 26 + (rows - 1) * 6;
        }
        const float panel_h = 16 + title_h + 18 + 30 + rows_h + 16;
        g.fill_round(Rect{x, top, w, panel_h}, kPanelRadius, kCard);
        const Rect title_rect{px - 8, top + 16 - 6, pw + 16, title_h + 12};
        const bool editing_title = editing_ && editing_->kind == Editing::Kind::ClipTitle && editing_->target == clip.file;
        if (editing_title) {
            g.fill_round(title_rect, 9, kBg);
            g.stroke_round(title_rect, 9, kAccent);
            edit_rect_ = Rect{px, top + 16 + (title_h - 30) / 2, pw, 30};
        } else {
            if (!clip.trashed_at && hovered(Action::Clip, "rename")) g.stroke_round(title_rect, 9, kLine);
            if (two_lines) {
                g.paragraph(title, Rect{px, top + 16, pw, title_h}, title_style, kInk);
            } else {
                g.text(title, Rect{px, top + 16, pw, title_h}, title_style, kInk);
            }
            if (!clip.trashed_at) add_hit(title_rect, Action::Clip, "rename");
        }
        float ty = top + 16 + title_h + 18;
        float bx = px;
        bx += 8 + push_button(bx, ty, clip.favorite ? L"Favorited" : L"Favorite",
                              clip.favorite ? glyph::kFavoriteFill : glyph::kFavorite, ButtonStyle::Line, true,
                              Action::Clip, "fav", clip.trashed_at.has_value(),
                              clip.favorite ? std::optional<Color>(kAccent) : std::nullopt);
        push_button(bx, ty, L"Add to collection", glyph::kFolder, ButtonStyle::Line, true, Action::Clip,
                    "add-collection", clip.trashed_at.has_value());
        ty += 30;
        if (!member.empty()) {
            float cx = px;
            float cy = ty + 12;
            for (const Collection* c : member) {
                chip(cx, cy, px, px + pw, to_wide(c->name.empty() ? std::string("Untitled") : c->name),
                     kCollectionColors[std::clamp(c->color, 0, kCollectionColorCount - 1)], {}, "col:" + c->id);
            }
        }
        y = top + panel_h + 14;
    }

    // Tags.
    {
        const float top = y;
        const float px = x + 18;
        const float pw = w - 36;
        // Lay the chips out once to know the height, then draw for real.
        float cx = px;
        float cy = 0;
        const TextStyle style{12.5f, Weight::Semibold};
        for (const std::string& tag : clip.tags) {
            const float cw = std::min(g.text_width(to_wide(tag), style), pw - 60) + 11 + 4 + 18 + 6;
            if (cx + cw > px + pw && cx > px) {
                cx = px;
                cy += 32;
            }
            cx += cw + 6;
        }
        if (!clip.trashed_at && cx + 104 > px + pw && cx > px) cy += 32;
        const float panel_h = 16 + 18 + 10 + cy + 26 + 16;
        g.fill_round(Rect{x, top, w, panel_h}, kPanelRadius, kCard);
        panel_title(px, top + 16, glyph::kTag, L"Tags");
        cx = px;
        cy = top + 16 + 18 + 10;
        for (std::size_t i = 0; i < clip.tags.size(); ++i) {
            chip(cx, cy, px, px + pw, to_wide(clip.tags[i]), std::nullopt,
                 clip.trashed_at ? std::string() : "tag-del:" + std::to_string(i), {});
        }
        if (!clip.trashed_at) {
            if (cx + 104 > px + pw && cx > px) {
                cx = px;
                cy += 32;
            }
            // .tag-in: a dashed pill (drawn solid), solid accent while typing.
            const Rect box{cx, cy, 104, 26};
            const bool focused = GetFocus() == tag_box_;
            g.stroke_round(box, 13, focused ? kAccent : kOff);
            place_control(tag_box_, Rect{box.x + 11, box.y + 5, box.w - 22, 16}, true);
        }
        y = top + panel_h + 14;
    }

    // Notes.
    {
        const float top = y;
        const float px = x + 18;
        const float pw = w - 36;
        const float panel_h = 16 + 18 + 10 + 90 + 16;
        g.fill_round(Rect{x, top, w, panel_h}, kPanelRadius, kCard);
        panel_title(px, top + 16, glyph::kPencil, L"Notes");
        const Rect box{px, top + 16 + 28, pw, 90};
        g.fill_round(box, kPageRadius, kBg);
        g.stroke_round(box, kPageRadius, GetFocus() == notes_box_ ? kAccent : kLine);
        if (GetWindowTextLengthW(notes_box_) == 0 && GetFocus() != notes_box_) {
            g.text(L"Add a note about this moment", Rect{box.x + 12, box.y + 10, box.w - 24, 18}, TextStyle{13.5f}, kMute);
        }
        place_control(notes_box_, Rect{box.x + 12, box.y + 10, box.w - 18, box.h - 18}, !clip.trashed_at);
        if (clip.trashed_at && !clip.notes.empty()) {
            g.paragraph(to_wide(clip.notes), Rect{box.x + 12, box.y + 10, box.w - 24, box.h - 20}, TextStyle{13.5f}, kInk);
        }
        y = top + panel_h + 14;
    }

    // Details.
    {
        std::vector<std::pair<std::wstring, std::wstring>> rows;
        if (clip.width > 0) rows.emplace_back(L"Resolution", std::format(L"{}×{}", clip.width, clip.height));
        rows.emplace_back(L"Length", clip.duration_s > 0 ? to_wide(format_duration(clip.duration_s)) : L"Unknown");
        rows.emplace_back(L"File size", to_wide(format_size(clip.size_bytes)));
        if (clip.duration_s > 0 && clip.size_bytes > 0) {
            const double mbps = static_cast<double>(clip.size_bytes) * 8.0 / clip.duration_s / 1e6;
            rows.emplace_back(L"Bitrate", std::format(L"{:.0f} Mbps", std::max(1.0, mbps)));
        }
        rows.emplace_back(L"Video", L"H.264");
        if (!clip.source.empty()) rows.emplace_back(L"Source", to_wide(clip.source));
        rows.emplace_back(L"Captured", captured_text(clip.created));
        rows.emplace_back(L"File", to_wide(clip.file));
        const float top = y;
        const float px = x + 18;
        const float pw = w - 36;
        const float panel_h = 16 + 18 + 10 + static_cast<float>(rows.size()) * 26 - 8 + 16;
        g.fill_round(Rect{x, top, w, panel_h}, kPanelRadius, kCard);
        panel_title(px, top + 16, glyph::kInfo, L"Details");
        float ry = top + 16 + 28;
        for (const auto& [label, value] : rows) {
            const float label_w = g.text(label, Rect{px, ry, pw / 2, 18}, TextStyle{13}, kMute);
            g.text(value, Rect{px + label_w + 16, ry, pw - label_w - 16, 18}, TextStyle{13, Weight::Regular, false, true},
                   kInk, Align::End);
            ry += 26;
        }
        y = top + panel_h;
    }
    return y;
}

}  // namespace bt::ui
