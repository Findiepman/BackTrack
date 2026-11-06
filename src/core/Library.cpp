#include "core/Library.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace bt {
namespace {

constexpr std::int64_t kKilobyte = 1000;  // the same decimal units Explorer's size column rounds to
constexpr std::int64_t kMegabyte = kKilobyte * kKilobyte;
constexpr std::int64_t kGigabyte = kMegabyte * kKilobyte;
constexpr int kSecondsPerMinute = 60;
constexpr int kSecondsPerHour = 3600;

TextRules g_text_rules;

char lower_ascii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// Identity of a file name or tag: Windows file names ignore case.
std::string file_key(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), lower_ascii);
    return out;
}

// For search and sorting: the installed rules, or ASCII.
std::string lower(std::string_view text) {
    return g_text_rules.fold != nullptr ? g_text_rules.fold(text) : file_key(text);
}

bool name_less(std::string_view a, std::string_view b) {
    if (g_text_rules.compare != nullptr) {
        return g_text_rules.compare(a, b) < 0;
    }
    return file_key(a) < file_key(b);
}

// Windows file names ignore case, so "Clip.mp4" and "clip.mp4" are the same file.
bool same_file(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return lower_ascii(x) == lower_ascii(y);
           });
}

std::string_view trim(std::string_view text) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    return text;
}

bool in_trash(const ClipEntry& clip) {
    return clip.trashed_at.has_value();
}

// Kept by the cleanup rules: the user marked it as worth keeping.
bool protected_clip(const ClipEntry& clip) {
    return clip.favorite || !clip.collections.empty();
}

Json strings_to_json(const std::vector<std::string>& values) {
    Json array{Json::Array{}};
    for (const std::string& value : values) array.push(Json(value));
    return array;
}

std::vector<std::string> strings_from_json(const Json& json) {
    std::vector<std::string> out;
    for (const Json& item : json.as_array()) {
        if (item.is_string()) out.push_back(item.as_string());
    }
    return out;
}

}  // namespace

bool Library::sync_with_folder(const std::vector<FolderFile>& files) {
    // Keyed once: this runs every few seconds on the UI thread, also with thousands of clips.
    std::unordered_map<std::string, const FolderFile*> on_disk;
    on_disk.reserve(files.size());
    for (const FolderFile& file : files) {
        on_disk.emplace(file_key(file.file), &file);
    }
    bool changed = false;
    const auto gone = std::remove_if(clips_.begin(), clips_.end(),
                                     [&on_disk](const ClipEntry& clip) { return !on_disk.contains(file_key(clip.file)); });
    if (gone != clips_.end()) {
        clips_.erase(gone, clips_.end());
        changed = true;
    }
    std::unordered_set<std::string> known;
    known.reserve(clips_.size());
    for (ClipEntry& clip : clips_) {
        const std::string key = file_key(clip.file);
        known.insert(key);
        const FolderFile& file = *on_disk.at(key);
        if (clip.size_bytes != file.size_bytes) {
            // Rewritten (another tool trimmed it): length and size must be probed again.
            clip.size_bytes = file.size_bytes;
            clip.duration_s = 0.0;
            changed = true;
        }
    }
    for (const FolderFile& file : files) {
        if (!known.insert(file_key(file.file)).second) {
            continue;  // already a clip (or the same name twice in the listing)
        }
        ClipEntry clip;
        clip.file = file.file;
        clip.size_bytes = file.size_bytes;
        clip.created = file.modified;
        clips_.push_back(std::move(clip));
        changed = true;
    }
    return changed;
}

void Library::add_saved_clip(const ClipEntry& clip) {
    if (ClipEntry* existing = find(clip.file)) {
        // The folder sync may have seen the file first; what the recorder knows wins, what the
        // user may already have edited (nothing, for a clip this new) is kept.
        existing->created = clip.created;
        existing->size_bytes = clip.size_bytes;
        existing->duration_s = clip.duration_s;
        existing->width = clip.width;
        existing->height = clip.height;
        existing->source = clip.source;
        existing->bookmarks = clip.bookmarks;
        return;
    }
    clips_.push_back(clip);
}

