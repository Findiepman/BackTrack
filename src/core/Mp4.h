#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bt::mp4 {

// One ISO-BMFF box header as found in a file.
struct BoxHeader {
    std::string type;  // four characters, e.g. "moov"
    std::uint64_t offset = 0;  // where the box starts
    std::uint64_t size = 0;  // whole box including its header
};

// Reads a box header from the first bytes at `offset` (8 bytes, or 16 for a 64-bit size). A size
// of 0 ("to the end of the file") is resolved with file_size. Empty when the bytes are not a
// valid header.
std::optional<BoxHeader> read_box_header(std::span<const std::uint8_t> bytes, std::uint64_t offset,
                                         std::uint64_t file_size);

// "Fast start": a copy of the moov box with every chunk offset (stco / co64) moved by `shift`
// bytes, for when the moov box is moved in front of the media data. Empty when the box is not
// understood or a 32-bit offset would overflow; the caller then leaves the file as it is.
std::optional<std::vector<std::uint8_t>> shift_chunk_offsets(std::span<const std::uint8_t> moov,
                                                             std::uint64_t shift);

// A clip with several audio tracks (separate tracks): a copy of the moov box in which they are
// alternatives of each other, the way FFmpeg writes them, so a player plays the first one only
// instead of all of them at once (QuickTime and some editors play every enabled track that is in
// no alternate group). Every audio track's tkhd gets alternate_group 1, and only the first one
// keeps its "enabled" flag. When `names` has an entry for an audio track (in track order), its
// handler name (hdlr: the track name players and editors show) becomes it; an empty entry keeps
// the name. The box sizes are updated when a name changes the size. Empty when the box is not
// understood. With fewer than two audio tracks the copy is unchanged.
std::optional<std::vector<std::uint8_t>> label_audio_tracks(std::span<const std::uint8_t> moov,
                                                            std::span<const std::string> names);

struct AudioTrackInfo {
    std::string name;  // the handler name (hdlr)
    std::vector<std::uint8_t> sample_description;  // the whole stsd box; empty when the track has none
};

// The audio tracks of a moov box in file order; empty when the box is not understood. A trim
// uses it to keep the names, and to put the tracks back in file order: Media Foundation's source
// reader numbers them differently (measured: last track first), but hands out each one's stsd
// as MF_MT_MPEG4_SAMPLE_DESCRIPTION.
std::vector<AudioTrackInfo> audio_tracks(std::span<const std::uint8_t> moov);

// What the repair (core/RepairPlan) needs to know about one track, read from the moov alone.
struct TrackInfo {
    std::string handler;  // hdlr handler type: "vide", "soun", ...
    std::string name;  // hdlr name ("Game", "Mic", "VideoHandler")
    std::string codec;  // the first sample entry in stsd: "avc1", "hvc1", "mp4a", ...; empty when there is none
    bool enabled = false;  // tkhd: players may play it
    std::uint32_t timescale = 0;  // mdhd: the track's time unit, ticks per second
    std::uint64_t duration = 0;  // mdhd: the media's length in that unit
    std::uint32_t samples = 0;  // stsz / stz2 sample count
    bool reordered = false;  // stbl has a ctts box: frames are stored out of display order (B-frames)
    std::uint32_t sample_rate = 0;  // audio: from the sample entry; 0 when it does not say (QuickTime v2)
    std::uint16_t channels = 0;  // audio
    // mp4a: the esds objectTypeIndication (0x40 AAC, 0x66 to 0x68 MPEG-2 AAC, 0x69 and 0x6B MP3);
    // 0 when there is none. MP3 in an MP4 is also an "mp4a" sample entry.
    std::uint8_t object_type = 0;
    std::vector<std::uint8_t> sample_description;  // the whole stsd box
    // The edit list (edts > elst): where the track's media time 0 is shown on the clip's timeline,
    // in 100 ns units. Positive: the track starts late (empty edits first, as FFmpeg writes a track
    // that starts after the others); negative: its first media is skipped (AAC encoder priming, or
    // a trim done with an edit list). Media Foundation's MP4 reader ignores edit lists (measured),
    // so this is what it gets wrong.
    std::int64_t edit_shift = 0;
    bool edits_understood = true;  // false: an edit list with more than one piece, or another speed
    // avc1 / avc3: the bytes before each NAL unit in a sample (avcC lengthSizeMinusOne + 1); 0 when
    // there is no avcC.
    std::uint8_t nal_length_size = 0;
};

// Every track of a moov box in file order. Empty optional when the box is not understood; a track
// without some of the boxes simply leaves those fields at their defaults.
std::optional<std::vector<TrackInfo>> tracks(std::span<const std::uint8_t> moov);

// Where one sample (a video frame or an audio packet) is stored in the file.
struct SampleLocation {
    std::uint64_t offset = 0;
    std::uint32_t size = 0;
};

// Every sample of the track at `track` (counting traks in file order) in decode order, from its
// sample tables (stsz, stsc, stco / co64). The repair reads the video's bytes straight from the
// file with it, exactly as stored. Empty when the tables are missing, do not add up, or a sample
// would lie past `file_size`.
std::optional<std::vector<SampleLocation>> sample_locations(std::span<const std::uint8_t> moov, std::size_t track,
                                                            std::uint64_t file_size);

// The first box of this type directly inside the moov box (e.g. "udta", where Medal and other
// recorders keep their own notes about the clip), whole. Empty when there is none or the box is
// not understood.
std::optional<std::vector<std::uint8_t>> moov_child(std::span<const std::uint8_t> moov, const std::string& type);

// A copy of the moov box with `box` (a whole box) added as its last child and every box of the same
// type that was there left out, so a repaired clip keeps the source's udta instead of the sink
// writer's. Empty when either is not understood.
std::optional<std::vector<std::uint8_t>> replace_moov_child(std::span<const std::uint8_t> moov,
                                                            std::span<const std::uint8_t> box);

}  // namespace bt::mp4
