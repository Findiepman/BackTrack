#include "core/Library.h"

#include "Check.h"

#include <cmath>
#include <limits>

using namespace bt;

namespace {

constexpr std::int64_t kNow = 1'762'300'000;  // November 2025
constexpr std::int64_t kDay = kSecondsPerDay;
constexpr std::int64_t kMb = 1'000'000;

FolderFile file(std::string name, std::int64_t size, std::int64_t modified) {
    return FolderFile{std::move(name), size, modified};
}

std::vector<std::string> names(const Library& lib, const std::vector<std::size_t>& indices) {
    std::vector<std::string> out;
    for (std::size_t i : indices) out.push_back(lib.clips()[i].file);
    return out;
}

std::string joined(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) out += (out.empty() ? "" : ",") + s;
    return out;
}

Library sample() {
    Library lib;
    lib.sync_with_folder({file("a.mp4", 40 * kMb, kNow - 1 * kDay), file("b.mp4", 60 * kMb, kNow - 10 * kDay),
                          file("c.mp4", 20 * kMb, kNow - 40 * kDay)});
    lib.set_media_info("a.mp4", 30.0, 1920, 1080);
    lib.set_media_info("b.mp4", 60.4, 1920, 1080);
    lib.set_media_info("c.mp4", 15.0, 2560, 1440);
    return lib;
}

void folder_sync() {
    Library lib;
    CHECK(lib.sync_with_folder({file("a.mp4", 10, 100), file("b.mp4", 20, 200)}));
    CHECK_EQ(lib.clips().size(), std::size_t{2});
    CHECK_EQ(lib.needs_probe().size(), std::size_t{2});
    CHECK(!lib.sync_with_folder({file("a.mp4", 10, 100), file("b.mp4", 20, 200)}));  // nothing new

    lib.set_media_info("a.mp4", 30.0, 1920, 1080);
    lib.set_favorite("a.mp4", true);
    CHECK_EQ(lib.needs_probe().size(), std::size_t{1});

    // Same file in other case (Windows ignores case): still the same clip, edits kept.
    CHECK(!lib.sync_with_folder({file("A.MP4", 10, 100), file("b.mp4", 20, 200)}));
    CHECK(lib.find("a.mp4")->favorite);

    // b deleted in Explorer, c appeared, a rewritten by another tool.
    CHECK(lib.sync_with_folder({file("a.mp4", 11, 100), file("c.mp4", 5, 300)}));
    CHECK(lib.find("b.mp4") == nullptr);
    CHECK(lib.find("c.mp4") != nullptr);
    CHECK_EQ(lib.find("a.mp4")->duration_s, 0.0);  // probe again
    CHECK(lib.find("a.mp4")->favorite);

    CHECK(lib.sync_with_folder({}));  // folder emptied
    CHECK(lib.clips().empty());
}

void saved_clip_merges_with_folder_sync() {
    Library lib;
    lib.sync_with_folder({file("new.mp4", 1, 5)});  // the watcher saw it first
    ClipEntry saved;
    saved.file = "new.mp4";
    saved.created = 50;
    saved.size_bytes = 41 * kMb;
    saved.duration_s = 30.2;
    saved.width = 1920;
    saved.height = 1080;
    saved.source = "Display 1";
    lib.add_saved_clip(saved);
    CHECK_EQ(lib.clips().size(), std::size_t{1});
    CHECK_EQ(lib.find("new.mp4")->duration_s, 30.2);
    CHECK_EQ(lib.find("new.mp4")->source, std::string("Display 1"));
    lib.add_saved_clip(ClipEntry{.file = "other.mp4"});
    CHECK_EQ(lib.clips().size(), std::size_t{2});
}