bool Library::set_media_info(std::string_view file, double duration_s, int width, int height) {
    ClipEntry* clip = find(file);
    if (clip == nullptr) return false;
    clip->duration_s = std::isfinite(duration_s) && duration_s > 0.0 ? duration_s : 0.0;
    clip->width = std::clamp(width, 0, kMaxVideoDimension);
    clip->height = std::clamp(height, 0, kMaxVideoDimension);
    return true;
}

ClipEntry* Library::find(std::string_view file) {
    for (ClipEntry& clip : clips_) {
        if (same_file(clip.file, file)) return &clip;
    }
    return nullptr;
}

const ClipEntry* Library::find(std::string_view file) const {
    for (const ClipEntry& clip : clips_) {
        if (same_file(clip.file, file)) return &clip;
    }
    return nullptr;
}

bool Library::remove(std::string_view file) {
    const auto it = std::find_if(clips_.begin(), clips_.end(), [file](const ClipEntry& c) { return same_file(c.file, file); });
    if (it == clips_.end()) return false;
    clips_.erase(it);
    return true;
}

std::vector<std::string> Library::needs_probe() const {
    std::vector<std::string> out;
    for (const ClipEntry& clip : clips_) {
        if (clip.duration_s <= 0.0) out.push_back(clip.file);
    }
    return out;
}

bool Library::set_favorite(std::string_view file, bool favorite) {
    ClipEntry* clip = find(file);
    if (clip == nullptr) return false;
    clip->favorite = favorite;
    return true;
}

bool Library::set_title(std::string_view file, std::string_view title) {
    ClipEntry* clip = find(file);
    if (clip == nullptr) return false;
    clip->title = std::string(trim(title));
    return true;
}

bool Library::set_notes(std::string_view file, std::string_view notes) {
    ClipEntry* clip = find(file);
    if (clip == nullptr) return false;
    clip->notes = std::string(notes);
    return true;
}

bool Library::add_tag(std::string_view file, std::string_view tag) {
    ClipEntry* clip = find(file);
    if (clip == nullptr) return false;
    tag = trim(tag);
    if (tag.empty()) return true;
    const bool known = std::any_of(clip->tags.begin(), clip->tags.end(), [tag](const std::string& t) { return same_file(t, tag); });
    if (!known) clip->tags.emplace_back(tag);
    return true;
}

bool Library::remove_tag(std::string_view file, std::string_view tag) {
    ClipEntry* clip = find(file);
    if (clip == nullptr) return false;
    std::erase_if(clip->tags, [tag](const std::string& t) { return same_file(t, tag); });
    return true;
}

bool Library::move_to_trash(std::string_view file, std::int64_t now) {
    ClipEntry* clip = find(file);
    if (clip == nullptr) return false;
    if (!clip->trashed_at) clip->trashed_at = now;
    return true;
}

bool Library::restore(std::string_view file) {
    ClipEntry* clip = find(file);
    if (clip == nullptr) return false;
    clip->trashed_at.reset();
    return true;
}

std::string Library::create_collection(std::string_view name, int color) {
    Collection collection;
    collection.id = "c" + std::to_string(next_collection_++);
    collection.name = std::string(trim(name));
    collection.color = std::max(color, 0);
    collections_.push_back(collection);
    return collection.id;
}

bool Library::rename_collection(std::string_view id, std::string_view name) {
    for (Collection& collection : collections_) {
        if (collection.id == id) {
            collection.name = std::string(trim(name));
            return true;
        }
    }
    return false;
}

