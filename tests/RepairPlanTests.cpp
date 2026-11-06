#include "core/Mp4.h"
#include "core/RepairPlan.h"

#include "Check.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace bt;

namespace {

using Bytes = std::vector<std::uint8_t>;

void put16(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}

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

// esds as FFmpeg and Medal write it, with the given objectTypeIndication.
Bytes esds(std::uint8_t object_type) {
    Bytes config = {object_type, 0x15, 0, 0, 0};  // stream type audio, buffer size
    put32(config, 192000);  // max bitrate
    put32(config, 192000);  // average bitrate
    config.insert(config.end(), {0x05, 0x02, 0x11, 0x90});  // AudioSpecificConfig: AAC LC, 48 kHz, stereo
    Bytes es = {0x00, 0x01, 0x00};  // ES_ID 1, no flags
    es.push_back(0x04);
    es.push_back(static_cast<std::uint8_t>(config.size()));
    es.insert(es.end(), config.begin(), config.end());
    es.insert(es.end(), {0x06, 0x01, 0x02});  // SLConfig
    Bytes body(4, 0);  // version + flags
    body.push_back(0x03);
    body.push_back(static_cast<std::uint8_t>(es.size()));
    body.insert(body.end(), es.begin(), es.end());
    return box("esds", body);
}

Bytes mp4a(std::uint16_t channels, std::uint32_t rate, std::uint8_t object_type) {
    Bytes body(6, 0);
    put16(body, 1);  // data reference index
    body.insert(body.end(), 8, 0);  // version, revision, vendor
    put16(body, channels);
    put16(body, 16);  // sample size
    put32(body, 0);  // pre-defined, reserved
    put32(body, rate << 16);
    const Bytes e = esds(object_type);
    body.insert(body.end(), e.begin(), e.end());
    return box("mp4a", body);
}

Bytes stsd(const Bytes& entry) {
    Bytes body(4, 0);
    put32(body, 1);
    body.insert(body.end(), entry.begin(), entry.end());
    return box("stsd", body);
}

Bytes stsz(std::uint32_t count) {
    Bytes body(4, 0);
    put32(body, 0);  // every sample has its own size
    put32(body, count);
    return box("stsz", body);
}

struct Edit {
    std::int64_t duration;
    std::int64_t media_time;
    std::int16_t rate = 1;
};

Bytes elst(std::initializer_list<Edit> edits, bool wide = false) {
    Bytes body = {static_cast<std::uint8_t>(wide ? 1 : 0), 0, 0, 0};
    put32(body, static_cast<std::uint32_t>(edits.size()));
    for (const Edit& e : edits) {
        if (wide) {
            put64(body, static_cast<std::uint64_t>(e.duration));
            put64(body, static_cast<std::uint64_t>(e.media_time));
        } else {
            put32(body, static_cast<std::uint32_t>(e.duration));
            put32(body, static_cast<std::uint32_t>(e.media_time));
        }
        put16(body, static_cast<std::uint16_t>(e.rate));
        put16(body, 0);
    }
    return box("edts", box("elst", body));
}

struct TrackSpec {
    std::string handler = "soun";
    std::string name;
    Bytes entry;  // the sample entry
    std::uint32_t timescale = 48000;
    std::uint32_t samples = 100;
    std::uint32_t duration = 0;  // mdhd, in the timescale
    bool enabled = true;
    bool ctts = false;
    Bytes edts;  // empty: no edit list
    Bytes tables;  // sample tables (stsz, stsc, stco...) in place of a count-only stsz
};

Bytes track(const TrackSpec& spec) {
    Bytes tkhd = {0x00, 0x00, 0x00, static_cast<std::uint8_t>(spec.enabled ? 0x03 : 0x02)};
    tkhd.insert(tkhd.end(), 20 + 8 + 2 + 2 + 2 + 2 + 36 + 8, 0);
    Bytes mdhd(4, 0);
    put32(mdhd, 0);
    put32(mdhd, 0);
    put32(mdhd, spec.timescale);
    put32(mdhd, spec.duration);
    put32(mdhd, 0);
    Bytes hdlr(4, 0);
    put32(hdlr, 0);
    hdlr.insert(hdlr.end(), spec.handler.begin(), spec.handler.end());
    hdlr.insert(hdlr.end(), 12, 0);
    hdlr.insert(hdlr.end(), spec.name.begin(), spec.name.end());
    hdlr.push_back(0);
    Bytes stbl_body = concat({stsd(spec.entry), box("stts", Bytes(8, 0)), spec.tables.empty() ? stsz(spec.samples) : spec.tables});
    if (spec.ctts) {
        stbl_body = concat({stbl_body, box("ctts", Bytes(8, 0))});
    }
    const Bytes minf = box("minf", box("stbl", stbl_body));
    return box("trak", concat({box("tkhd", tkhd), spec.edts,
                               box("mdia", concat({box("mdhd", mdhd), box("hdlr", hdlr), minf}))}));
}

Bytes mvhd(std::uint32_t timescale) {
    Bytes body(4, 0);
    put32(body, 0);
    put32(body, 0);
    put32(body, timescale);
    body.resize(100, 0);
    return box("mvhd", body);
}

// An H.264 sample entry with an avcC declaring NAL lengths of length_minus_one + 1 bytes.
Bytes avc_entry(const std::string& type, std::uint8_t length_minus_one) {
    const Bytes avcc = {0x01, 0x4D, 0x00, 0x2A, static_cast<std::uint8_t>(0xFC | length_minus_one), 0xE0, 0x00};
    return box(type, concat({Bytes(78, 0), box("avcC", avcc)}));
}

TrackSpec video_track() {
    TrackSpec v;
    v.handler = "vide";
    v.name = "VideoHandler";
    v.entry = avc_entry("avc1", 3);
    v.timescale = 90000;
    v.samples = 600;
    return v;
}

TrackSpec audio_track(const std::string& name, bool enabled = true) {
    TrackSpec a;
    a.name = name;
    a.entry = mp4a(2, 48000, 0x40);
    a.enabled = enabled;
    return a;
}

Bytes moov_of(std::initializer_list<TrackSpec> tracks, const Bytes& extra = {}) {
    Bytes body = mvhd(1000);
    for (const TrackSpec& t : tracks) {
        const Bytes trak = track(t);
        body.insert(body.end(), trak.begin(), trak.end());
    }
    body.insert(body.end(), extra.begin(), extra.end());
    return box("moov", body);
}

// A Medal clip as measured: video, then "Game" (default) and "Mic" (not default).
Bytes medal_moov() {
    return moov_of({video_track(), audio_track("Game"), audio_track("Mic", false)}, box("udta", Bytes(20, 0x44)));
}

RepairCheck check_of(const Bytes& moov) {
    const auto tracks = mp4::tracks(moov);
    return tracks ? decide_repair(*tracks) : RepairCheck{false, -1, "unreadable"};
}

void reads_the_tracks_of_a_medal_clip() {
    const auto tracks = mp4::tracks(medal_moov());
    CHECK(tracks.has_value());
    CHECK_EQ(tracks->size(), std::size_t{3});
    const mp4::TrackInfo& video = (*tracks)[0];
    CHECK_EQ(video.handler, std::string("vide"));
    CHECK_EQ(video.codec, std::string("avc1"));
    CHECK_EQ(video.timescale, 90000u);
    CHECK_EQ(video.samples, 600u);
    CHECK(!video.reordered);
    const mp4::TrackInfo& game = (*tracks)[1];
    CHECK_EQ(game.handler, std::string("soun"));
    CHECK_EQ(game.name, std::string("Game"));
    CHECK_EQ(game.codec, std::string("mp4a"));
    CHECK(game.enabled);
    CHECK_EQ(game.sample_rate, 48000u);
    CHECK_EQ(game.channels, std::uint16_t{2});
    CHECK_EQ(game.object_type, std::uint8_t{0x40});
    CHECK_EQ(game.edit_shift, std::int64_t{0});
    CHECK(game.edits_understood);
    CHECK(!game.sample_description.empty());
    CHECK_EQ(std::string(game.sample_description.begin() + 4, game.sample_description.begin() + 8), std::string("stsd"));
    const mp4::TrackInfo& mic = (*tracks)[2];
    CHECK_EQ(mic.name, std::string("Mic"));
    CHECK(!mic.enabled);

    // A ctts box: frames stored out of display order.
    TrackSpec b_frames = video_track();
    b_frames.ctts = true;
    const auto reordered = mp4::tracks(moov_of({b_frames, audio_track("Game")}));
    CHECK(reordered && (*reordered)[0].reordered);
}

void reads_edit_lists() {
    const auto shift_of = [](const Bytes& edts, std::uint32_t timescale = 48000) {
        TrackSpec a = audio_track("Mic");
        a.edts = edts;
        a.timescale = timescale;
        const auto tracks = mp4::tracks(moov_of({video_track(), a}));
        return tracks ? std::optional<mp4::TrackInfo>((*tracks)[1]) : std::nullopt;
    };
    // Medal: one piece from media time 0. No shift.
    auto t = shift_of(elst({{105000, 0}}));
    CHECK(t && t->edits_understood && t->edit_shift == 0);
    // FFmpeg, a track that starts 2.978667 s after the others (movie timescale 1000): an empty edit first.
    t = shift_of(elst({{2979, -1}, {5021, 0}}));
    CHECK(t && t->edits_understood);
    CHECK_EQ(t->edit_shift, std::int64_t{29'790'000});
    // AAC priming: 1024 frames at 44.1 kHz skipped, 23.2 ms earlier.
    t = shift_of(elst({{15000, 1024}}), 44100);
    CHECK(t && t->edits_understood);
    CHECK_EQ(t->edit_shift, -std::int64_t{1024} * 10'000'000 / 44100);
    // Version 1 (64-bit) entries read the same.
    t = shift_of(elst({{2979, -1}, {5021, 0}}, true));
    CHECK(t && t->edits_understood && t->edit_shift == 29'790'000);
    // Two pieces (cut and joined), another speed, or a pause: not understood.
    t = shift_of(elst({{1000, 0}, {1000, 96000}}));
    CHECK(t && !t->edits_understood);
    t = shift_of(elst({{1000, 0, 2}}));
    CHECK(t && !t->edits_understood);
    t = shift_of(elst({{1000, 0, 0}}));
    CHECK(t && !t->edits_understood);
    // Absurd durations are refused rather than overflowing.
    t = shift_of(elst({{0x7FFF'FFFFll, -1}, {0x7FFF'FFFFll, -1}, {0x7FFF'FFFFll, -1}, {1, 0}}, true));  // 74 days
    CHECK(t && t->edits_understood && t->edit_shift == 3 * 0x7FFF'FFFFll * 10'000);
    t = shift_of(elst({{0x7FFF'FFFFll, -1}, {0x7FFF'FFFFll, -1}, {0x7FFF'FFFFll, -1}, {0x7FFF'FFFFll, -1},
                       {0x7FFF'FFFFll, -1}, {1, 0}}, true));  // 124 days: past the limit
    CHECK(t && !t->edits_understood);
    t = shift_of(elst({{std::numeric_limits<std::int64_t>::max(), -1}, {1, 0}}, true));
    CHECK(t && !t->edits_understood);
    // An entry count larger than the box: not understood, never read past it.
    Bytes broken = elst({{1000, 0}});
    broken[8 + 8 + 4 + 3] = 50;  // edts header, elst header, version/flags, count's low byte
    t = shift_of(broken);
    CHECK(t && !t->edits_understood);
    // A movie timescale of 0 with an empty edit: not understood (no division by zero).
    TrackSpec late = audio_track("Mic");
    late.edts = elst({{100, -1}, {100, 0}});
    Bytes body = mvhd(0);
    for (const Bytes& trak : {track(video_track()), track(late)}) body.insert(body.end(), trak.begin(), trak.end());
    const auto zero = mp4::tracks(box("moov", body));
    CHECK(zero && !(*zero)[1].edits_understood);
}

void reads_what_the_sound_really_is() {
    TrackSpec mp3 = audio_track("Mic");
    mp3.entry = mp4a(2, 44100, 0x6B);
    const auto tracks = mp4::tracks(moov_of({video_track(), audio_track("Game"), mp3}));
    CHECK(tracks && (*tracks)[2].object_type == 0x6B && (*tracks)[2].sample_rate == 44100);
    CHECK(!check_of(moov_of({video_track(), audio_track("Game"), mp3})).needed);
    CHECK(bt_test::contains(check_of(moov_of({video_track(), audio_track("Game"), mp3})).reason, "not AAC"));
    // MPEG-2 AAC is AAC.
    TrackSpec mpeg2 = audio_track("Mic");
    mpeg2.entry = mp4a(1, 48000, 0x67);
    CHECK(check_of(moov_of({video_track(), audio_track("Game"), mpeg2})).needed);
    // An mp4a entry without an esds counts as AAC (the decoder will tell).
    TrackSpec bare = audio_track("Mic");
    Bytes entry(6, 0);
    put16(entry, 1);
    entry.insert(entry.end(), 20, 0);
    bare.entry = box("mp4a", entry);
    const auto bare_tracks = mp4::tracks(moov_of({video_track(), audio_track("Game"), bare}));
    CHECK(bare_tracks && (*bare_tracks)[2].object_type == 0 && (*bare_tracks)[2].sample_rate == 0);
    CHECK(check_of(moov_of({video_track(), audio_track("Game"), bare})).needed);
}

void decides_which_clips_need_a_repair() {
    // Medal: needed, and the reason names both tracks.
    RepairCheck c = check_of(medal_moov());
    CHECK(c.needed);
    CHECK_EQ(c.audio_tracks, 2);
    CHECK_EQ(c.reason, std::string("Discord and most players only play the first sound track (\"Game\"), so the second (\"Mic\") is not heard."));
    // Three tracks.
    c = check_of(moov_of({video_track(), audio_track("Game"), audio_track("Mic"), audio_track("Chat")}));
    CHECK(c.needed);
    CHECK_EQ(c.audio_tracks, 3);
    CHECK(bt_test::contains(c.reason, "the other 2 (\"Mic\", \"Chat\") are not heard"));
    // Media Foundation's default names are left out of the sentence.
    c = check_of(moov_of({video_track(), audio_track("SoundHandler"), audio_track("SoundHandler")}));
    CHECK(c.needed);
    CHECK_EQ(c.reason, std::string("Discord and most players only play the first sound track, so the second is not heard."));
    // Backtrack's own clip with separate tracks (and a repaired clip): the first is the mix already.
    c = check_of(moov_of({video_track(), audio_track("Game + mic"), audio_track("Game"), audio_track("Mic")}));
    CHECK(!c.needed);
    CHECK_EQ(c.audio_tracks, 3);
    c = check_of(moov_of({video_track(), audio_track("game + MIC"), audio_track("Mic")}));
    CHECK(!c.needed);
    // One track, none, no video.
    c = check_of(moov_of({video_track(), audio_track("Game")}));
    CHECK(!c.needed);
    CHECK_EQ(c.audio_tracks, 1);
    c = check_of(moov_of({video_track()}));
    CHECK(!c.needed);
    CHECK_EQ(c.audio_tracks, 0);
    c = check_of(moov_of({audio_track("Game"), audio_track("Mic")}));
    CHECK(!c.needed);
    CHECK(bt_test::contains(c.reason, "no video"));
    // The other tracks hold no sound at all: nothing to add. An empty first track with a voice
    // after it is needed (Discord plays silence).
    TrackSpec empty_mic = audio_track("Mic");
    empty_mic.samples = 0;
    c = check_of(moov_of({video_track(), audio_track("Game"), empty_mic}));
    CHECK(!c.needed);
    TrackSpec empty_game = audio_track("Game");
    empty_game.samples = 0;
    CHECK(check_of(moov_of({video_track(), empty_game, audio_track("Mic")})).needed);
    // Video the repair cannot copy.
    TrackSpec hevc = video_track();
    hevc.entry = box("hvc1", Bytes(78, 0));
    c = check_of(moov_of({hevc, audio_track("Game"), audio_track("Mic")}));
    CHECK(!c.needed);
    CHECK(bt_test::contains(c.reason, "not H.264"));
    TrackSpec avc3 = video_track();
    avc3.entry = avc_entry("avc3", 3);
    CHECK(check_of(moov_of({avc3, audio_track("Game"), audio_track("Mic")})).needed);
    // NAL lengths of 2 bytes, or no avcC: the frames cannot be copied as they are.
    TrackSpec short_lengths = video_track();
    short_lengths.entry = avc_entry("avc1", 1);
    c = check_of(moov_of({short_lengths, audio_track("Game"), audio_track("Mic")}));
    CHECK(!c.needed);
    CHECK(bt_test::contains(c.reason, "stored in a way"));
    TrackSpec no_avcc = video_track();
    no_avcc.entry = box("avc1", Bytes(78, 0));
    CHECK(!check_of(moov_of({no_avcc, audio_track("Game"), audio_track("Mic")})).needed);
    CHECK_EQ(mp4::tracks(moov_of({video_track()}))->front().nal_length_size, std::uint8_t{4});
    TrackSpec b_frames = video_track();
    b_frames.ctts = true;
    c = check_of(moov_of({b_frames, audio_track("Game"), audio_track("Mic")}));
    CHECK(!c.needed);
    CHECK(bt_test::contains(c.reason, "B-frames"));
    // Sound that is not AAC at all.
    TrackSpec opus = audio_track("Mic");
    opus.entry = box("Opus", Bytes(36, 0));
    CHECK(!check_of(moov_of({video_track(), audio_track("Game"), opus})).needed);
    // Edit lists: a late start and encoder priming are fine; a cut, a shifted video or a long skip are not.
    TrackSpec late = audio_track("Mic");
    late.edts = elst({{3000, -1}, {5000, 0}});
    CHECK(check_of(moov_of({video_track(), audio_track("Game"), late})).needed);
    TrackSpec primed = audio_track("Mic");
    primed.edts = elst({{5000, 2112}});
    CHECK(check_of(moov_of({video_track(), audio_track("Game"), primed})).needed);
    TrackSpec trimmed = audio_track("Mic");
    trimmed.edts = elst({{5000, 48000}});  // one second skipped
    c = check_of(moov_of({video_track(), audio_track("Game"), trimmed}));
    CHECK(!c.needed);
    CHECK(bt_test::contains(c.reason, "edit list"));
    TrackSpec cut = audio_track("Mic");
    cut.edts = elst({{1000, 0}, {1000, 96000}});
    CHECK(bt_test::contains(check_of(moov_of({video_track(), audio_track("Game"), cut})).reason, "edit list"));
    TrackSpec late_video = video_track();
    late_video.edts = elst({{500, -1}, {5000, 0}});
    CHECK(bt_test::contains(check_of(moov_of({late_video, audio_track("Game"), audio_track("Mic")})).reason, "edit list"));
    // An edit that ends well before the media (a trim of the end): a copy of every frame would undo it.
    TrackSpec end_trim = video_track();
    end_trim.duration = 900'000;  // 10 s at 90 kHz
    end_trim.edts = elst({{5000, 0}});  // 5 s at the movie's 1000
    CHECK(!check_of(moov_of({end_trim, audio_track("Game"), audio_track("Mic")})).needed);
    end_trim.edts = elst({{9950, 0}});  // 50 ms short: rounding, still the whole video
    CHECK(check_of(moov_of({end_trim, audio_track("Game"), audio_track("Mic")})).needed);
    // A start more than 10 minutes late is not believable.
    TrackSpec very_late = audio_track("Mic");
    very_late.edts = elst({{601'000, -1}, {5000, 0}});
    CHECK(!check_of(moov_of({video_track(), audio_track("Game"), very_late})).needed);
    // An empty track's odd edit list does not matter.
    TrackSpec empty_cut = audio_track("Chat");
    empty_cut.samples = 0;
    empty_cut.edts = elst({{1000, 0}, {1000, 96000}});
    CHECK(check_of(moov_of({video_track(), audio_track("Game"), audio_track("Mic"), empty_cut})).needed);
}

void survives_damaged_indexes() {
    CHECK(!mp4::tracks(Bytes{}).has_value());
    CHECK(!mp4::tracks(box("free", Bytes(8, 0))).has_value());
    const Bytes moov = medal_moov();
    // Every shortened copy (size field fixed up) is refused or read without going past its end.
    bool all_safe = true;
    for (std::size_t length = 0; length < moov.size(); ++length) {
        Bytes cut(moov.begin(), moov.begin() + static_cast<std::ptrdiff_t>(length));
        if (cut.size() >= 4) {
            cut[0] = static_cast<std::uint8_t>(length >> 24);
            cut[1] = static_cast<std::uint8_t>(length >> 16);
            cut[2] = static_cast<std::uint8_t>(length >> 8);
            cut[3] = static_cast<std::uint8_t>(length);
        }
        const auto tracks = mp4::tracks(cut);
        all_safe = all_safe && (!tracks || tracks->size() <= 3);
        if (tracks) {
            (void)decide_repair(*tracks);
        }
        (void)mp4::moov_child(cut, "udta");
    }
    CHECK(all_safe);
    // Every byte changed in turn (the worst value for sizes): never a crash.
    for (std::size_t at = 8; at < moov.size(); ++at) {
        Bytes bad = moov;
        bad[at] = 0xFF;
        const auto tracks = mp4::tracks(bad);
        if (tracks) {
            (void)decide_repair(*tracks);
        }
    }
    CHECK(true);
}

void keeps_the_source_notes() {
    const Bytes moov = medal_moov();
    const auto udta = mp4::moov_child(moov, "udta");
    CHECK(udta.has_value());
    CHECK_EQ(udta->size(), std::size_t{28});
    CHECK(!mp4::moov_child(moov, "meta").has_value());
    // The sink writer's moov has its own udta; the source's replaces it, as the last box.
    const Bytes written = moov_of({video_track(), audio_track("Game + mic"), audio_track("Game"), audio_track("Mic")},
                                  box("udta", Bytes(4, 0x99)));
    const auto replaced = mp4::replace_moov_child(written, *udta);
    CHECK(replaced.has_value());
    CHECK_EQ(replaced->size(), written.size() - 12 + 28);
    CHECK(std::equal(udta->begin(), udta->end(), replaced->end() - static_cast<std::ptrdiff_t>(udta->size())));
    const auto again = mp4::tracks(*replaced);
    CHECK(again && again->size() == 4 && (*again)[1].name == "Game + mic");
    const auto only = mp4::moov_child(*replaced, "udta");
    CHECK(only && *only == *udta);
    // Without one to replace it is added; the chunk offsets can still be moved afterwards.
    const Bytes bare = moov_of({video_track(), audio_track("Game")});
    const auto added = mp4::replace_moov_child(bare, *udta);
    CHECK(added && added->size() == bare.size() + udta->size());
    CHECK(mp4::shift_chunk_offsets(*added, 100).has_value());
    // Not a box, or not a moov: refused.
    CHECK(!mp4::replace_moov_child(bare, Bytes{1, 2, 3}).has_value());
    CHECK(!mp4::replace_moov_child(Bytes{}, *udta).has_value());
    // A udta with size 0 ("to the end of the moov") gets its real size in the copy.
    Bytes open_udta = box("udta", Bytes(6, 0x55));
    open_udta[0] = open_udta[1] = open_udta[2] = open_udta[3] = 0;
    const Bytes open_moov = box("moov", concat({mvhd(1000), track(video_track()), open_udta}));
    const auto fixed = mp4::moov_child(open_moov, "udta");
    CHECK(fixed && fixed->size() == 14 && (*fixed)[3] == 14);
}

void picks_the_bitrate_and_rate() {
    CHECK_EQ(mix_aac_bytes_per_second({}), 20'000u);  // 160 kbit/s
    const std::uint32_t medal[] = {24'000, 23'999};  // 192 kbit/s, a hair under
    CHECK_EQ(mix_aac_bytes_per_second(medal), 24'000u);
    const std::uint32_t low[] = {17'862, 17'826};  // the lowest-fps Medal clip: 142.9 kbit/s
    CHECK_EQ(mix_aac_bytes_per_second(low), 20'000u);
    const std::uint32_t high[] = {16'000, 40'000};  // more than the encoder takes: its highest
    CHECK_EQ(mix_aac_bytes_per_second(high), 24'000u);
    const std::uint32_t hair[] = {23'999};  // the slack: a hair under 192 kbit/s counts as 192
    CHECK_EQ(mix_aac_bytes_per_second(hair), 24'000u);
    const std::uint32_t under[] = {23'700};  // 189.6 kbit/s: more than 1 % under
    CHECK_EQ(mix_aac_bytes_per_second(under), 20'000u);
    const std::uint32_t between[] = {22'000};  // 176 kbit/s: the step below
    CHECK_EQ(mix_aac_bytes_per_second(between), 20'000u);
    const std::uint32_t none[] = {0, 0};
    CHECK_EQ(mix_aac_bytes_per_second(none), 20'000u);
    CHECK_EQ(mix_sample_rate(48'000), 48'000u);
    CHECK_EQ(mix_sample_rate(44'100), 44'100u);
    CHECK_EQ(mix_sample_rate(32'000), 48'000u);
    CHECK_EQ(mix_sample_rate(0), 48'000u);
}

void maps_reader_streams_to_tracks() {
    using K = TrackKind;
    // Medal and Backtrack: the reader lists the tracks last to first (measured).
    const K file[] = {K::Video, K::Audio, K::Audio};
    const K reversed[] = {K::Audio, K::Audio, K::Video};
    auto orders = reader_track_orders(file, reversed);
    CHECK_EQ(orders.size(), std::size_t{1});
    CHECK(orders[0] == (std::vector<std::size_t>{2, 1, 0}));
    // A reader in file order works too.
    orders = reader_track_orders(file, file);
    CHECK_EQ(orders.size(), std::size_t{1});
    CHECK(orders[0] == (std::vector<std::size_t>{0, 1, 2}));
    // Mirrored: both fit, the measured order first.
    const K mirrored[] = {K::Audio, K::Video, K::Audio};
    orders = reader_track_orders(mirrored, mirrored);
    CHECK_EQ(orders.size(), std::size_t{2});
    CHECK(orders[0] == (std::vector<std::size_t>{2, 1, 0}));
    CHECK(orders[1] == (std::vector<std::size_t>{0, 1, 2}));
    // Nothing fits, or the counts differ.
    const K odd[] = {K::Audio, K::Video, K::Other};
    CHECK(reader_track_orders(file, odd).empty());
    CHECK(reader_track_orders(file, std::span<const K>(reversed, 2)).empty());
    CHECK(reader_track_orders({}, {}).empty());
    const K one[] = {K::Video};
    CHECK_EQ(reader_track_orders(one, one).size(), std::size_t{1});
}

Bytes stsz_sizes(std::initializer_list<std::uint32_t> sizes) {
    Bytes body(4, 0);
    put32(body, 0);
    put32(body, static_cast<std::uint32_t>(sizes.size()));
    for (const std::uint32_t s : sizes) put32(body, s);
    return box("stsz", body);
}

Bytes stsc(std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> runs) {
    Bytes body(4, 0);
    put32(body, static_cast<std::uint32_t>(runs.size()));
    for (const auto& [first_chunk, per_chunk] : runs) {
        put32(body, first_chunk);
        put32(body, per_chunk);
        put32(body, 1);  // sample description index
    }
    return box("stsc", body);
}

Bytes chunk_offsets(std::initializer_list<std::uint64_t> offsets, bool wide = false) {
    Bytes body(4, 0);
    put32(body, static_cast<std::uint32_t>(offsets.size()));
    for (const std::uint64_t o : offsets) {
        if (wide) put64(body, o);
        else put32(body, static_cast<std::uint32_t>(o));
    }
    return box(wide ? "co64" : "stco", body);
}

std::optional<std::vector<mp4::SampleLocation>> locations_of(const Bytes& tables, std::uint64_t file_size = 1'000'000,
                                                             std::size_t track = 1) {
    TrackSpec video = video_track();
    video.tables = tables;
    return mp4::sample_locations(moov_of({audio_track("Game"), video}), track, file_size);
}

void finds_every_frame_in_the_file() {
    // Five frames in three chunks: two, two, one (the last run of stsc covers the rest).
    const Bytes tables = concat({stsz_sizes({10, 20, 30, 40, 50}), stsc({{1, 2}, {3, 1}}), chunk_offsets({100, 200, 300})});
    auto list = locations_of(tables);
    CHECK(list.has_value());
    CHECK_EQ(list->size(), std::size_t{5});
    const std::uint64_t offsets[] = {100, 110, 200, 230, 300};
    const std::uint32_t sizes[] = {10, 20, 30, 40, 50};
    bool all = list->size() == 5;
    for (std::size_t i = 0; all && i < 5; ++i) all = (*list)[i].offset == offsets[i] && (*list)[i].size == sizes[i];
    CHECK(all);
    // 64-bit chunk offsets, past 4 GB.
    list = locations_of(concat({stsz_sizes({10, 20}), stsc({{1, 1}}), chunk_offsets({0x1'0000'0000ull, 0x1'0000'1000ull}, true)}),
                        0x2'0000'0000ull);
    CHECK(list && list->size() == 2 && (*list)[1].offset == 0x1'0000'1000ull && (*list)[1].size == 20);
    // One size for every sample.
    Bytes constant(4, 0);
    put32(constant, 8);
    put32(constant, 4);
    list = locations_of(concat({box("stsz", constant), stsc({{1, 4}}), chunk_offsets({64})}));
    CHECK(list && list->size() == 4 && (*list)[3].offset == 64 + 24 && (*list)[3].size == 8);
    // Compact sizes (stz2), 8 and 4 bits.
    Bytes compact8(4, 0);
    compact8.insert(compact8.end(), {0, 0, 0, 8});
    put32(compact8, 3);
    compact8.insert(compact8.end(), {5, 6, 7});
    list = locations_of(concat({box("stz2", compact8), stsc({{1, 3}}), chunk_offsets({1000})}));
    CHECK(list && list->size() == 3 && (*list)[2].offset == 1011 && (*list)[2].size == 7);
    Bytes compact4(4, 0);
    compact4.insert(compact4.end(), {0, 0, 0, 4});
    put32(compact4, 3);
    compact4.insert(compact4.end(), {0x12, 0x30});
    list = locations_of(concat({box("stz2", compact4), stsc({{1, 3}}), chunk_offsets({0})}));
    CHECK(list && list->size() == 3 && (*list)[0].size == 1 && (*list)[1].size == 2 && (*list)[2].size == 3);
    // Another track by its position; one that is not there.
    TrackSpec audio = audio_track("Game");
    audio.tables = concat({stsz_sizes({7}), stsc({{1, 1}}), chunk_offsets({5})});
    const auto first = mp4::sample_locations(moov_of({audio, video_track()}), 0, 1000);
    CHECK(first && first->size() == 1 && (*first)[0].offset == 5);
    CHECK(!locations_of(tables, 1'000'000, 2).has_value());
    // Tables that do not add up: fewer samples in the chunks than sizes, a chunk run starting at 0
    // or going backwards, a run past the last chunk, a frame past the end of the file, missing tables.
    CHECK(!locations_of(concat({stsz_sizes({10, 20, 30}), stsc({{1, 1}}), chunk_offsets({100, 200})})).has_value());
    CHECK(!locations_of(concat({stsz_sizes({10}), stsc({{0, 1}}), chunk_offsets({100})})).has_value());
    CHECK(!locations_of(concat({stsz_sizes({10, 20}), stsc({{2, 1}, {1, 1}}), chunk_offsets({100, 200})})).has_value());
    CHECK(!locations_of(concat({stsz_sizes({10, 20}), stsc({{1, 1}, {5, 1}}), chunk_offsets({100, 200})})).has_value());
    CHECK(!locations_of(tables, 340).has_value());
    CHECK(locations_of(tables, 350).has_value());
    CHECK(!locations_of(concat({stsz_sizes({10}), chunk_offsets({100})})).has_value());
    CHECK(!locations_of(concat({stsz_sizes({10}), stsc({{1, 1}})})).has_value());
    // A size table claiming more entries than it holds, or an absurd constant-size count.
    Bytes lying(4, 0);
    put32(lying, 0);
    put32(lying, 1000);
    CHECK(!locations_of(concat({box("stsz", lying), stsc({{1, 1}}), chunk_offsets({1})})).has_value());
    Bytes huge(4, 0);
    put32(huge, 1);
    put32(huge, 0xFFFF'FFFFu);
    CHECK(!locations_of(concat({box("stsz", huge), stsc({{1, 0xFFFF'FFFFu}}), chunk_offsets({1})})).has_value());
    // A constant size whose samples together are bigger than the file.
    Bytes too_big(4, 0);
    put32(too_big, 1000);
    put32(too_big, 2000);
    CHECK(!locations_of(concat({box("stsz", too_big), stsc({{1, 2000}}), chunk_offsets({0})})).has_value());
    // No samples at all: an empty list.
    list = locations_of(concat({stsz_sizes({}), stsc({}), chunk_offsets({})}));
    CHECK(list && list->empty());
}

void names_the_backup() {
    CHECK(backup_file_name(L"clip.mp4", 1) == L"clip.mp4");
    CHECK(backup_file_name(L"clip.mp4", 2) == L"clip (2).mp4");
    CHECK(backup_file_name(L"Grand-Theft-Auto-V-Legacy__2026-08-03__21-02-13.mp4", 12) ==
          L"Grand-Theft-Auto-V-Legacy__2026-08-03__21-02-13 (12).mp4");
    CHECK(backup_file_name(L"a.b.mp4", 3) == L"a.b (3).mp4");
    CHECK(backup_file_name(L"noextension", 2) == L"noextension (2)");
    CHECK(backup_file_name(L".hidden", 2) == L".hidden (2)");
}

std::vector<float> stereo(std::size_t frames, float value) {
    return std::vector<float>(frames * 2, value);
}

void mixes_tracks_on_one_timeline() {
    // Two tracks in step: the sum, frame for frame.
    TrackMixer mixer(2, 2, 960);
    mixer.add(0, 0, stereo(1024, 0.25f));
    CHECK_EQ(mixer.ready(), std::int64_t{0});  // the second track has not reached anything yet
    mixer.add(1, 0, stereo(1000, 0.5f));
    CHECK_EQ(mixer.ready(), std::int64_t{1000});
    std::vector<float> out;
    mixer.mix(out);
    CHECK_EQ(out.size(), std::size_t{2000});
    CHECK(std::all_of(out.begin(), out.end(), [](float v) { return v == 0.75f; }));
    CHECK_EQ(mixer.mixed(), std::int64_t{1000});
    // Medal's stamps stray a little from back to back: still back to back (no gap, no cut).
    mixer.add(0, 1000, stereo(1024, 0.25f));  // says 1000, the track is at 1024: 24 frames off
    CHECK_EQ(mixer.written(0), std::int64_t{2048});
    mixer.add(1, 1100, stereo(1048, 0.5f));  // 100 frames late by its stamp, within 20 ms
    CHECK_EQ(mixer.written(1), std::int64_t{2048});
    out.clear();
    mixer.mix(out);
    CHECK_EQ(out.size(), std::size_t{2 * 1048});
    CHECK(std::all_of(out.begin(), out.end(), [](float v) { return v == 0.75f; }));

    // A real gap (the game hung for 0.7 s): silence, so what follows stays in step.
    mixer.add(0, 2048 + 33600, stereo(1024, 0.25f));
    CHECK_EQ(mixer.written(0), std::int64_t{2048 + 33600 + 1024});
    // An overlap: the part already there is cut.
    mixer.add(1, 2048 - 2000, stereo(3000, 0.5f));
    CHECK_EQ(mixer.written(1), std::int64_t{2048 + 1000});
    out.clear();
    mixer.mix(out);
    CHECK_EQ(out.size(), std::size_t{2 * 1000});
    CHECK(std::all_of(out.begin(), out.end(), [](float v) { return v == 0.5f; }));  // track 0 is silent here
    // The second track ends: from there it is silence, and the first decides.
    mixer.finish(1);
    CHECK_EQ(mixer.ready(), std::int64_t{2048 + 33600 + 1024});
    out.clear();
    mixer.mix(out);
    CHECK_EQ(out.size(), std::size_t{2 * (33600 + 1024 - 1000)});
    CHECK_EQ(out[2 * (33600 - 1000) - 1], 0.0f);  // the last frame of the gap
    CHECK_EQ(out[2 * (33600 - 1000)], 0.25f);  // the first after it
    mixer.add(1, 50000, stereo(10, 1.0f));  // nothing is added to a finished track
    CHECK_EQ(mixer.written(1), std::int64_t{3048});
    mixer.finish(0);
    out.clear();
    mixer.mix(out);
    CHECK(out.empty());
}

void places_late_and_early_starts_exactly() {
    TrackMixer mixer(2, 1, 960);
    // A track that starts 10 ms late (inside the tolerance) still starts late: the first piece is exact.
    mixer.add(1, 480, std::vector<float>(100, 1.0f));
    CHECK_EQ(mixer.written(1), std::int64_t{580});
    // Sound before frame 0 is left out.
    mixer.add(0, -50, std::vector<float>(100, 0.5f));
    CHECK_EQ(mixer.written(0), std::int64_t{50});
    mixer.add(0, -5000, std::vector<float>(100, 0.5f));  // wholly before 0 after its first piece: a cut
    CHECK_EQ(mixer.written(0), std::int64_t{50});
    TrackMixer early(1, 1, 960);
    early.add(0, -5000, std::vector<float>(100, 0.5f));  // wholly before 0: nothing
    CHECK_EQ(early.written(0), std::int64_t{0});
    early.add(0, 0, std::vector<float>(10, 0.5f));
    CHECK_EQ(early.written(0), std::int64_t{10});
    std::vector<float> out;
    mixer.finish(0);
    mixer.finish(1);
    mixer.mix(out);
    CHECK_EQ(out.size(), std::size_t{580});
    CHECK_EQ(out[0], 0.5f);  // track 0 only
    CHECK_EQ(out[49], 0.5f);
    CHECK_EQ(out[50], 0.0f);  // neither
    CHECK_EQ(out[480], 1.0f);  // track 1 only
    // A track that never had sound and has finished counts as silence.
    TrackMixer silent(2, 2, 960);
    silent.finish(1);
    silent.add(0, 0, stereo(10, 0.1f));
    CHECK_EQ(silent.ready(), std::int64_t{10});
    // Odd sample counts (not a whole frame) leave the rest out.
    TrackMixer odd(1, 2, 960);
    odd.add(0, 0, std::vector<float>(5, 0.1f));
    CHECK_EQ(odd.written(0), std::int64_t{2});
}

float peak_of(const std::vector<std::int16_t>& pcm) {
    int peak = 0;
    for (const std::int16_t s : pcm) peak = std::max(peak, std::abs(static_cast<int>(s)));
    return static_cast<float>(peak) / 32768.0f;
}

void limits_without_touching_quiet_sound() {
    constexpr std::uint32_t kLook = 96;
    constexpr std::uint32_t kRelease = 4800;
    constexpr float kCeiling = 0.944f;
    // Under the ceiling: exactly the input, after the look-ahead, and every frame comes out.
    PeakLimiter quiet(2, kLook, kRelease, kCeiling);
    std::vector<float> in;
    for (int i = 0; i < 4800; ++i) {
        const float v = 0.9f * static_cast<float>(std::sin(i * 0.05));
        in.push_back(v);
        in.push_back(-v);
    }
    std::vector<std::int16_t> out;
    quiet.process(in, out);
    CHECK_EQ(out.size(), (std::size_t{4800} - kLook) * 2);
    quiet.flush(out);
    CHECK_EQ(out.size(), in.size());
    bool exact = true;
    for (std::size_t i = 0; i < in.size(); ++i) {
        exact = exact && out[i] == static_cast<std::int16_t>(std::lround(in[i] * 32768.0f));
    }
    CHECK(exact);
    CHECK_EQ(quiet.limited_frames(), std::uint64_t{0});

    // A loud sum (game and voice together at 1.8x full scale for 50 ms): never past the ceiling,
    // never clipped, and the gain slides instead of jumping.
    PeakLimiter loud(1, kLook, kRelease, kCeiling);
    std::vector<float> burst;
    for (int i = 0; i < 48000; ++i) {
        const float level = (i >= 10000 && i < 12400) ? 1.8f : 0.5f;
        burst.push_back(level * static_cast<float>(std::sin(i * 0.3)));
    }
    std::vector<std::int16_t> limited;
    loud.process(burst, limited);
    loud.flush(limited);
    CHECK_EQ(limited.size(), burst.size());
    CHECK(peak_of(limited) <= kCeiling + 0.001f);
    CHECK(loud.limited_frames() > 2400);
    CHECK(loud.lowest_gain() < 0.6f && loud.lowest_gain() > 0.45f);
    // The gain (output / input, where the input is not tiny) changes by at most 1 / (look-ahead + 1) a frame.
    float biggest_step = 0.0f;
    float previous = 1.0f;
    for (std::size_t i = 0; i < burst.size(); ++i) {
        if (std::abs(burst[i]) > 0.2f) {
            const float gain = static_cast<float>(limited[i]) / 32768.0f / burst[i];
            if (i > 0) biggest_step = std::max(biggest_step, std::abs(gain - previous));
            previous = gain;
        }
    }
    CHECK(biggest_step < 0.03f);
    // Long after the burst (past the release) the sound is untouched again.
    bool untouched = true;
    for (std::size_t i = 30000; i < burst.size(); ++i) {
        untouched = untouched && limited[i] == static_cast<std::int16_t>(std::lround(burst[i] * 32768.0f));
    }
    CHECK(untouched);

    // The very first frames have no look-ahead history: the soft knee still keeps them under full scale.
    PeakLimiter first(1, kLook, kRelease, kCeiling);
    std::vector<float> start(200, 3.0f);
    std::vector<std::int16_t> start_out;
    first.process(start, start_out);
    first.flush(start_out);
    CHECK(peak_of(start_out) < 1.0f);
    // Damaged input (NaN, infinity) becomes silence, not a full-scale burst.
    PeakLimiter damaged(1, 4, 100, kCeiling);
    const float bad[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 0.5f};
    std::vector<std::int16_t> bad_out;
    damaged.process(bad, bad_out);
    damaged.flush(bad_out);
    CHECK_EQ(bad_out.size(), std::size_t{3});
    CHECK_EQ(bad_out[0], std::int16_t{0});
    CHECK_EQ(bad_out[1], std::int16_t{0});
    CHECK_EQ(bad_out[2], std::int16_t{16384});
    // Full-scale negative input maps to the int16 range without overflow.
    PeakLimiter edge(1, 0, 1, kCeiling);
    const float extremes[] = {-1.0f, 1.0f};
    std::vector<std::int16_t> edge_out;
    edge.process(extremes, edge_out);
    edge.flush(edge_out);
    CHECK(edge_out.size() == 2 && edge_out[0] < 0 && edge_out[1] > 0);
}

}  // namespace

int main() {
    reads_the_tracks_of_a_medal_clip();
    reads_edit_lists();
    reads_what_the_sound_really_is();
    decides_which_clips_need_a_repair();
    survives_damaged_indexes();
    keeps_the_source_notes();
    picks_the_bitrate_and_rate();
    maps_reader_streams_to_tracks();
    finds_every_frame_in_the_file();
    names_the_backup();
    mixes_tracks_on_one_timeline();
    places_late_and_early_starts_exactly();
    limits_without_touching_quiet_sound();
    return bt_test::finish("RepairPlanTests");
}