void media_info_is_sanitised() {
    Library lib;
    lib.sync_with_folder({file("a.mp4", 1, 1)});
    CHECK(lib.set_media_info("a.mp4", -5.0, -1, 720));
    CHECK_EQ(lib.find("a.mp4")->duration_s, 0.0);
    CHECK_EQ(lib.find("a.mp4")->width, 0);
    CHECK(lib.set_media_info("a.mp4", std::numeric_limits<double>::infinity(), 1, 1));
    CHECK_EQ(lib.find("a.mp4")->duration_s, 0.0);
    CHECK(!lib.set_media_info("missing.mp4", 1.0, 1, 1));
}

void views_and_counts() {
    Library lib = sample();
    lib.set_favorite("b.mp4", true);
    lib.move_to_trash("c.mp4", kNow);

    LibraryQuery q;
    q.now = kNow;
    CHECK_EQ(joined(names(lib, lib.query(q))), std::string("a.mp4,b.mp4"));  // newest first, no trash
    q.view = LibraryView::Favorites;
    CHECK_EQ(joined(names(lib, lib.query(q))), std::string("b.mp4"));
    q.view = LibraryView::Recent;
    CHECK_EQ(joined(names(lib, lib.query(q))), std::string("a.mp4"));
    q.view = LibraryView::Trash;
    CHECK_EQ(joined(names(lib, lib.query(q))), std::string("c.mp4"));

    const LibraryCounts c = lib.counts(kNow);
    CHECK_EQ(c.all, std::size_t{2});
    CHECK_EQ(c.favorites, std::size_t{1});
    CHECK_EQ(c.recent, std::size_t{1});
    CHECK_EQ(c.trash, std::size_t{1});
    CHECK_EQ(c.bytes, 120 * kMb);

    CHECK(lib.restore("c.mp4"));
    CHECK_EQ(lib.counts(kNow).trash, std::size_t{0});
    CHECK(!lib.move_to_trash("nope.mp4", kNow));
}

void sorting() {
    const Library lib = sample();
    LibraryQuery q;
    q.now = kNow;
    q.sort = ClipSort::Oldest;
    CHECK_EQ(joined(names(lib, lib.query(q))), std::string("c.mp4,b.mp4,a.mp4"));
    q.sort = ClipSort::Longest;
    CHECK_EQ(joined(names(lib, lib.query(q))), std::string("b.mp4,a.mp4,c.mp4"));
    q.sort = ClipSort::Shortest;
    CHECK_EQ(joined(names(lib, lib.query(q))), std::string("c.mp4,a.mp4,b.mp4"));
    q.sort = ClipSort::Largest;
    CHECK_EQ(joined(names(lib, lib.query(q))), std::string("b.mp4,a.mp4,c.mp4"));

    Library titled = sample();
    titled.set_title("c.mp4", "  Alpha  ");
    titled.set_title("a.mp4", "zebra");
    q.sort = ClipSort::Name;
    CHECK_EQ(joined(names(titled, titled.query(q))), std::string("c.mp4,b.mp4,a.mp4"));  // Alpha, b, zebra
    CHECK_EQ(display_title(*titled.find("c.mp4")), std::string("Alpha"));  // trimmed
}

void search_and_tags() {
    Library lib = sample();
    lib.set_title("a.mp4", "Checkout bug, repro");
    lib.set_notes("b.mp4", "The CRASH is gone");
    CHECK(lib.add_tag("c.mp4", " Funny "));
    CHECK(lib.add_tag("c.mp4", "funny"));  // duplicate ignoring case
    CHECK(lib.add_tag("c.mp4", "   "));  // empty, ignored
    CHECK(lib.add_tag("a.mp4", "work"));
    CHECK_EQ(lib.find("c.mp4")->tags.size(), std::size_t{1});
    CHECK_EQ(lib.find("c.mp4")->tags[0], std::string("Funny"));

    LibraryQuery q;
    q.now = kNow;
    q.search = "BUG";
    CHECK_EQ(joined(names(lib, lib.query(q))), std::string("a.mp4"));
    q.search = "crash";
    CHECK_EQ(joined(names(lib, lib.query(q))), std::string("b.mp4"));
    q.search = "fun";
    CHECK_EQ(joined(names(lib, lib.query(q))), std::string("c.mp4"));
    q.search = "c.mp4";  // the file name is searchable even with a title set
    CHECK_EQ(joined(names(lib, lib.query(q))), std::string("c.mp4"));
    q.search = "   ";
    CHECK_EQ(lib.query(q).size(), std::size_t{3});
    q.search = "";
    q.tag = "FUNNY";
    CHECK_EQ(joined(names(lib, lib.query(q))), std::string("c.mp4"));

    const std::vector<std::string> tags = lib.all_tags();
    CHECK_EQ(joined(tags), std::string("Funny,work"));
    CHECK(lib.remove_tag("c.mp4", "FUNNY"));
    CHECK(lib.find("c.mp4")->tags.empty());
    CHECK(contains_ignoring_case("caf\xC3\xA9 Clip", "CLIP"));
    CHECK(contains_ignoring_case("anything", ""));
}