bool Library::delete_collection(std::string_view id) {
    const auto removed = std::erase_if(collections_, [id](const Collection& c) { return c.id == id; });
    if (removed == 0) return false;
    for (ClipEntry& clip : clips_) {
        std::erase(clip.collections, std::string(id));
    }
    return true;
}

bool Library::set_in_collection(std::string_view file, std::string_view id, bool member) {
    ClipEntry* clip = find(file);
    const bool collection_exists =
        std::any_of(collections_.begin(), collections_.end(), [id](const Collection& c) { return c.id == id; });
    if (clip == nullptr || !collection_exists) return false;
    const auto it = std::find(clip->collections.begin(), clip->collections.end(), id);
    if (member && it == clip->collections.end()) {
        clip->collections.emplace_back(id);
    } else if (!member && it != clip->collections.end()) {
        clip->collections.erase(it);
    }
    return true;
}

std::size_t Library::collection_size(std::string_view id) const {
    return static_cast<std::size_t>(std::count_if(clips_.begin(), clips_.end(), [id](const ClipEntry& clip) {
        return !in_trash(clip) && std::find(clip.collections.begin(), clip.collections.end(), id) != clip.collections.end();
    }));
}

const Collection* Library::find_collection(std::string_view id) const {
    for (const Collection& c : collections_) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

std::vector<std::size_t> Library::query(const LibraryQuery& q) const {
    const std::string search(trim(q.search));
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < clips_.size(); ++i) {
        const ClipEntry& clip = clips_[i];
        if ((q.view == LibraryView::Trash) != in_trash(clip)) continue;
        if (q.view == LibraryView::Favorites && !clip.favorite) continue;
        if (q.view == LibraryView::Recent && clip.created < q.now - kRecentDays * kSecondsPerDay) continue;
        if (q.view == LibraryView::Collection &&
            std::find(clip.collections.begin(), clip.collections.end(), q.collection) == clip.collections.end()) {
            continue;
        }
        if (!q.tag.empty() &&
            std::none_of(clip.tags.begin(), clip.tags.end(), [&q](const std::string& t) { return same_file(t, q.tag); })) {
            continue;
        }
        if (!search.empty()) {
            const bool hit = contains_ignoring_case(display_title(clip), search) ||
                             contains_ignoring_case(clip.file, search) || contains_ignoring_case(clip.notes, search) ||
                             std::any_of(clip.tags.begin(), clip.tags.end(),
                                         [&search](const std::string& t) { return contains_ignoring_case(t, search); });
            if (!hit) continue;
        }
        out.push_back(i);
    }
    const auto by_name = [this](std::size_t a, std::size_t b) { return file_key(clips_[a].file) < file_key(clips_[b].file); };
    std::stable_sort(out.begin(), out.end(), [&](std::size_t a, std::size_t b) {
        const ClipEntry& x = clips_[a];
        const ClipEntry& y = clips_[b];
        switch (q.sort) {
            case ClipSort::Newest: if (x.created != y.created) return x.created > y.created; break;
            case ClipSort::Oldest: if (x.created != y.created) return x.created < y.created; break;
            case ClipSort::Longest: if (x.duration_s != y.duration_s) return x.duration_s > y.duration_s; break;
            case ClipSort::Shortest: if (x.duration_s != y.duration_s) return x.duration_s < y.duration_s; break;
            case ClipSort::Largest: if (x.size_bytes != y.size_bytes) return x.size_bytes > y.size_bytes; break;
            case ClipSort::Name: {
                const std::string nx = display_title(x);
                const std::string ny = display_title(y);
                if (name_less(nx, ny)) return true;
                if (name_less(ny, nx)) return false;
                break;
            }
        }
        return by_name(a, b);
    });
    return out;
}

LibraryCounts Library::counts(std::int64_t now) const {
    LibraryCounts c;
    for (const ClipEntry& clip : clips_) {
        c.bytes += clip.size_bytes;
        if (in_trash(clip)) {
            ++c.trash;
            continue;
        }
        ++c.all;
        if (clip.favorite) ++c.favorites;
        if (clip.created >= now - kRecentDays * kSecondsPerDay) ++c.recent;
    }
    return c;
}

