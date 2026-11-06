#include "core/Mp4.h"

#include "Check.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace bt;

namespace {

using Bytes = std::vector<std::uint8_t>;

void put32(Bytes& b, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) b.push_back(static_cast<std::uint8_t>(v >> shift));
}

void put64(Bytes& b, std::uint64_t v) {
    put32(b, static_cast<std::uint32_t>(v >> 32));
    put32(b, static_cast<std::uint32_t>(v));
}

Bytes box(const std::string& type, const Bytes& body) {
    Bytes b;
    put32(b, static_cast<std::uint32_t>(8 + body.size()));
    b.insert(b.end(), type.begin(), type.end());
    b.insert(b.end(), body.begin(), body.end());
    return b;
}

Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

Bytes stco(std::initializer_list<std::uint32_t> offsets) {
    Bytes body;
    put32(body, 0);  // version + flags
    put32(body, static_cast<std::uint32_t>(offsets.size()));
    for (const std::uint32_t o : offsets) put32(body, o);
    return box("stco", body);
}

Bytes co64(std::initializer_list<std::uint64_t> offsets) {
    Bytes body;
    put32(body, 0);
    put32(body, static_cast<std::uint32_t>(offsets.size()));
    for (const std::uint64_t o : offsets) put64(body, o);
    return box("co64", body);
}

// moov > trak > mdia > minf > stbl > (stsd, chunk table), plus an mvhd and a udta that must be
// copied unchanged.
Bytes moov_with(const Bytes& video_table, const Bytes& audio_table) {
    auto track = [](const Bytes& table) {
        const Bytes stbl = box("stbl", concat({box("stsd", Bytes(12, 0x11)), table}));
        return box("trak", concat({box("tkhd", Bytes(20, 0x22)), box("mdia", box("minf", stbl))}));
    };
    return box("moov", concat({box("mvhd", Bytes(100, 0x33)), track(video_table), track(audio_table),
                               box("udta", Bytes(9, 0x44))}));
}

std::uint32_t read32_at(const Bytes& b, std::size_t at) {
    return (std::uint32_t{b[at]} << 24) | (std::uint32_t{b[at + 1]} << 16) | (std::uint32_t{b[at + 2]} << 8) | b[at + 3];
}

// Position of the first entry of the n-th table with this type.
std::size_t find_table(const Bytes& moov, const std::string& type, int nth) {
    for (std::size_t i = 4; i + 4 <= moov.size(); ++i) {
        if (std::string(moov.begin() + static_cast<std::ptrdiff_t>(i), moov.begin() + static_cast<std::ptrdiff_t>(i) + 4) == type && nth-- == 0) {
            return i + 4 + 4 + 4;  // type, version/flags, entry count
        }
    }
    return 0;
}

void shifts_every_chunk_offset() {
    const Bytes moov = moov_with(stco({48, 1000, 250000}), stco({500, 600}));
    const auto moved = mp4::shift_chunk_offsets(moov, moov.size());
    CHECK(moved.has_value());
    CHECK_EQ(moved->size(), moov.size());
    const std::size_t video = find_table(*moved, "stco", 0);
    const std::size_t audio = find_table(*moved, "stco", 1);
    const auto shift = static_cast<std::uint32_t>(moov.size());
    CHECK_EQ(read32_at(*moved, video), 48 + shift);
    CHECK_EQ(read32_at(*moved, video + 4), 1000 + shift);
    CHECK_EQ(read32_at(*moved, video + 8), 250000 + shift);
    CHECK_EQ(read32_at(*moved, audio), 500 + shift);
    CHECK_EQ(read32_at(*moved, audio + 4), 600 + shift);
    // Everything else is byte for byte the same.
    std::size_t differences = 0;
    for (std::size_t i = 0; i < moov.size(); ++i) {
        if (moov[i] != (*moved)[i]) ++differences;
    }
    CHECK(differences <= 5 * 4);  // only the five offsets changed
}

