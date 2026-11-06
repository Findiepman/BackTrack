#pragma once

// The clip library behind the UI: what Backtrack knows about each saved clip beyond the file
// itself (favorite, title, tags, notes, collections, trash) and the queries the library screens
// run. Pure logic, no Windows: the platform side lists the clips folder and probes files, this
// merges that in, and library.json is just to_json/from_json of it.
//
// Clips are identified by their file name inside the clips folder. A file renamed in Explorer
// shows up as a new clip and the old entry goes; Backtrack itself never renames clips.

#include "core/Json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bt {

struct ClipEntry {
    std::string file;  // file name in the clips folder, UTF-8 ("clip_2026-11-04_15-40-39.mp4")
    std::string title;  // empty: the UI shows the file name without extension
    std::string notes;
    std::vector<std::string> tags;  // as typed, unique ignoring ASCII case
    std::vector<std::string> collections;  // collection ids
    bool favorite = false;
    std::int64_t created = 0;  // Unix seconds: when the clip was saved (file write time for found files)
    std::int64_t size_bytes = 0;
    double duration_s = 0.0;  // 0: not probed yet
    int width = 0;
    int height = 0;
    std::string source;  // what was recorded, e.g. "Display 1"; empty for clips found on disk
    std::optional<std::int64_t> trashed_at;  // Unix seconds; set while the clip is in the Trash
    std::vector<double> bookmarks;  // seconds from the clip's start, sorted (the bookmark hotkey)
    std::string share_url;  // the link from Share; empty: never shared
    std::int64_t shared_at = 0;  // Unix seconds
    std::int64_t share_expires = 0;  // Unix seconds the host deletes it; 0: it stays
};

struct Collection {
    std::string id;  // "c1", "c2", ... never reused
    std::string name;
    int color = 0;  // index into the UI's collection palette
};

// One file the clips folder listing found.
struct FolderFile {
    std::string file;
    std::int64_t size_bytes = 0;
    std::int64_t modified = 0;  // Unix seconds
};

enum class LibraryView { All, Favorites, Recent, Trash, Collection, Shared };
enum class ClipSort { Newest, Oldest, Longest, Shortest, Largest, Name };

struct LibraryQuery {
    LibraryView view = LibraryView::All;
    std::string collection;  // for LibraryView::Collection
    std::string search;  // matches title/file name, tags and notes, ignoring ASCII case
    std::string tag;  // empty: any
    ClipSort sort = ClipSort::Newest;
    std::int64_t now = 0;  // Unix seconds, for Recent
};

// "Recent" in the sidebar: saved within this many days.
inline constexpr std::int64_t kRecentDays = 7;
// The Trash is emptied (to the Windows Recycle Bin) this many days after a clip went in.
inline constexpr std::int64_t kTrashDays = 30;
inline constexpr std::int64_t kSecondsPerDay = 24 * 60 * 60;
// The "version" written in library.json. A file with a higher one comes from a newer Backtrack.
inline constexpr int kLibraryFileVersion = 1;
// More bookmarks than this in one clip are damage (or a stuck key): the rest are dropped.
inline constexpr std::size_t kMaxBookmarks = 200;
// Larger sizes in library.json are damage; clamped so adding them up cannot overflow.
inline constexpr std::int64_t kMaxClipBytes = 1'000'000'000'000;  // 1 TB
inline constexpr int kMaxVideoDimension = 100'000;

struct CleanupPolicy {
    int delete_after_days = 0;  // 0: never. Favorites and clips in a collection are kept.
    std::int64_t max_bytes = 0;  // 0: no limit. Oldest first; favorites and collection clips kept.
};

struct LibraryCounts {
    std::size_t all = 0;
    std::size_t favorites = 0;
    std::size_t recent = 0;
    std::size_t trash = 0;
    std::size_t shared = 0;  // with a link that still works
    std::int64_t bytes = 0;  // every clip on disk, Trash included
};

class Library {
public:
    const std::vector<ClipEntry>& clips() const { return clips_; }
    const std::vector<Collection>& collections() const { return collections_; }

    // Brings the library in line with the clips folder: new files become clips (created = file
    // time), sizes are refreshed, entries whose file is gone are removed. True when anything
    // changed. Clips with duration 0 still need probing (needs_probe).
    bool sync_with_folder(const std::vector<FolderFile>& files);

    // A clip Backtrack just saved: added (or updated, if a folder sync saw the file first) with
    // what the recorder knows.
    void add_saved_clip(const ClipEntry& clip);

