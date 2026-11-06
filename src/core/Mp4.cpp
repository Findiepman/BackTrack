#include "core/Mp4.h"

#include <algorithm>
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

// Box layouts used by label_audio_tracks (ISO/IEC 14496-12).
constexpr std::size_t kHandlerTypeAt = kFullBoxFields + 4;  // hdlr: after version/flags and pre_defined
constexpr std::size_t kHandlerNameAt = kHandlerTypeAt + 4 + 12;  // after handler_type and 3 reserved words
// tkhd: the alternate_group field follows the times, track id, duration (32 or 64 bit by version),
// two reserved words and the layer.
constexpr std::size_t kTkhdAlternateGroupV0 = kFullBoxFields + 5 * 4 + 8 + 2;
constexpr std::size_t kTkhdAlternateGroupV1 = kFullBoxFields + 2 * 8 + 2 * 4 + 8 + 8 + 2;
constexpr std::uint8_t kTkhdEnabled = 0x01;  // lowest bit of the flags
constexpr std::uint16_t kAudioAlternateGroup = 1;  // what FFmpeg uses for audio

struct Child {
    std::string type;
    std::size_t begin = 0;  // the whole box
    std::size_t body = 0;  // after its header
    std::size_t end = 0;
};

// The boxes directly inside bytes[begin, end). Empty optional when they do not fill it exactly.
std::optional<std::vector<Child>> children(std::span<const std::uint8_t> bytes, std::size_t begin, std::size_t end) {
    std::vector<Child> list;
    const std::span<const std::uint8_t> view = bytes.first(end);
    for (std::size_t at = begin; at < end;) {
        const auto header = read_box_header(view, at, end);
        if (!header || header->size > end - at) {
            return std::nullopt;
        }
        const std::size_t header_size = (read32(view, at) == 1) ? kLargeHeaderSize : kHeaderSize;
        list.push_back(Child{header->type, at, at + header_size, at + static_cast<std::size_t>(header->size)});
        at = list.back().end;
    }
    return list;
}

void append(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

// A box with a 32-bit size around `body`; empty when it would not fit one.
std::optional<std::vector<std::uint8_t>> make_box(const std::string& type, std::span<const std::uint8_t> body) {
    if (body.size() > std::numeric_limits<std::uint32_t>::max() - kHeaderSize) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> box(kHeaderSize);
    write32(box, 0, static_cast<std::uint32_t>(body.size() + kHeaderSize));
    std::copy(type.begin(), type.end(), box.begin() + 4);
    append(box, body);
    return box;
}

// trak > mdia > hdlr: the handler type ("soun" for sound, "vide" for video) and where the hdlr
// box is; empty when the track has none.
std::optional<Child> find_handler(std::span<const std::uint8_t> bytes, const Child& trak, std::string& handler_type) {
    const auto trak_children = children(bytes, trak.body, trak.end);
    if (!trak_children) {
        return std::nullopt;
    }
    for (const Child& mdia : *trak_children) {
        if (mdia.type != "mdia") {
            continue;
        }
        const auto mdia_children = children(bytes, mdia.body, mdia.end);
        if (!mdia_children) {
            return std::nullopt;
        }
        for (const Child& hdlr : *mdia_children) {
            if (hdlr.type == "hdlr" && hdlr.end - hdlr.body >= kHandlerNameAt) {
                handler_type.assign(reinterpret_cast<const char*>(bytes.data() + hdlr.body + kHandlerTypeAt), 4);
                return hdlr;
            }
        }
    }
    return std::nullopt;
}

// trak > mdia > minf > stbl > stsd, the whole box; empty when it is not there.
std::vector<std::uint8_t> sample_description(std::span<const std::uint8_t> bytes, const Child& trak) {
    const Child* parent = &trak;
    std::optional<Child> found;
    for (const char* type : {"mdia", "minf", "stbl", "stsd"}) {
        const auto list = children(bytes, parent->body, parent->end);
        if (!list) {
            return {};
        }
        const auto it = std::find_if(list->begin(), list->end(), [&](const Child& c) { return c.type == type; });
        if (it == list->end()) {
            return {};
        }
        found = *it;
        parent = &*found;
    }
    return std::vector<std::uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(found->begin),
                                     bytes.begin() + static_cast<std::ptrdiff_t>(found->end));
}

std::string handler_name(std::span<const std::uint8_t> bytes, const Child& hdlr) {
    const auto* first = reinterpret_cast<const char*>(bytes.data() + hdlr.body + kHandlerNameAt);
    const auto* last = reinterpret_cast<const char*>(bytes.data() + hdlr.end);
    return std::string(first, std::find(first, last, '\0'));
}