void collections() {
    Library lib = sample();
    const std::string best = lib.create_collection("  Best of ", 2);
    const std::string work = lib.create_collection("Work", 1);
    CHECK_EQ(best, std::string("c1"));
    CHECK_EQ(work, std::string("c2"));
    CHECK_EQ(lib.collections()[0].name, std::string("Best of"));
    CHECK(lib.set_in_collection("a.mp4", best, true));
    CHECK(lib.set_in_collection("a.mp4", best, true));  // twice is once
    CHECK(lib.set_in_collection("b.mp4", best, true));
    CHECK(!lib.set_in_collection("b.mp4", "c99", true));
    CHECK(!lib.set_in_collection("nope.mp4", best, true));
    CHECK_EQ(lib.find("a.mp4")->collections.size(), std::size_t{1});
    CHECK_EQ(lib.collection_size(best), std::size_t{2});
    lib.move_to_trash("b.mp4", kNow);
    CHECK_EQ(lib.collection_size(best), std::size_t{1});
    CHECK(lib.find_collection(best) != nullptr && lib.find_collection(best)->name == "Best of");
    CHECK(lib.find_collection("c42") == nullptr);

    LibraryQuery q;
    q.now = kNow;
    q.view = LibraryView::Collection;
    q.collection = best;
    CHECK_EQ(joined(names(lib, lib.query(q))), std::string("a.mp4"));

    CHECK(lib.rename_collection(work, "Work demos"));
    CHECK(!lib.rename_collection("c9", "x"));
    CHECK(lib.delete_collection(best));
    CHECK(lib.find("a.mp4")->collections.empty());
    CHECK(!lib.delete_collection(best));
    // Ids are never reused, so an old reference can never point at a new collection.
    CHECK_EQ(lib.create_collection("New", 0), std::string("c3"));
}

void cleanup() {
    Library lib;
    lib.sync_with_folder({file("old.mp4", 10 * kMb, kNow - 100 * kDay), file("old_fav.mp4", 10 * kMb, kNow - 100 * kDay),
                          file("old_col.mp4", 10 * kMb, kNow - 100 * kDay), file("mid.mp4", 10 * kMb, kNow - 20 * kDay),
                          file("new.mp4", 10 * kMb, kNow - 1 * kDay), file("trash_old.mp4", 10 * kMb, kNow - 5 * kDay),
                          file("trash_new.mp4", 10 * kMb, kNow - 5 * kDay)});
    lib.set_favorite("old_fav.mp4", true);
    lib.set_in_collection("old_col.mp4", lib.create_collection("Keep", 0), true);
    lib.move_to_trash("trash_old.mp4", kNow - 31 * kDay);
    lib.move_to_trash("trash_new.mp4", kNow - 2 * kDay);

    // Default policy: only the Trash empties itself.
    CHECK_EQ(joined(lib.due_for_removal(CleanupPolicy{}, kNow)), std::string("trash_old.mp4"));

    CleanupPolicy by_age;
    by_age.delete_after_days = 30;
    CHECK_EQ(joined(lib.due_for_removal(by_age, kNow)), std::string("old.mp4,trash_old.mp4"));

    // 70 MB total, limit 35 MB: trash_old by age, then trash_new (Trash first), then oldest
    // unprotected: old (100 d), mid (20 d). Favorites and collection clips stay even over the limit.
    CleanupPolicy by_size;
    by_size.max_bytes = 35 * kMb;
    CHECK_EQ(joined(lib.due_for_removal(by_size, kNow)), std::string("trash_old.mp4,trash_new.mp4,old.mp4,mid.mp4"));

    by_size.max_bytes = 1;
    const std::vector<std::string> all = lib.due_for_removal(by_size, kNow);
    CHECK_EQ(all.size(), std::size_t{5});  // everything except the favorite and the collection clip
}

