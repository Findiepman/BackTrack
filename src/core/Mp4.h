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

}  // namespace bt::mp4