std::vector<std::string> Library::all_tags() const {
    std::vector<std::string> tags;
    std::set<std::string> seen;
    for (const ClipEntry& clip : clips_) {
        for (const std::string& tag : clip.tags) {
            if (seen.insert(file_key(tag)).second) tags.push_back(tag);
        }
    }
    std::sort(tags.begin(), tags.end(), [](const std::string& a, const std::string& b) { return name_less(a, b); });
    return tags;
}

std::vector<std::string> Library::due_for_removal(const CleanupPolicy& policy, std::int64_t now) const {
    std::vector<std::string> out;
    std::vector<bool> taken(clips_.size(), false);
    std::int64_t total = 0;
    for (std::size_t i = 0; i < clips_.size(); ++i) {
        const ClipEntry& clip = clips_[i];
        const bool trash_expired = in_trash(clip) && *clip.trashed_at <= now - kTrashDays * kSecondsPerDay;
        const bool too_old = !in_trash(clip) && !protected_clip(clip) && policy.delete_after_days > 0 &&
                             clip.created <= now - policy.delete_after_days * kSecondsPerDay;
        if (trash_expired || too_old) {
            out.push_back(clip.file);
            taken[i] = true;
        } else {
            total += clip.size_bytes;
        }
    }
    if (policy.max_bytes > 0 && total > policy.max_bytes) {
        // Trash first (oldest in first), then the oldest unprotected clips.
        std::vector<std::size_t> order;
        for (std::size_t i = 0; i < clips_.size(); ++i) {
            if (!taken[i] && (in_trash(clips_[i]) || !protected_clip(clips_[i]))) order.push_back(i);
        }
        std::stable_sort(order.begin(), order.end(), [this](std::size_t a, std::size_t b) {
            const ClipEntry& x = clips_[a];
            const ClipEntry& y = clips_[b];
            if (in_trash(x) != in_trash(y)) return in_trash(x);
            const std::int64_t tx = in_trash(x) ? *x.trashed_at : x.created;
            const std::int64_t ty = in_trash(y) ? *y.trashed_at : y.created;
            return tx < ty;
        });
        for (const std::size_t i : order) {
            if (total <= policy.max_bytes) break;
            out.push_back(clips_[i].file);
            total -= clips_[i].size_bytes;
        }
    }
    return out;
}

Json Library::to_json() const {
    Json root;
    root.set("version", Json(kLibraryFileVersion));
    root.set("next_collection", Json(next_collection_));
    Json collections{Json::Array{}};
    for (const Collection& c : collections_) {
        Json item;
        item.set("id", Json(c.id));
        item.set("name", Json(c.name));
        item.set("color", Json(c.color));
        collections.push(std::move(item));
    }
    root.set("collections", std::move(collections));
    Json clips{Json::Array{}};
    for (const ClipEntry& clip : clips_) {
        Json item;
        item.set("file", Json(clip.file));
        if (!clip.title.empty()) item.set("title", Json(clip.title));
        if (!clip.notes.empty()) item.set("notes", Json(clip.notes));
        if (!clip.tags.empty()) item.set("tags", strings_to_json(clip.tags));
        if (!clip.collections.empty()) item.set("collections", strings_to_json(clip.collections));
        if (clip.favorite) item.set("favorite", Json(true));
        item.set("created", Json(clip.created));
        item.set("size", Json(clip.size_bytes));
        item.set("duration", Json(clip.duration_s));
        item.set("width", Json(clip.width));
        item.set("height", Json(clip.height));
        if (!clip.source.empty()) item.set("source", Json(clip.source));
        if (clip.trashed_at) item.set("trashed_at", Json(*clip.trashed_at));
        if (!clip.bookmarks.empty()) {
            Json marks{Json::Array{}};
            for (const double b : clip.bookmarks) marks.push(Json(b));
            item.set("bookmarks", std::move(marks));
        }
        clips.push(std::move(item));
    }
    root.set("clips", std::move(clips));
    return root;
}