void json_round_trip() {
    Library lib = sample();
    const std::string col = lib.create_collection("Funny \"stuff\"", 3);
    lib.set_in_collection("a.mp4", col, true);
    lib.set_favorite("a.mp4", true);
    lib.set_title("a.mp4", "Caf\xC3\xA9 run");
    lib.set_notes("a.mp4", "line 1\nline 2");
    lib.add_tag("a.mp4", "speedrun");
    lib.move_to_trash("c.mp4", kNow);

    const std::string text = lib.to_json().dump(2);
    const JsonParseResult parsed = parse_json(text);
    CHECK(parsed.value.has_value());
    const Library back = Library::from_json(*parsed.value);
    CHECK_EQ(back.to_json().dump(2), text);
    const ClipEntry* a = back.find("a.mp4");
    CHECK(a != nullptr && a->favorite && a->title == "Caf\xC3\xA9 run" && a->notes == "line 1\nline 2");
    CHECK(a != nullptr && a->collections.size() == 1 && a->tags.size() == 1 && a->width == 1920);
    CHECK(back.find("c.mp4")->trashed_at.has_value());
    CHECK_EQ(Library(back).create_collection("x", 0), std::string("c2"));
}

void many_clips_sync() {
    // The sync runs every few seconds on the UI thread: it must stay fast with thousands of clips.
    std::vector<FolderFile> files;
    for (int i = 0; i < 20000; ++i) files.push_back(file("clip_" + std::to_string(i) + ".mp4", 1000 + i, i));
    Library lib;
    CHECK(lib.sync_with_folder(files));
    CHECK_EQ(lib.clips().size(), std::size_t{20000});
    files.erase(files.begin() + 5);
    files.push_back(file("CLIP_7.MP4", 1007, 7));  // the same file listed twice in other case
    CHECK(lib.sync_with_folder(files));
    CHECK_EQ(lib.clips().size(), std::size_t{19999});
    CHECK(lib.find("clip_5.mp4") == nullptr);
    CHECK(!lib.sync_with_folder(files));
}

void text_rules() {
    Library lib;
    lib.sync_with_folder({file("a.mp4", 1, 1), file("b.mp4", 1, 2)});
    lib.set_title("a.mp4", "\xC3\x89" "clair");  // capital E acute
    lib.set_title("b.mp4", "zebra");
    LibraryQuery q;
    q.search = "\xC3\xA9" "clair";  // small e acute
    CHECK(lib.query(q).empty());  // ASCII rules: no match
    // A stand-in for Windows' rules: map the capital to the small letter.
    set_text_rules(TextRules{
        [](std::string_view t) {
            std::string s(t);
            for (std::size_t i = 0; i + 1 < s.size(); ++i) {
                if (s[i] == '\xC3' && s[i + 1] == '\x89') s[i + 1] = '\xA9';
            }
            for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            return s;
        },
        [](std::string_view a, std::string_view b) {
            // Accented E sorts as E.
            const auto plain = [](std::string_view t) { return t.starts_with("\xC3\x89") ? "E" + std::string(t.substr(2)) : std::string(t); };
            const std::string x = plain(a), y = plain(b);
            return x < y ? -1 : x > y ? 1 : 0;
        }});
    CHECK_EQ(lib.query(q).size(), std::size_t{1});
    q.search.clear();
    q.sort = ClipSort::Name;
    CHECK_EQ(joined(names(lib, lib.query(q))), std::string("a.mp4,b.mp4"));
    set_text_rules(TextRules{});
}

