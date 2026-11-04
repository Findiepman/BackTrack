#include "core/Mp4.h"

#include <limits>

namespace bt::mp4 {
namespace {

constexpr std::size_t kHeaderSize = 8;
constexpr std::size_t kLargeHeaderSize = 16;
constexpr std::size_t kFullBoxFields = 4;  // version + flags
constexpr std::size_t kEntryCountSize = 4;

std::uint32_t read32(std::span<const std::uint8_t> b, std::size_t at) {
    return (std::uint32_t{b[at]} << 24) | (std::uint32_t{b[at + 1]} << 16) | (std::uint32_t{b[at + 2]} << 8) |
           std::uint32_t{b[at + 3]};
}

std::uint64_t read64(std::span<const std::uint8_t> b, std::size_t at) {
    return (std::uint64_t{read32(b, at)} << 32) | read32(b, at + 4);
}

void write32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    b[at] = static_cast<std::uint8_t>(v >> 24);
    b[at + 1] = static_cast<std::uint8_t>(v >> 16);
    b[at + 2] = static_cast<std::uint8_t>(v >> 8);
    b[at + 3] = static_cast<std::uint8_t>(v);
}

void write64(std::vector<std::uint8_t>& b, std::size_t at, std::uint64_t v) {
    write32(b, at, static_cast<std::uint32_t>(v >> 32));
    write32(b, at + 4, static_cast<std::uint32_t>(v));
}

bool is_container(const std::string& type) {
    // The path from moov down to the chunk offset tables. Everything else is copied unchanged.
    return type == "moov" || type == "trak" || type == "mdia" || type == "minf" || type == "stbl";
}

// Walks the boxes in [begin, end) of `box` and patches chunk offset tables in place.
bool patch(std::vector<std::uint8_t>& box, std::size_t begin, std::size_t end, std::uint64_t shift) {
    std::size_t at = begin;
    while (at < end) {
        const std::span<const std::uint8_t> view(box.data(), end);
        const auto header = read_box_header(view, at, end);
        if (!header || header->size > end - at) {
            return false;
        }
        const std::size_t header_size = (read32(view, at) == 1) ? kLargeHeaderSize : kHeaderSize;
        const std::size_t body = at + header_size;
        const std::size_t box_end = at + static_cast<std::size_t>(header->size);
        if (is_container(header->type)) {
            if (!patch(box, body, box_end, shift)) {
                return false;
            }
        } else if (header->type == "stco" || header->type == "co64") {
            const bool wide = header->type == "co64";
            const std::size_t entry_size = wide ? 8 : 4;
            if (box_end - body < kFullBoxFields + kEntryCountSize) {
                return false;
            }
            const std::uint32_t count = read32(view, body + kFullBoxFields);
            const std::size_t first = body + kFullBoxFields + kEntryCountSize;
            if (static_cast<std::uint64_t>(count) * entry_size > box_end - first) {
                return false;
            }
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::size_t entry = first + i * entry_size;
                if (wide) {
                    write64(box, entry, read64(view, entry) + shift);
                } else {
                    const std::uint64_t moved = std::uint64_t{read32(view, entry)} + shift;
                    if (moved > std::numeric_limits<std::uint32_t>::max()) {
                        return false;  // would need co64; the file keeps its moov at the end
                    }
                    write32(box, entry, static_cast<std::uint32_t>(moved));
                }
            }
        }
        at = box_end;
    }
    return at == end;
}

}  // namespace

std::optional<BoxHeader> read_box_header(std::span<const std::uint8_t> bytes, std::uint64_t offset,
                                         std::uint64_t file_size) {
    if (offset + kHeaderSize > bytes.size()) {
        return std::nullopt;
    }
    const auto at = static_cast<std::size_t>(offset);
    BoxHeader header;
    header.offset = offset;
    header.type.assign(reinterpret_cast<const char*>(bytes.data() + at + 4), 4);
    const std::uint32_t size32 = read32(bytes, at);
    if (size32 == 1) {
        if (at + kLargeHeaderSize > bytes.size()) {
            return std::nullopt;
        }
        header.size = read64(bytes, at + kHeaderSize);
        if (header.size < kLargeHeaderSize) {
            return std::nullopt;
        }
    } else if (size32 == 0) {
        if (file_size < offset + kHeaderSize) {
            return std::nullopt;
        }
        header.size = file_size - offset;
    } else {
        if (size32 < kHeaderSize) {
            return std::nullopt;
        }
        header.size = size32;
    }
    return header;
}

std::optional<std::vector<std::uint8_t>> shift_chunk_offsets(std::span<const std::uint8_t> moov,
                                                             std::uint64_t shift) {
    const auto header = read_box_header(moov, 0, moov.size());
    if (!header || header->type != "moov" || header->size != moov.size()) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> copy(moov.begin(), moov.end());
    const std::size_t header_size = (read32(moov, 0) == 1) ? kLargeHeaderSize : kHeaderSize;
    if (!patch(copy, header_size, copy.size(), shift)) {
        return std::nullopt;
    }
    return copy;
}

}  // namespace bt::mp4