Library Library::from_json(const Json& json) {
    Library library;
    std::set<std::string> collection_ids;
    int highest_id = 0;
    for (const Json& item : json["collections"].as_array()) {
        Collection c;
        c.id = item["id"].as_string();
        c.name = item["name"].as_string();
        c.color = static_cast<int>(std::clamp<std::int64_t>(item["color"].as_int(), 0, 1000));
        if (c.id.empty() || !collection_ids.insert(c.id).second) continue;
        if (c.id.size() > 1 && c.id[0] == 'c') {
            // Keep "never reused" true even if next_collection was lost.
            const std::int64_t number = Json(std::strtod(c.id.c_str() + 1, nullptr)).as_int();
            highest_id = static_cast<int>(std::clamp<std::int64_t>(number, highest_id, 1'000'000'000));
        }
        library.collections_.push_back(std::move(c));
    }
    library.next_collection_ = static_cast<int>(
        std::clamp<std::int64_t>(json["next_collection"].as_int(1), static_cast<std::int64_t>(highest_id) + 1, 1'000'000'001));

    std::unordered_set<std::string> files;
    for (const Json& item : json["clips"].as_array()) {
        ClipEntry clip;
        clip.file = item["file"].as_string();
        if (clip.file.empty() || !files.insert(file_key(clip.file)).second) continue;
        clip.title = item["title"].as_string();
        clip.notes = item["notes"].as_string();
        for (const std::string& tag : strings_from_json(item["tags"])) {
            if (!trim(tag).empty() &&
                std::none_of(clip.tags.begin(), clip.tags.end(), [&tag](const std::string& t) { return same_file(t, tag); })) {
                clip.tags.push_back(tag);
            }
        }
        for (const std::string& id : strings_from_json(item["collections"])) {
            // A membership in a collection that no longer exists is dropped.
            if (collection_ids.contains(id) &&
                std::find(clip.collections.begin(), clip.collections.end(), id) == clip.collections.end()) {
                clip.collections.push_back(id);
            }
        }
        clip.favorite = item["favorite"].as_bool();
        clip.created = item["created"].as_int();
        clip.size_bytes = std::clamp<std::int64_t>(item["size"].as_int(), 0, kMaxClipBytes);
        const double duration = item["duration"].as_number();
        clip.duration_s = std::isfinite(duration) && duration > 0.0 ? duration : 0.0;
        clip.width = static_cast<int>(std::clamp<std::int64_t>(item["width"].as_int(), 0, kMaxVideoDimension));
        clip.height = static_cast<int>(std::clamp<std::int64_t>(item["height"].as_int(), 0, kMaxVideoDimension));
        clip.source = item["source"].as_string();
        if (item["trashed_at"].is_number()) clip.trashed_at = item["trashed_at"].as_int();
        std::vector<double> marks;
        for (const Json& mark : item["bookmarks"].as_array()) {
            if (mark.is_number()) marks.push_back(mark.as_number());
        }
        clip.bookmarks = clean_bookmarks(std::move(marks), clip.duration_s);
        library.clips_.push_back(std::move(clip));
    }
    return library;
}

bool Library::set_bookmarks(std::string_view file, std::vector<double> seconds) {
    ClipEntry* clip = find(file);
    if (clip == nullptr) return false;
    clip->bookmarks = clean_bookmarks(std::move(seconds), clip->duration_s);
    return true;
}

std::vector<double> clean_bookmarks(std::vector<double> seconds, double length) {
    std::erase_if(seconds, [length](double s) { return !std::isfinite(s) || s < 0.0 || (length > 0.0 && s > length); });
    std::sort(seconds.begin(), seconds.end());
    std::vector<double> out;
    for (const double s : seconds) {
        if (!out.empty() && s - out.back() < 0.1) continue;
        out.push_back(s);
        if (out.size() == kMaxBookmarks) break;
    }
    return out;
}

std::vector<double> bookmarks_within(const std::vector<double>& marks, double start, double length) {
    std::vector<double> out;
    for (const double m : marks) {
        if (std::isfinite(m) && m >= start && m < start + length) out.push_back(m - start);
    }
    return clean_bookmarks(std::move(out), length);
}

std::string display_title(const ClipEntry& clip) {
    if (!clip.title.empty()) return clip.title;
    const std::size_t dot = clip.file.rfind('.');
    return dot == std::string::npos || dot == 0 ? clip.file : clip.file.substr(0, dot);
}

std::string format_duration(double seconds) {
    const long long total = std::isfinite(seconds) && seconds > 0.0 ? std::llround(seconds) : 0;
    const long long hours = total / kSecondsPerHour;
    const long long minutes = (total % kSecondsPerHour) / kSecondsPerMinute;
    const long long secs = total % kSecondsPerMinute;
    char buffer[32];
    if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%lld:%02lld:%02lld", hours, minutes, secs);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%lld:%02lld", minutes, secs);
    }
    return buffer;
}

std::string format_size(std::int64_t bytes) {
    char buffer[32];
    bytes = std::max<std::int64_t>(bytes, 0);
    if (bytes >= kGigabyte) {
        std::snprintf(buffer, sizeof(buffer), "%.1f GB", static_cast<double>(bytes) / static_cast<double>(kGigabyte));
    } else if (bytes >= kMegabyte) {
        std::snprintf(buffer, sizeof(buffer), "%lld MB", static_cast<long long>((bytes + kMegabyte / 2) / kMegabyte));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%lld KB", static_cast<long long>((bytes + kKilobyte / 2) / kKilobyte));
    }
    return buffer;
}

std::string format_age(std::int64_t seconds_ago) {
    constexpr std::int64_t kMinute = 60;
    constexpr std::int64_t kHour = 60 * kMinute;
    constexpr std::int64_t kWeek = 7 * kSecondsPerDay;
    constexpr std::int64_t kMonth = 30 * kSecondsPerDay;
    constexpr std::int64_t kYear = 365 * kSecondsPerDay;
    if (seconds_ago < kMinute) return "Just now";
    if (seconds_ago < kHour) return std::to_string(seconds_ago / kMinute) + "m ago";
    if (seconds_ago < kSecondsPerDay) return std::to_string(seconds_ago / kHour) + "h ago";
    if (seconds_ago < kWeek * 2) return std::to_string(seconds_ago / kSecondsPerDay) + "d ago";
    if (seconds_ago < kMonth * 2) return std::to_string(seconds_ago / kWeek) + "w ago";
    if (seconds_ago < kYear) return std::to_string(seconds_ago / kMonth) + "mo ago";
    return std::to_string(seconds_ago / kYear) + "y ago";
}

std::string format_resolution(int width, int height) {
    if (width <= 0 || height <= 0) return {};
    // Named by height when the shape is the usual 16:9 (or close: 1920x1080, 2560x1440, 3840x2160).
    // In 64 bits: a damaged file can claim any size.
    const std::int64_t w9 = static_cast<std::int64_t>(width) * 9;
    const std::int64_t h16 = static_cast<std::int64_t>(height) * 16;
    const bool wide = w9 >= h16 - 16 && w9 <= h16 + 16;
    if (wide && height == 2160) return "4K";
    if (wide && (height == 720 || height == 1080 || height == 1440 || height == 480)) return std::to_string(height) + "p";
    return std::to_string(width) + "x" + std::to_string(height);
}

void set_text_rules(TextRules rules) {
    g_text_rules = rules;
}

bool contains_ignoring_case(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

}  // namespace bt