void damaged_json_is_tolerated() {
    // Wrong types, duplicates, dangling collection ids, negative sizes: all dropped or defaulted.
    const JsonParseResult parsed = parse_json(R"({
        "next_collection": -5,
        "collections": [{"id": "c7", "name": "A"}, {"id": "c7", "name": "dup"}, {"name": "no id"}, 5],
        "clips": [
            {"file": "big.mp4", "size": 1e30},
            {"file": "a.mp4", "size": -10, "duration": "long", "width": 1e20, "tags": ["x", 3, "X", ""],
             "collections": ["c7", "c9", "c7"], "favorite": "yes", "trashed_at": "soon"},
            {"file": "A.MP4"},
            {"title": "no file"},
            "garbage"
        ],
        "future_key": {"anything": true}
    })");
    CHECK(parsed.value.has_value());
    Library lib = Library::from_json(*parsed.value);
    CHECK_EQ(lib.collections().size(), std::size_t{1});
    CHECK_EQ(lib.clips().size(), std::size_t{2});
    CHECK_EQ(lib.find("big.mp4")->size_bytes, kMaxClipBytes);
    const ClipEntry& a = *lib.find("a.mp4");
    CHECK_EQ(a.size_bytes, std::int64_t{0});
    CHECK_EQ(a.duration_s, 0.0);
    CHECK_EQ(a.width, 100'000);
    CHECK_EQ(joined(a.tags), std::string("x"));
    CHECK_EQ(joined(a.collections), std::string("c7"));
    CHECK(!a.favorite);
    CHECK(!a.trashed_at.has_value());
    CHECK_EQ(lib.create_collection("next", 0), std::string("c8"));  // past the highest id seen

    // Not an object at all: an empty library, not a crash.
    CHECK(Library::from_json(Json(5)).clips().empty());
    CHECK(Library::from_json(Json()).collections().empty());
}