void wide_tables_and_overflow() {
    const Bytes wide = moov_with(co64({0x1'0000'0000ull, 42}), stco({7}));
    const auto moved = mp4::shift_chunk_offsets(wide, 100);
    CHECK(moved.has_value());
    const std::size_t entry = find_table(*moved, "co64", 0);
    CHECK_EQ(read32_at(*moved, entry), 1u);  // high word of 0x1'0000'0064
    CHECK_EQ(read32_at(*moved, entry + 4), 100u);

    // A 32-bit offset that would pass 4 GB: refused, the caller keeps the moov at the end.
    const Bytes near_limit = moov_with(stco({0xFFFF'FF00u}), stco({1}));
    CHECK(!mp4::shift_chunk_offsets(near_limit, 0x1000).has_value());
}

void refuses_what_it_does_not_understand() {
    CHECK(!mp4::shift_chunk_offsets(Bytes{}, 10).has_value());
    CHECK(!mp4::shift_chunk_offsets(box("free", Bytes(4, 0)), 10).has_value());  // not a moov
    // A chunk table claiming more entries than it holds.
    Bytes broken_body;
    put32(broken_body, 0);
    put32(broken_body, 1000);
    put32(broken_body, 1);
    CHECK(!mp4::shift_chunk_offsets(moov_with(box("stco", broken_body), stco({1})), 10).has_value());
    // A child box that runs past its parent.
    Bytes moov = moov_with(stco({1}), stco({2}));
    moov[8 + 3] = 0xFF;  // mvhd size byte made huge
    CHECK(!mp4::shift_chunk_offsets(moov, 10).has_value());
}

void box_headers() {
    const Bytes file = concat({box("ftyp", Bytes(16, 0)), box("mdat", Bytes(100, 0))});
    const auto ftyp = mp4::read_box_header(file, 0, file.size());
    CHECK(ftyp && ftyp->type == "ftyp" && ftyp->size == 24);
    const auto mdat = mp4::read_box_header(file, 24, file.size());
    CHECK(mdat && mdat->type == "mdat" && mdat->size == 108);
    CHECK(!mp4::read_box_header(file, file.size() - 4, file.size()).has_value());

    // 64-bit size.
    Bytes large;
    put32(large, 1);
    large.insert(large.end(), {'m', 'd', 'a', 't'});
    put64(large, 0x1'2345'6789ull);
    const auto big = mp4::read_box_header(large, 0, 0x2'0000'0000ull);
    CHECK(big && big->size == 0x1'2345'6789ull);

    // Size 0: to the end of the file.
    Bytes open_ended;
    put32(open_ended, 0);
    open_ended.insert(open_ended.end(), {'m', 'd', 'a', 't'});
    const auto rest = mp4::read_box_header(open_ended, 0, 5000);
    CHECK(rest && rest->size == 5000);

    // Sizes smaller than a header are invalid.
    Bytes tiny;
    put32(tiny, 4);
    tiny.insert(tiny.end(), {'f', 'r', 'e', 'e'});
    CHECK(!mp4::read_box_header(tiny, 0, 8).has_value());
}

// A track as the MP4 sink writes it: tkhd (version 0 or 1), mdia > hdlr + minf > stbl > stco.
Bytes real_track(const std::string& handler, const std::string& name, std::uint32_t chunk, std::uint8_t version = 0) {
    Bytes tkhd;
    tkhd.push_back(version);
    tkhd.insert(tkhd.end(), {0x00, 0x00, 0x03});  // flags: enabled | in movie
    tkhd.insert(tkhd.end(), version == 1 ? 32 : 20, 0x55);  // times, track id, reserved, duration
    tkhd.insert(tkhd.end(), 8, 0x00);  // reserved
    tkhd.insert(tkhd.end(), {0x00, 0x00});  // layer
    tkhd.insert(tkhd.end(), {0x00, 0x00});  // alternate group
    tkhd.insert(tkhd.end(), 2 + 2 + 36 + 8, 0x66);  // volume, reserved, matrix, width, height
    Bytes hdlr(4, 0);  // version/flags
    put32(hdlr, 0);  // pre_defined
    hdlr.insert(hdlr.end(), handler.begin(), handler.end());
    hdlr.insert(hdlr.end(), 12, 0);
    hdlr.insert(hdlr.end(), name.begin(), name.end());
    hdlr.push_back(0);
    const Bytes stbl = box("stbl", concat({box("stsd", Bytes(12, 0x11)), stco({chunk})}));
    return box("trak", concat({box("tkhd", tkhd), box("edts", Bytes(5, 0x77)),
                               box("mdia", concat({box("mdhd", Bytes(24, 0x88)), box("hdlr", hdlr), box("minf", stbl)}))}));
}

Bytes separate_tracks_moov() {
    return box("moov", concat({box("mvhd", Bytes(100, 0x33)), real_track("vide", "VideoHandler", 48),
                               real_track("soun", "SoundHandler", 1000), real_track("soun", "SoundHandler", 2000),
                               real_track("soun", "SoundHandler", 3000, 1), box("udta", Bytes(9, 0x44))}));
}

// The n-th box of this type anywhere in the bytes (a plain search, good enough for test data).
std::size_t find_box(const Bytes& bytes, const std::string& type, int nth) {
    for (std::size_t i = 4; i + 4 <= bytes.size(); ++i) {
        if (std::string(bytes.begin() + static_cast<std::ptrdiff_t>(i), bytes.begin() + static_cast<std::ptrdiff_t>(i) + 4) == type && nth-- == 0) {
            return i - 4;
        }
    }
    return 0;
}

void shifts_every_track_of_a_clip_with_separate_tracks() {
    // Video plus three audio tracks: every trak's table moves, none is missed after the first two.
    const Bytes moov = separate_tracks_moov();
    const auto moved = mp4::shift_chunk_offsets(moov, 5000);
    CHECK(moved.has_value());
    CHECK_EQ(read32_at(*moved, find_table(*moved, "stco", 0)), 48u + 5000);
    CHECK_EQ(read32_at(*moved, find_table(*moved, "stco", 1)), 1000u + 5000);
    CHECK_EQ(read32_at(*moved, find_table(*moved, "stco", 2)), 2000u + 5000);
    CHECK_EQ(read32_at(*moved, find_table(*moved, "stco", 3)), 3000u + 5000);
}

std::vector<std::string> names_of(const Bytes& moov) {
    std::vector<std::string> names;
    for (const mp4::AudioTrackInfo& track : mp4::audio_tracks(moov)) names.push_back(track.name);
    return names;
}

void labels_separate_audio_tracks() {
    const Bytes moov = separate_tracks_moov();
    const std::vector<std::string> names = {"Game + mic", "Game", "Mic"};
    const auto labeled = mp4::label_audio_tracks(moov, names);
    CHECK(labeled.has_value());
    const Bytes& out = *labeled;
    // Still one well-formed moov whose size adds up, and the names read back in order.
    CHECK_EQ(read32_at(out, 0), static_cast<std::uint32_t>(out.size()));
    CHECK(names_of(out) == names);
    CHECK(names_of(moov) == (std::vector<std::string>{"SoundHandler", "SoundHandler", "SoundHandler"}));
    // Each track's stsd comes back whole (what Media Foundation hands out per stream).
    const auto tracks = mp4::audio_tracks(out);
    CHECK_EQ(tracks.size(), std::size_t{3});
    CHECK(tracks[0].sample_description == box("stsd", Bytes(12, 0x11)));
    // tkhd: video untouched; audio in alternate group 1, only the first enabled. Version 1 too.
    const auto flags = [&](int nth) { return out[find_box(out, "tkhd", nth) + 8 + 3]; };
    const auto group = [&](int nth, std::size_t at) {
        const std::size_t g = find_box(out, "tkhd", nth) + 8 + at;
        return static_cast<int>(out[g] << 8 | out[g + 1]);
    };
    CHECK_EQ(flags(0), std::uint8_t{3});
    CHECK_EQ(group(0, 34), 0);
    CHECK_EQ(flags(1), std::uint8_t{3});
    CHECK_EQ(group(1, 34), 1);
    CHECK_EQ(flags(2), std::uint8_t{2});
    CHECK_EQ(group(2, 34), 1);
    CHECK_EQ(flags(3), std::uint8_t{2});
    CHECK_EQ(group(3, 46), 1);
    // The chunk tables survive the rebuild, and the labeled box can still be moved to the front.
    const auto moved = mp4::shift_chunk_offsets(out, out.size());
    CHECK(moved.has_value());
    CHECK_EQ(read32_at(*moved, find_table(*moved, "stco", 3)), 3000u + static_cast<std::uint32_t>(out.size()));
    // Boxes around the tracks are kept byte for byte.
    CHECK(std::equal(moov.begin() + 8, moov.begin() + 116, out.begin() + 8));  // mvhd
    const std::size_t udta = find_box(out, "udta", 0);
    CHECK(udta + 17 == out.size());

    // No names: same size, only the flags and groups change (the in-place fallback).
    const auto unnamed = mp4::label_audio_tracks(moov, {});
    CHECK(unnamed && unnamed->size() == moov.size());
    CHECK(names_of(*unnamed) == names_of(moov));
    const auto unnamed_flags = [&](int nth) { return (*unnamed)[find_box(*unnamed, "tkhd", nth) + 8 + 3]; };
    const auto unnamed_group = [&](int nth, std::size_t at) {
        const std::size_t g = find_box(*unnamed, "tkhd", nth) + 8 + at;
        return static_cast<int>((*unnamed)[g] << 8 | (*unnamed)[g + 1]);
    };
    CHECK_EQ(unnamed_flags(1), std::uint8_t{3});
    CHECK_EQ(unnamed_flags(2), std::uint8_t{2});
    CHECK_EQ(unnamed_group(2, 34), 1);
    CHECK_EQ(unnamed_flags(3), std::uint8_t{2});
    CHECK_EQ(unnamed_group(3, 46), 1);

    // One audio track (separate tracks off): nothing changes at all.
    const Bytes single = box("moov", concat({box("mvhd", Bytes(100, 0x33)), real_track("vide", "VideoHandler", 48),
                                             real_track("soun", "SoundHandler", 1000)}));
    const auto same = mp4::label_audio_tracks(single, names);
    CHECK(same && *same == single);

    // Damaged input is refused, never half rewritten.
    Bytes broken = moov;
    broken[find_box(broken, "hdlr", 2) + 3] = 0xFF;  // a size past its parent
    CHECK(!mp4::label_audio_tracks(broken, names).has_value());
    CHECK(!mp4::label_audio_tracks(Bytes{}, names).has_value());
    CHECK(mp4::audio_tracks(Bytes{}).empty());
    CHECK(mp4::audio_tracks(broken).empty());
    // A tkhd version this code does not know.
    Bytes v2 = moov;
    v2[find_box(v2, "tkhd", 2) + 8] = 2;
    CHECK(!mp4::label_audio_tracks(v2, names).has_value());
    // Every shortened copy (with its size field fixed up, the worst case) is refused or read
    // without going past its end: never a crash. A cut right after a whole track is a valid
    // moov with fewer tracks, so a result is fine as long as it is well formed.
    bool all_safe = true;
    for (std::size_t length = 0; length < moov.size(); ++length) {
        Bytes cut(moov.begin(), moov.begin() + static_cast<std::ptrdiff_t>(length));
        if (cut.size() >= 4) {
            cut[0] = static_cast<std::uint8_t>(length >> 24);
            cut[1] = static_cast<std::uint8_t>(length >> 16);
            cut[2] = static_cast<std::uint8_t>(length >> 8);
            cut[3] = static_cast<std::uint8_t>(length);
        }
        const auto labeled_cut = mp4::label_audio_tracks(cut, names);
        all_safe = all_safe && (!labeled_cut || (labeled_cut->size() >= 8 && read32_at(*labeled_cut, 0) == labeled_cut->size()));
        all_safe = all_safe && mp4::audio_tracks(cut).size() <= 3;
    }
    CHECK(all_safe);
}

}  // namespace

int main() {
    shifts_every_chunk_offset();
    wide_tables_and_overflow();
    refuses_what_it_does_not_understand();
    box_headers();
    shifts_every_track_of_a_clip_with_separate_tracks();
    labels_separate_audio_tracks();
    return bt_test::finish("Mp4Tests");
}