// One audio trak rebuilt: tkhd marked, hdlr renamed when `name` is not empty.
std::optional<std::vector<std::uint8_t>> relabel_trak(std::span<const std::uint8_t> bytes, const Child& trak,
                                                      bool first_audio, const std::string& name) {
    const auto trak_children = children(bytes, trak.body, trak.end);
    if (!trak_children) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> body;
    for (const Child& child : *trak_children) {
        const auto whole = bytes.subspan(child.begin, child.end - child.begin);
        if (child.type == "tkhd") {
            std::vector<std::uint8_t> tkhd(whole.begin(), whole.end());
            const std::size_t body_at = child.body - child.begin;
            if (tkhd.size() < body_at + kFullBoxFields) {
                return std::nullopt;
            }
            const std::uint8_t version = tkhd[body_at];
            const std::size_t group_at = body_at + (version == 1 ? kTkhdAlternateGroupV1 : kTkhdAlternateGroupV0);
            if (version > 1 || tkhd.size() < group_at + 2) {
                return std::nullopt;
            }
            std::uint8_t& flags = tkhd[body_at + 3];
            flags = first_audio ? static_cast<std::uint8_t>(flags | kTkhdEnabled)
                                : static_cast<std::uint8_t>(flags & ~kTkhdEnabled);
            tkhd[group_at] = static_cast<std::uint8_t>(kAudioAlternateGroup >> 8);
            tkhd[group_at + 1] = static_cast<std::uint8_t>(kAudioAlternateGroup & 0xFF);
            append(body, tkhd);
        } else if (child.type == "mdia" && !name.empty()) {
            const auto mdia_children = children(bytes, child.body, child.end);
            if (!mdia_children) {
                return std::nullopt;
            }
            std::vector<std::uint8_t> mdia_body;
            for (const Child& part : *mdia_children) {
                if (part.type == "hdlr" && part.end - part.body >= kHandlerNameAt) {
                    std::vector<std::uint8_t> hdlr_body(bytes.begin() + static_cast<std::ptrdiff_t>(part.body),
                                                        bytes.begin() + static_cast<std::ptrdiff_t>(part.body + kHandlerNameAt));
                    hdlr_body.insert(hdlr_body.end(), name.begin(), name.end());
                    hdlr_body.push_back(0);
                    const auto hdlr = make_box("hdlr", hdlr_body);
                    if (!hdlr) {
                        return std::nullopt;
                    }
                    append(mdia_body, *hdlr);
                } else {
                    append(mdia_body, bytes.subspan(part.begin, part.end - part.begin));
                }
            }
            const auto mdia = make_box("mdia", mdia_body);
            if (!mdia) {
                return std::nullopt;
            }
            append(body, *mdia);
        } else {
            append(body, whole);
        }
    }
    return make_box("trak", body);
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

std::optional<std::vector<std::uint8_t>> label_audio_tracks(std::span<const std::uint8_t> moov,
                                                            std::span<const std::string> names) {
    const auto header = read_box_header(moov, 0, moov.size());
    if (!header || header->type != "moov" || header->size != moov.size()) {
        return std::nullopt;
    }
    const std::size_t header_size = (read32(moov, 0) == 1) ? kLargeHeaderSize : kHeaderSize;
    const auto top = children(moov, header_size, moov.size());
    if (!top) {
        return std::nullopt;
    }
    std::vector<bool> is_audio(top->size(), false);
    std::size_t audio_tracks = 0;
    for (std::size_t i = 0; i < top->size(); ++i) {
        if ((*top)[i].type != "trak") {
            continue;
        }
        // Every track has a handler; one that cannot be found is a box this code does not understand.
        std::string handler_type;
        if (!find_handler(moov, (*top)[i], handler_type)) {
            return std::nullopt;
        }
        if (handler_type == "soun") {
            is_audio[i] = true;
            ++audio_tracks;
        }
    }
    if (audio_tracks < 2) {
        return std::vector<std::uint8_t>(moov.begin(), moov.end());
    }
    std::vector<std::uint8_t> body;
    std::size_t audio_index = 0;
    for (std::size_t i = 0; i < top->size(); ++i) {
        const Child& child = (*top)[i];
        if (!is_audio[i]) {
            append(body, moov.subspan(child.begin, child.end - child.begin));
            continue;
        }
        const std::string name = audio_index < names.size() ? names[audio_index] : std::string();
        const auto trak = relabel_trak(moov, child, audio_index == 0, name);
        if (!trak) {
            return std::nullopt;
        }
        append(body, *trak);
        ++audio_index;
    }
    return make_box("moov", body);
}

std::vector<AudioTrackInfo> audio_tracks(std::span<const std::uint8_t> moov) {
    std::vector<AudioTrackInfo> tracks;
    const auto header = read_box_header(moov, 0, moov.size());
    if (!header || header->type != "moov" || header->size != moov.size()) {
        return tracks;
    }
    const std::size_t header_size = (read32(moov, 0) == 1) ? kLargeHeaderSize : kHeaderSize;
    const auto top = children(moov, header_size, moov.size());
    if (!top) {
        return tracks;
    }
    for (const Child& child : *top) {
        if (child.type != "trak") {
            continue;
        }
        std::string handler_type;
        const auto hdlr = find_handler(moov, child, handler_type);
        if (!hdlr) {
            return {};
        }
        if (handler_type == "soun") {
            tracks.push_back(AudioTrackInfo{handler_name(moov, *hdlr), sample_description(moov, child)});
        }
    }
    return tracks;
}

}  // namespace bt::mp4