    // Results of probing a file for its length and size. False when the clip is unknown.
    bool set_media_info(std::string_view file, double duration_s, int width, int height);

    ClipEntry* find(std::string_view file);
    const ClipEntry* find(std::string_view file) const;
    bool remove(std::string_view file);  // the file was deleted
    std::vector<std::string> needs_probe() const;

    // Edits from the UI. Each returns false when the clip or collection does not exist.
    bool set_favorite(std::string_view file, bool favorite);
    bool set_title(std::string_view file, std::string_view title);
    bool set_notes(std::string_view file, std::string_view notes);
    bool set_bookmarks(std::string_view file, std::vector<double> seconds);  // cleaned up by clean_bookmarks
    // After an upload: the link, when, and when the host deletes it (0: never).
    bool set_shared(std::string_view file, std::string_view url, std::int64_t at, std::int64_t expires);
    bool add_tag(std::string_view file, std::string_view tag);  // trimmed; empty or duplicate ignored
    bool remove_tag(std::string_view file, std::string_view tag);
    bool move_to_trash(std::string_view file, std::int64_t now);
    bool restore(std::string_view file);

    std::string create_collection(std::string_view name, int color);  // returns the new id
    bool rename_collection(std::string_view id, std::string_view name);
    bool delete_collection(std::string_view id);  // clips stay, only lose the membership
    bool set_in_collection(std::string_view file, std::string_view id, bool member);
    std::size_t collection_size(std::string_view id) const;  // not counting the Trash
    const Collection* find_collection(std::string_view id) const;

    // Indices into clips(), filtered and sorted.
    std::vector<std::size_t> query(const LibraryQuery& query) const;
    LibraryCounts counts(std::int64_t now) const;
    std::vector<std::string> all_tags() const;  // sorted, unique ignoring ASCII case

    // Files to send to the Recycle Bin now: Trash older than kTrashDays, then the cleanup policy.
    // Never a favorite or a clip in a collection (unless it is in the Trash).
    std::vector<std::string> due_for_removal(const CleanupPolicy& policy, std::int64_t now) const;

    Json to_json() const;
    // A missing or damaged part keeps its default; unknown keys are ignored (a newer version's file).
    static Library from_json(const Json& json);

private:
    std::vector<ClipEntry> clips_;
    std::vector<Collection> collections_;
    int next_collection_ = 1;
};

// How text is matched and sorted ignoring case. Core only knows ASCII; the app installs Windows'
// rules at startup (so "eclair" with an accent finds its capital form, and accented names sort
// next to the plain ones). Set once, before any Library is used from more than one thread.
struct TextRules {
    std::string (*fold)(std::string_view text) = nullptr;  // lower case, for search
    int (*compare)(std::string_view a, std::string_view b) = nullptr;  // < 0, 0, > 0, for sorting by name
};
void set_text_rules(TextRules rules);

// Shown when a clip has no title of its own: the file name without its extension.
std::string display_title(const ClipEntry& clip);
// "0:30", "1:05", "1:02:03"; rounds to whole seconds.
std::string format_duration(double seconds);
// "41 MB", "1.2 GB", "820 KB".
std::string format_size(std::int64_t bytes);
// How long ago, the way the library cards say it: "Just now", "36m ago", "5h ago", "2d ago",
// "3w ago", "4mo ago", "2y ago". A time in the future (clock changed) is "Just now".
std::string format_age(std::int64_t seconds_ago);
// "1080p", "1440p", "4K", "720p", or "1366x768" for anything else; empty when unknown.
std::string format_resolution(int width, int height);
// ASCII case-insensitive substring search (UTF-8 bytes beyond ASCII compare exactly).
bool contains_ignoring_case(std::string_view haystack, std::string_view needle);

// Sorted, inside 0..length (length 0: not known, no upper bound), not NaN, marks closer than
// 0.1 s merged (one press seen twice), at most kMaxBookmarks.
std::vector<double> clean_bookmarks(std::vector<double> seconds, double length);

// The bookmarks that fall inside a piece [start, start + length) of a longer timeline, as offsets
// from its start: a saved clip out of the replay buffer, a trimmed clip out of its original.
std::vector<double> bookmarks_within(const std::vector<double>& marks, double start, double length);

// A link that still works at `now` (Litterbox links stop after 72 hours).
bool share_link_live(const ClipEntry& clip, std::int64_t now);

}  // namespace bt
