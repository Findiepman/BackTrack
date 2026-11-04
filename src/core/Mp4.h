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

}  // namespace bt::mp4
