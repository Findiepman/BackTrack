// "Repair clips from other programs" in Settings › Storage: finds clips whose microphone is on a
// second sound track (Medal does that; Discord and most players only play the first one) and
// repairs them with platform/Repair: a mixed first track, the video untouched.
//
// One clip per worker job, so thumbnails and the rest of the window keep working between clips
// and a cancel takes effect at once. The repaired copy is written into "Before repair" (not
// scanned by the library, so it never shows up as a clip of its own) and swapped in on the UI
// thread: the folder scan runs there too, so it never sees the moment the clip is missing, and
// the clip keeps its name, so favorites, tags and notes stay with it. The original is kept in
// "Before repair".

#include "platform/Log.h"
#include "platform/Repair.h"
#include "platform/Shell.h"
#include "platform/Text.h"
#include "ui/App.h"
#include "ui/AppIds.h"
#include "ui/Theme.h"

#include <windows.h>

#include <algorithm>
#include <format>

namespace bt::ui {

using namespace theme;
using namespace ids;

namespace {

namespace fs = std::filesystem;

constexpr wchar_t kBackupFolder[] = L"Before repair";

std::wstring clips_word(std::size_t n) { return n == 1 ? L"clip" : L"clips"; }

}  // namespace

bool App::clip_in_use(const std::string& file) const {
    return (clip_view_ && clip_view_->file == file) || (export_ && export_->file == file) ||
           (share_ && share_->file == file);
}

void App::start_repair_scan(bool quiet) {
    if (repair_ && (repair_->stage == RepairView::Stage::Scanning || repair_->stage == RepairView::Stage::Working)) return;
    if (recorder_->output_dir_is_fallback()) {
        if (!quiet) clips_folder_available();  // says why
        return;
    }
    if (repair_) repair_->cancel->store(true);
    auto view = std::make_shared<RepairView>();
    view->stage = RepairView::Stage::Scanning;
    view->folder = recorder_->output_dir();
    repair_ = view;
    std::vector<std::string> files;
    for (const ClipEntry& clip : library_.clips()) files.push_back(clip.file);
    run_in_background([this, view, files, quiet]() -> std::function<void()> {
        std::vector<std::string> found;
        std::int64_t bytes = 0;
        for (const std::string& file : files) {
            if (view->cancel->load()) break;
            const fs::path path = view->folder / to_wide(file);
            const std::optional<RepairCheck> check = check_for_repair(path);
            if (!check || !check->needed) continue;
            found.push_back(file);
            std::error_code ec;
            const auto size = fs::file_size(path, ec);
            if (!ec) bytes += static_cast<std::int64_t>(size);
        }
        std::int64_t free_bytes = -1;
        std::error_code ec;
        const fs::space_info space = fs::space(view->folder, ec);
        if (!ec) free_bytes = static_cast<std::int64_t>(space.available);
        return [this, view, found = std::move(found), bytes, free_bytes, quiet] {
            if (repair_ != view || view->stage != RepairView::Stage::Scanning) return;
            view->files = found;
            view->total_bytes = bytes;
            view->free_bytes = free_bytes;
            view->stage = RepairView::Stage::Found;
            log::info("Repair check: {} clips in {} can be repaired", found.size(), to_utf8(view->folder.wstring()));
            if (quiet) {
                if (found.empty()) {
                    repair_.reset();
                } else {
                    show_notice(std::format(L"{} {} from another program can be repaired so the mic is heard. "
                                            L"See Settings, Storage.",
                                            found.size(), clips_word(found.size())));
                }
            }
            invalidate();
        };
    });
    invalidate();
}

void App::start_repair() {
    if (!repair_ || repair_->stage != RepairView::Stage::Found || repair_->files.empty()) return;
    if (recorder_->output_dir() != repair_->folder) {
        repair_.reset();  // the clips folder changed since the check
        invalidate();
        return;
    }
    std::error_code ec;
    fs::create_directories(repair_->folder / kBackupFolder, ec);
    if (ec) {
        show_notice(L"Backtrack cannot make the \"Before repair\" folder in the clips folder.");
        return;
    }
    // Repaired copies a quit or a crash left before their swap.
    for (fs::directory_iterator it(repair_->folder / kBackupFolder, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().filename().wstring().starts_with(L"~repairing ")) {
            std::error_code remove_ec;
            fs::remove(it->path(), remove_ec);
        }
    }
    repair_->stage = RepairView::Stage::Working;
    repair_->next = 0;
    log::info("Repairing {} clips in {}", repair_->files.size(), to_utf8(repair_->folder.wstring()));
    repair_next();
}

void App::cancel_repair() {
    if (!repair_) return;
    repair_->cancel->store(true);
    if (repair_->stage == RepairView::Stage::Scanning) repair_.reset();
    invalidate();
}

void App::finish_repair() {
    const std::shared_ptr<RepairView> view = repair_;
    view->stage = RepairView::Stage::Done;
    log::info("Repair finished: {} repaired, {} failed, {} skipped", view->repaired, view->failed.size(), view->skipped);
    if (page_ != Page::Settings) {
        show_notice(std::format(L"Repaired {} {}.", view->repaired, clips_word(view->repaired)));
    }
    invalidate();
}

// The next clip of the list, or the end. Called on the UI thread.
void App::repair_next() {
    const std::shared_ptr<RepairView> view = repair_;
    if (!view || view->stage != RepairView::Stage::Working) return;
    if (view->cancel->load() || recorder_->output_dir() != view->folder) {
        finish_repair();
        return;
    }
    while (view->next < view->files.size()) {
        const std::string& file = view->files[view->next];
        // Gone meanwhile (deleted in Explorer), or open in Backtrack: left as it is.
        if (library_.find(file) == nullptr || clip_in_use(file)) {
            ++view->skipped;
            ++view->next;
            continue;
        }
        break;
    }
    if (view->next >= view->files.size()) {
        finish_repair();
        return;
    }
    const std::string file = view->files[view->next];
    view->current = file;
    view->clip_progress->store(0.0);
    const fs::path source = view->folder / to_wide(file);
    const fs::path repaired = view->folder / kBackupFolder / (L"~repairing " + to_wide(file));
    const HWND window = window_;
    run_in_background([this, view, file, source, repaired, window]() -> std::function<void()> {
        std::error_code ec;
        fs::remove(repaired, ec);  // a half one from a run that was killed
        auto last = std::chrono::steady_clock::now();
        const RepairResult result = repair_clip(
            source, repaired,
            [&](double f) {
                view->clip_progress->store(f);
                const auto now = std::chrono::steady_clock::now();
                if (now - last > std::chrono::milliseconds(200)) {
                    last = now;
                    PostMessageW(window, kMsgExport, 0, 0);  // repaints the window
                }
            },
            *view->cancel);
        return [this, view, file, source, repaired, result] { complete_repair_clip(view, file, source, repaired, result); };
    });
    invalidate();
}

// On the UI thread, after repair_clip: the swap, then the next clip.
void App::complete_repair_clip(const std::shared_ptr<RepairView>& view, const std::string& file, const fs::path& source,
                               const fs::path& repaired, const RepairResult& result) {
    const auto discard = [&] {
        std::error_code remove_ec;
        fs::remove(repaired, remove_ec);
    };
    if (repair_ != view) {
        discard();
        return;
    }
    if (recycling_ || shell_busy_) {
        // Inside the message loop of a Recycle or a folder move: a rename now could move a file the
        // shell is working on. Again once that is done.
        run_in_background([this, view, file, source, repaired, result]() -> std::function<void()> {
            Sleep(200);
            return [this, view, file, source, repaired, result] {
                complete_repair_clip(view, file, source, repaired, result);
            };
        });
        return;
    }
    if (result.cancelled) {
        discard();
    } else if (!result.ok) {
        log::warn("Could not repair {}: {}", file, result.message);
        view->failed.push_back({file, result.message});
        discard();
    } else if (clip_in_use(file) || library_.find(file) == nullptr) {
        ++view->skipped;  // opened or deleted while it was being repaired
        discard();
    } else {
        // The swap: the original into "Before repair", the repaired copy in its place. All in the
        // same folder tree on one drive, so both are renames.
        const fs::path backup = backup_path(view->folder, source.filename());
        if (backup.empty() || !MoveFileExW(source.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH)) {
            view->failed.push_back({file, "The clip is in use by another program."});
            discard();
        } else if (!MoveFileExW(repaired.c_str(), source.c_str(), MOVEFILE_WRITE_THROUGH)) {
            if (MoveFileExW(backup.c_str(), source.c_str(), MOVEFILE_WRITE_THROUGH)) {
                view->failed.push_back({file, "The repaired clip could not take the original's place."});
                discard();
            } else {
                // Neither is in the clips folder now. The repaired copy is kept too: nothing is lost.
                log::error("Repair of {}: the original stays at {}", file, to_utf8(backup.wstring()));
                view->failed.push_back({file, "Its original is in \"Before repair\"; move it back to the clips folder."});
                show_notice(L"A clip could not be put back. Its original is in the \"Before repair\" folder.");
            }
        } else {
            ++view->repaired;
            log::info("Repaired {} (original in {})", file, to_utf8(backup.wstring()));
            scan_folder();  // the new size: a new thumbnail and length
        }
    }
    ++view->next;
    repair_next();
}

void App::run_repair(const std::string& what) {
    if (what == "repair-scan") {
        start_repair_scan(false);
    } else if (what == "repair-go") {
        start_repair();
    } else if (what == "repair-cancel") {
        cancel_repair();
    } else if (what == "repair-backups") {
        shell::open(recorder_->output_dir() / kBackupFolder);
    } else if (what == "repair-reset") {
        if (repair_) repair_->cancel->store(true);
        repair_.reset();
        invalidate();
    }
}

float App::repair_row_height() const {
    if (!repair_) return 30;
    switch (repair_->stage) {
        case RepairView::Stage::Working: return 30 + 10 + 8;
        case RepairView::Stage::Done: return repair_->failed.empty() ? 30 : 30 + 8 + 18 * static_cast<float>(std::min<std::size_t>(repair_->failed.size(), 3));
        default: return 30;
    }
}

void App::paint_repair_row(const Rect& area) {
    Graphics& g = *graphics_;
    const Rect line{area.x, area.y, area.w, 30};
    const auto button_right = [&](const wchar_t* label, const char* arg, ButtonStyle style, bool disabled = false) {
        const float w = button_width(label, 0, true);
        push_button(area.right() - w, area.y, label, 0, style, true, Action::Setting, arg, disabled);
        return w;
    };
    const auto status = [&](const std::wstring& text, float right_gap, Color color) {
        g.text(text, Rect{line.x, line.y, line.w - right_gap - 12, line.h}, TextStyle{13, Weight::Semibold}, color);
    };
    if (!repair_) {
        button_right(L"Check clips", "repair-scan", ButtonStyle::Line);
        return;
    }
    RepairView& view = *repair_;
    switch (view.stage) {
        case RepairView::Stage::Scanning: {
            const float w = button_right(L"Checking...", "repair-scan", ButtonStyle::Line, true);
            status(L"Checking the clips...", w, kMute);
            break;
        }
        case RepairView::Stage::Found: {
            if (view.files.empty()) {
                const float w = button_right(L"Check again", "repair-scan", ButtonStyle::Line);
                status(L"No clip needs a repair.", w, kOk);
                break;
            }
            // The originals stay, so the repaired copies need about as much room again.
            const bool room = view.free_bytes < 0 || view.free_bytes > view.total_bytes + 2'000'000'000LL;
            const std::wstring label = std::format(L"Repair {} {}", view.files.size(), clips_word(view.files.size()));
            const float w = button_right(label.c_str(), "repair-go", ButtonStyle::Primary, !room);
            status(room ? std::format(L"{} {} can be repaired. Needs {} free.", view.files.size(),
                                      clips_word(view.files.size()), to_wide(format_size(view.total_bytes)))
                        : std::format(L"Not enough free space: the repair needs {}.", to_wide(format_size(view.total_bytes))),
                   w, room ? kInk : kWarn);
            break;
        }
        case RepairView::Stage::Working: {
            const float w = button_right(L"Cancel", "repair-cancel", ButtonStyle::Line, view.cancel->load());
            const std::size_t total = view.files.size();
            const double fraction =
                total == 0 ? 0.0
                           : (static_cast<double>(view.next) + std::clamp(view.clip_progress->load(), 0.0, 1.0)) /
                                 static_cast<double>(total);
            status(view.cancel->load() ? std::wstring(L"Stopping...")
                                       : std::format(L"Repairing {} of {}: {}", std::min(view.next + 1, total), total,
                                                     to_wide(view.current)),
                   w, kInk);
            const Rect bar{area.x, area.y + 40, area.w, 8};
            g.fill_round(bar, 4, kLine);
            g.fill_round(Rect{bar.x, bar.y, bar.w * static_cast<float>(fraction), bar.h}, 4, kAccent);
            break;
        }
        case RepairView::Stage::Done: {
            const float open_w = button_width(L"Open backups", glyph::kFolder, true);
            push_button(area.right() - open_w, area.y, L"Open backups", glyph::kFolder, ButtonStyle::Ghost, true,
                        Action::Setting, "repair-backups", view.repaired == 0);
            const float again_w = button_width(L"Check again", 0, true);
            push_button(area.right() - open_w - 8 - again_w, area.y, L"Check again", 0, ButtonStyle::Line, true,
                        Action::Setting, "repair-scan");
            std::wstring text = std::format(L"Repaired {} {}.", view.repaired, clips_word(view.repaired));
            if (!view.failed.empty()) text += std::format(L" {} could not be repaired.", view.failed.size());
            if (view.skipped > 0) text += std::format(L" {} skipped (open or deleted).", view.skipped);
            status(text, open_w + 8 + again_w, view.failed.empty() ? kOk : kWarn);
            float y = area.y + 38;
            for (std::size_t i = 0; i < view.failed.size() && i < 3; ++i) {
                g.text(to_wide(view.failed[i].first + ": " + view.failed[i].second), Rect{area.x, y, area.w, 18},
                       TextStyle{12}, kMute);
                y += 18;
            }
            break;
        }
    }
}

}  // namespace bt::ui