void formatting() {
    CHECK_EQ(format_duration(30.0), std::string("0:30"));
    CHECK_EQ(format_duration(29.6), std::string("0:30"));
    CHECK_EQ(format_duration(65.0), std::string("1:05"));
    CHECK_EQ(format_duration(3723.0), std::string("1:02:03"));
    CHECK_EQ(format_duration(-1.0), std::string("0:00"));
    CHECK_EQ(format_duration(std::nan("")), std::string("0:00"));
    CHECK_EQ(format_size(41 * kMb), std::string("41 MB"));
    CHECK_EQ(format_size(1'240'000'000), std::string("1.2 GB"));
    CHECK_EQ(format_size(820'000), std::string("820 KB"));
    CHECK_EQ(format_size(-5), std::string("0 KB"));
    CHECK_EQ(display_title(ClipEntry{.file = "clip_1.mp4"}), std::string("clip_1"));
    CHECK_EQ(display_title(ClipEntry{.file = "noext"}), std::string("noext"));
    CHECK_EQ(display_title(ClipEntry{.file = ".mp4"}), std::string(".mp4"));

    CHECK_EQ(format_age(-30), std::string("Just now"));
    CHECK_EQ(format_age(59), std::string("Just now"));
    CHECK_EQ(format_age(60), std::string("1m ago"));
    CHECK_EQ(format_age(36 * 60), std::string("36m ago"));
    CHECK_EQ(format_age(3600), std::string("1h ago"));
    CHECK_EQ(format_age(kDay - 1), std::string("23h ago"));
    CHECK_EQ(format_age(kDay), std::string("1d ago"));
    CHECK_EQ(format_age(13 * kDay), std::string("13d ago"));
    CHECK_EQ(format_age(14 * kDay), std::string("2w ago"));
    CHECK_EQ(format_age(60 * kDay), std::string("2mo ago"));
    CHECK_EQ(format_age(400 * kDay), std::string("1y ago"));

    CHECK_EQ(format_resolution(1920, 1080), std::string("1080p"));
    CHECK_EQ(format_resolution(2560, 1440), std::string("1440p"));
    CHECK_EQ(format_resolution(3840, 2160), std::string("4K"));
    CHECK_EQ(format_resolution(1280, 720), std::string("720p"));
    CHECK_EQ(format_resolution(1024, 600), std::string("1024x600"));
    CHECK_EQ(format_resolution(1440, 1080), std::string("1440x1080"));  // 4:3 is not "1080p"
    CHECK_EQ(format_resolution(0, 1080), std::string());
    CHECK_EQ(format_resolution(2'000'000'000, 2'000'000'000), std::string("2000000000x2000000000"));  // no overflow
}

}  // namespace

bool same_marks(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::abs(a[i] - b[i]) > 1e-9) return false;
    }
    return true;
}

void bookmarks() {
    // Cleaning: sorted, NaN and out-of-range gone, a double press merged.
    const std::vector<double> clean =
        clean_bookmarks({5.0, std::nan(""), -1.0, 2.0, 2.04, 31.0, 30.0}, 30.0);
    CHECK(clean == (std::vector<double>{2.0, 5.0, 30.0}));
    CHECK(clean_bookmarks({100.0}, 0.0).size() == 1);  // length unknown: no upper bound
    std::vector<double> many;
    for (int i = 0; i < 1000; ++i) many.push_back(i);
    CHECK_EQ(clean_bookmarks(many, 0.0).size(), kMaxBookmarks);

    // A saved clip covering 100..130 s of the buffer gets the marks inside it as offsets.
    CHECK(same_marks(bookmarks_within({90.0, 101.5, 129.9, 130.0, 140.0}, 100.0, 30.0), {1.5, 29.9}));
    CHECK(bookmarks_within({}, 0.0, 30.0).empty());
    // A trim starting at 4 s (its keyframe) keeps 5 s as 1 s.
    CHECK(bookmarks_within({1.0, 5.0}, 4.0, 10.0) == (std::vector<double>{1.0}));

    Library lib = sample();
    CHECK(lib.set_bookmarks("a.mp4", {3.0, 1.0}));
    CHECK(!lib.set_bookmarks("nope.mp4", {1.0}));
    CHECK(lib.find("a.mp4")->bookmarks == (std::vector<double>{1.0, 3.0}));
    // Kept through library.json; a damaged entry is dropped, not the clip.
    const Library back = Library::from_json(*parse_json(lib.to_json().dump()).value);
    CHECK(back.find("a.mp4")->bookmarks == (std::vector<double>{1.0, 3.0}));
    const Library damaged =
        Library::from_json(*parse_json(R"({"clips":[{"file":"x.mp4","duration":10,"bookmarks":[1,"two",50,null,2]}]})").value);
    CHECK(damaged.find("x.mp4") != nullptr && damaged.find("x.mp4")->bookmarks == (std::vector<double>{1.0, 2.0}));
    // A clip without bookmarks writes no "bookmarks" key (older versions read the file unchanged).
    CHECK(sample().to_json().dump().find("bookmarks") == std::string::npos);
    // The recorder's saved clip carries them in, also when a folder sync saw the file first.
    Library sync;
    sync.sync_with_folder({FolderFile{"new.mp4", 10, kNow}});
    ClipEntry saved;
    saved.file = "new.mp4";
    saved.bookmarks = {2.5};
    sync.add_saved_clip(saved);
    CHECK(sync.find("new.mp4")->bookmarks == (std::vector<double>{2.5}));
}

int main() {
    bookmarks();
    folder_sync();
    saved_clip_merges_with_folder_sync();
    media_info_is_sanitised();
    views_and_counts();
    sorting();
    search_and_tags();
    collections();
    cleanup();
    json_round_trip();
    damaged_json_is_tolerated();
    many_clips_sync();
    text_rules();
    formatting();
    return bt_test::finish("LibraryTests");
}
