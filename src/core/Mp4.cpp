#include "core/Mp4.h"

#include <algorithm>
#include <initializer_list>
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

namespace {

// The children of `parent` with this type path below it, e.g. {"mdia", "minf", "stbl"}; empty when
// a box on the way is missing. Empty optional when a box on the way is damaged.
std::optional<std::optional<Child>> descend(std::span<const std::uint8_t> bytes, const Child& parent,
                                            std::initializer_list<const char*> path) {
    std::optional<Child> at = parent;
    for (const char* type : path) {
        const auto list = children(bytes, at->body, at->end);
        if (!list) {
            return std::nullopt;
        }
        const auto it = std::find_if(list->begin(), list->end(), [&](const Child& c) { return c.type == type; });
        if (it == list->end()) {
            return std::optional<Child>{};
        }
        at = *it;
    }
    return at;
}

// Box layouts used by tracks() (ISO/IEC 14496-12).
constexpr std::size_t kMdhdTimescaleV0 = kFullBoxFields + 2 * 4;  // after creation and modification times
constexpr std::size_t kMdhdTimescaleV1 = kFullBoxFields + 2 * 8;
constexpr std::size_t kStszCountAt = kFullBoxFields + 4;  // after sample_size (stz2: reserved + field_size)
constexpr std::size_t kStsdFirstEntryAt = kFullBoxFields + kEntryCountSize;
// AudioSampleEntry: 6 reserved + data_reference_index, then version, revision, vendor, channel
// count, sample size, pre-defined, reserved and the sample rate (16.16 fixed point).
constexpr std::size_t kSoundVersionAt = 8;
constexpr std::size_t kSoundChannelsAt = 16;
constexpr std::size_t kSoundRateAt = 24;
constexpr std::size_t kSoundChildrenAt = 28;  // the boxes inside a version 0 entry (esds) start here
constexpr std::size_t kSoundV1Extra = 16;  // QuickTime version 1 adds four 32-bit fields
// VisualSampleEntry: 8 bytes of SampleEntry and 70 of picture fields before its boxes (avcC);
// avcC: version, profile, compatibility, level, then lengthSizeMinusOne in the low two bits.
constexpr std::size_t kVisualChildrenAt = 78;
constexpr std::size_t kAvcLengthSizeAt = 4;
constexpr std::size_t kMvhdTimescaleV0 = kFullBoxFields + 2 * 4;
constexpr std::size_t kMvhdTimescaleV1 = kFullBoxFields + 2 * 8;
constexpr std::uint8_t kEsDescriptorTag = 0x03;
constexpr std::uint8_t kDecoderConfigTag = 0x04;
constexpr std::int64_t kTicks = 10'000'000;  // 100 ns units per second

// An MPEG-4 descriptor's length: up to four bytes of seven bits each. Empty when it runs past `end`.
std::optional<std::size_t> descriptor_length(std::span<const std::uint8_t> b, std::size_t& at, std::size_t end) {
    std::size_t length = 0;
    for (int i = 0; i < 4; ++i) {
        if (at >= end) {
            return std::nullopt;
        }
        const std::uint8_t byte = b[at++];
        length = (length << 7) | (byte & 0x7F);
        if ((byte & 0x80) == 0) {
            return length;
        }
    }
    return std::nullopt;
}

// esds > ES_Descriptor > DecoderConfigDescriptor > objectTypeIndication; 0 when not found.
std::uint8_t esds_object_type(std::span<const std::uint8_t> b, const Child& esds) {
    std::size_t at = esds.body + kFullBoxFields;
    if (at >= esds.end || b[at++] != kEsDescriptorTag) {
        return 0;
    }
    const auto es_length = descriptor_length(b, at, esds.end);
    constexpr std::size_t kEsIdAndFlags = 3;
    if (!es_length || at + kEsIdAndFlags > esds.end) {
        return 0;
    }
    const std::uint8_t flags = b[at + 2];
    at += kEsIdAndFlags;
    if ((flags & 0x80) != 0) {
        at += 2;  // dependsOn_ES_ID
    }
    if ((flags & 0x40) != 0) {
        if (at >= esds.end) {
            return 0;
        }
        at += 1 + std::size_t{b[at]};  // URL
    }
    if ((flags & 0x20) != 0) {
        at += 2;  // OCR_ES_Id
    }
    if (at >= esds.end || b[at++] != kDecoderConfigTag) {
        return 0;
    }
    const auto config_length = descriptor_length(b, at, esds.end);
    if (!config_length || *config_length == 0 || at >= esds.end) {
        return 0;
    }
    return b[at];
}

// value / timescale seconds in 100 ns units, without overflowing for any value below 100 days.
std::int64_t ticks_of(std::uint64_t value, std::uint32_t timescale) {
    return static_cast<std::int64_t>(value / timescale) * kTicks +
           static_cast<std::int64_t>((value % timescale) * static_cast<std::uint64_t>(kTicks) / timescale);
}

// Reads the edit list into info.edit_shift. Only the shapes recorders write are understood: any
// number of empty edits (media_time -1), then at most one piece played at normal speed.
void read_edits(std::span<const std::uint8_t> b, const Child& elst, std::uint32_t movie_timescale, TrackInfo& info) {
    const std::size_t size = elst.end - elst.body;
    if (size < kFullBoxFields + kEntryCountSize) {
        info.edits_understood = false;
        return;
    }
    const bool wide = b[elst.body] == 1;
    const std::size_t entry_size = wide ? 20 : 12;
    const std::uint32_t count = read32(b, elst.body + kFullBoxFields);
    if (static_cast<std::uint64_t>(count) * entry_size > size - kFullBoxFields - kEntryCountSize) {
        info.edits_understood = false;
        return;
    }
    // Far beyond any clip (100 days); keeps the 100 ns conversion inside int64.
    constexpr std::uint64_t kMaxSeconds = 100ull * 24 * 3600;
    std::uint64_t empty = 0;  // in the movie's timescale
    bool played = false;
    std::int64_t shift = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t at = elst.body + kFullBoxFields + kEntryCountSize + i * entry_size;
        const std::uint64_t duration = wide ? read64(b, at) : read32(b, at);
        const auto media_time = wide ? static_cast<std::int64_t>(read64(b, at + 8))
                                     : static_cast<std::int64_t>(static_cast<std::int32_t>(read32(b, at + 4)));
        const std::size_t rate_at = at + (wide ? 16 : 8);
        const auto rate = static_cast<std::int16_t>((b[rate_at] << 8) | b[rate_at + 1]);
        if (played) {
            info.edits_understood = false;  // a second piece: cut and joined, not a plain shift
            return;
        }
        if (media_time == -1) {
            if (movie_timescale == 0 || duration / movie_timescale > kMaxSeconds) {
                info.edits_understood = false;
                return;
            }
            empty += duration;
            if (empty / movie_timescale > kMaxSeconds) {
                info.edits_understood = false;
                return;
            }
            continue;
        }
        if (rate != 1 || media_time < 0 || info.timescale == 0 ||
            static_cast<std::uint64_t>(media_time) / info.timescale > kMaxSeconds) {
            info.edits_understood = false;
            return;
        }
        played = true;
        shift = -ticks_of(static_cast<std::uint64_t>(media_time), info.timescale);
        // A piece that stops well before the media does is a trim of the end, which a copy of every
        // packet would undo. (0 is "to the end" in fragmented files.)
        constexpr std::int64_t kEndSlack = 1'000'000;  // 100 ms
        if (duration != 0 && info.duration > static_cast<std::uint64_t>(media_time) && movie_timescale != 0 &&
            duration / movie_timescale <= kMaxSeconds && info.duration / info.timescale <= kMaxSeconds &&
            ticks_of(duration, movie_timescale) + kEndSlack <
                ticks_of(info.duration - static_cast<std::uint64_t>(media_time), info.timescale)) {
            info.edits_understood = false;
            return;
        }
    }
    if (empty > 0) {
        shift += ticks_of(empty, movie_timescale);
    }
    info.edit_shift = shift;
}

}  // namespace

std::optional<std::vector<TrackInfo>> tracks(std::span<const std::uint8_t> moov) {
    const auto header = read_box_header(moov, 0, moov.size());
    if (!header || header->type != "moov" || header->size != moov.size()) {
        return std::nullopt;
    }
    const std::size_t header_size = (read32(moov, 0) == 1) ? kLargeHeaderSize : kHeaderSize;
    const auto top = children(moov, header_size, moov.size());
    if (!top) {
        return std::nullopt;
    }
    std::uint32_t movie_timescale = 0;
    for (const Child& child : *top) {
        if (child.type == "mvhd" && child.end - child.body >= kFullBoxFields) {
            const std::size_t at = moov[child.body] == 1 ? kMvhdTimescaleV1 : kMvhdTimescaleV0;
            if (child.end - child.body >= at + 4) {
                movie_timescale = read32(moov, child.body + at);
            }
        }
    }
    std::vector<TrackInfo> list;
    for (const Child& trak : *top) {
        if (trak.type != "trak") {
            continue;
        }
        TrackInfo info;
        const auto hdlr = find_handler(moov, trak, info.handler);
        if (!hdlr) {
            return std::nullopt;
        }
        info.name = handler_name(moov, *hdlr);
        const auto tkhd = descend(moov, trak, {"tkhd"});
        const auto mdhd = descend(moov, trak, {"mdia", "mdhd"});
        const auto stbl = descend(moov, trak, {"mdia", "minf", "stbl"});
        if (!tkhd || !mdhd || !stbl) {
            return std::nullopt;
        }
        if (*tkhd && (*tkhd)->end - (*tkhd)->body >= kFullBoxFields) {
            info.enabled = (moov[(*tkhd)->body + 3] & kTkhdEnabled) != 0;
        }
        if (*mdhd && (*mdhd)->end - (*mdhd)->body >= kFullBoxFields) {
            const std::size_t at = moov[(*mdhd)->body] == 1 ? kMdhdTimescaleV1 : kMdhdTimescaleV0;
            const bool wide = moov[(*mdhd)->body] == 1;
            if ((*mdhd)->end - (*mdhd)->body >= at + 4 + (wide ? 8 : 4)) {
                info.timescale = read32(moov, (*mdhd)->body + at);
                info.duration = wide ? read64(moov, (*mdhd)->body + at + 4) : read32(moov, (*mdhd)->body + at + 4);
            }
        }
        // After mdhd: the media timescale is needed to read the edit list.
        const auto elst = descend(moov, trak, {"edts", "elst"});
        if (!elst) {
            return std::nullopt;
        }
        if (*elst) {
            read_edits(moov, **elst, movie_timescale, info);
        }
        if (*stbl) {
            const auto parts = children(moov, (*stbl)->body, (*stbl)->end);
            if (!parts) {
                return std::nullopt;
            }
            for (const Child& part : *parts) {
                const std::size_t size = part.end - part.body;
                if ((part.type == "stsz" || part.type == "stz2") && size >= kStszCountAt + 4) {
                    info.samples = read32(moov, part.body + kStszCountAt);
                } else if (part.type == "ctts") {
                    info.reordered = true;
                } else if (part.type == "stsd" && info.sample_description.empty()) {
                    info.sample_description.assign(moov.begin() + static_cast<std::ptrdiff_t>(part.begin),
                                                   moov.begin() + static_cast<std::ptrdiff_t>(part.end));
                    const auto entries = size >= kStsdFirstEntryAt ? children(moov, part.body + kStsdFirstEntryAt, part.end)
                                                                    : std::nullopt;
                    if (entries && !entries->empty()) {
                        const Child& entry = entries->front();
                        info.codec = entry.type;
                        if ((info.codec == "avc1" || info.codec == "avc3") && entry.end - entry.body > kVisualChildrenAt) {
                            const auto boxes = children(moov, entry.body + kVisualChildrenAt, entry.end);
                            for (const Child& box : boxes.value_or(std::vector<Child>{})) {
                                if (box.type == "avcC" && box.end - box.body > kAvcLengthSizeAt) {
                                    info.nal_length_size = static_cast<std::uint8_t>((moov[box.body + kAvcLengthSizeAt] & 0x03) + 1);
                                }
                            }
                        }
                        if (info.handler == "soun" && entry.end - entry.body >= kSoundRateAt + 4 &&
                            read32(moov, entry.body + kSoundVersionAt) >> 16 < 2) {
                            const std::uint32_t version = read32(moov, entry.body + kSoundVersionAt) >> 16;
                            info.channels = static_cast<std::uint16_t>(read32(moov, entry.body + kSoundChannelsAt) >> 16);
                            info.sample_rate = read32(moov, entry.body + kSoundRateAt) >> 16;
                            const std::size_t inner = entry.body + kSoundChildrenAt + (version == 1 ? kSoundV1Extra : 0);
                            const auto boxes = inner <= entry.end ? children(moov, inner, entry.end) : std::nullopt;
                            if (boxes) {
                                for (const Child& box : *boxes) {
                                    if (box.type == "esds") {
                                        info.object_type = esds_object_type(moov, box);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        list.push_back(std::move(info));
    }
    return list;
}

std::optional<std::vector<std::uint8_t>> moov_child(std::span<const std::uint8_t> moov, const std::string& type) {
    const auto header = read_box_header(moov, 0, moov.size());
    if (!header || header->type != "moov" || header->size != moov.size()) {
        return std::nullopt;
    }
    const std::size_t header_size = (read32(moov, 0) == 1) ? kLargeHeaderSize : kHeaderSize;
    const auto top = children(moov, header_size, moov.size());
    if (!top) {
        return std::nullopt;
    }
    for (const Child& child : *top) {
        if (child.type == type) {
            std::vector<std::uint8_t> copy(moov.begin() + static_cast<std::ptrdiff_t>(child.begin),
                                           moov.begin() + static_cast<std::ptrdiff_t>(child.end));
            // Size 0 means "to the end of the parent"; copied elsewhere (in front of the media data)
            // it would swallow the rest of the file, so it gets its real size.
            if (read32(copy, 0) == 0) {
                if (copy.size() > std::numeric_limits<std::uint32_t>::max()) {
                    return std::nullopt;
                }
                write32(copy, 0, static_cast<std::uint32_t>(copy.size()));
            }
            return copy;
        }
    }
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> replace_moov_child(std::span<const std::uint8_t> moov,
                                                            std::span<const std::uint8_t> box) {
    const auto header = read_box_header(moov, 0, moov.size());
    const auto added = read_box_header(box, 0, box.size());
    if (!header || header->type != "moov" || header->size != moov.size() || !added || added->size != box.size()) {
        return std::nullopt;
    }
    const std::size_t header_size = (read32(moov, 0) == 1) ? kLargeHeaderSize : kHeaderSize;
    const auto top = children(moov, header_size, moov.size());
    if (!top) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> body;
    for (const Child& child : *top) {
        if (child.type != added->type) {
            append(body, moov.subspan(child.begin, child.end - child.begin));
        }
    }
    append(body, box);
    return make_box("moov", body);
}

std::optional<std::vector<SampleLocation>> sample_locations(std::span<const std::uint8_t> moov, std::size_t track,
                                                            std::uint64_t file_size) {
    // More samples than any clip has (46 hours at 60 fps); a table claiming more is
    // damaged, and must not make this allocate gigabytes.
    constexpr std::uint32_t kMaxSamples = 10'000'000;
    const auto header = read_box_header(moov, 0, moov.size());
    if (!header || header->type != "moov" || header->size != moov.size()) {
        return std::nullopt;
    }
    const std::size_t header_size = (read32(moov, 0) == 1) ? kLargeHeaderSize : kHeaderSize;
    const auto top = children(moov, header_size, moov.size());
    if (!top) {
        return std::nullopt;
    }
    std::optional<Child> trak;
    std::size_t index = 0;
    for (const Child& child : *top) {
        if (child.type == "trak" && index++ == track) {
            trak = child;
            break;
        }
    }
    if (!trak) {
        return std::nullopt;
    }
    const auto stbl = descend(moov, *trak, {"mdia", "minf", "stbl"});
    if (!stbl || !*stbl) {
        return std::nullopt;
    }
    const auto parts = children(moov, (*stbl)->body, (*stbl)->end);
    if (!parts) {
        return std::nullopt;
    }
    const auto find = [&](const char* type) -> std::optional<Child> {
        const auto it = std::find_if(parts->begin(), parts->end(), [&](const Child& c) { return c.type == type; });
        return it == parts->end() ? std::nullopt : std::optional<Child>(*it);
    };
    const auto stsz = find("stsz");
    const auto stz2 = find("stz2");
    const auto stsc = find("stsc");
    const auto stco = find("stco");
    const auto co64 = find("co64");
    if ((!stsz && !stz2) || !stsc || (!stco && !co64)) {
        return std::nullopt;
    }

    // Sample sizes.
    const Child& sizes = stsz ? *stsz : *stz2;
    if (sizes.end - sizes.body < kStszCountAt + 4) {
        return std::nullopt;
    }
    const std::uint32_t count = read32(moov, sizes.body + kStszCountAt);
    const std::size_t first_size = sizes.body + kStszCountAt + 4;
    const std::size_t room = sizes.end - first_size;
    std::uint32_t constant = 0;
    unsigned field_bits = 32;
    if (stsz) {
        constant = read32(moov, sizes.body + kFullBoxFields);
    } else {
        field_bits = moov[sizes.body + kFullBoxFields + 3];
        if (field_bits != 4 && field_bits != 8 && field_bits != 16) {
            return std::nullopt;
        }
    }
    if (count > kMaxSamples || (constant == 0 && static_cast<std::uint64_t>(count) * field_bits > room * 8ull)) {
        return std::nullopt;
    }
    const auto size_of = [&](std::uint32_t i) -> std::uint32_t {
        if (constant != 0) return constant;
        switch (field_bits) {
            case 4: {
                const std::uint8_t byte = moov[first_size + i / 2];
                return (i % 2 == 0) ? byte >> 4 : byte & 0x0F;
            }
            case 8: return moov[first_size + i];
            case 16: return static_cast<std::uint32_t>(moov[first_size + 2 * i] << 8 | moov[first_size + 2 * i + 1]);
            default: return read32(moov, first_size + 4 * static_cast<std::size_t>(i));
        }
    };

    // Chunk offsets.
    const Child& offsets = stco ? *stco : *co64;
    const std::size_t offset_size = stco ? 4 : 8;
    if (offsets.end - offsets.body < kFullBoxFields + kEntryCountSize) {
        return std::nullopt;
    }
    const std::uint32_t chunks = read32(moov, offsets.body + kFullBoxFields);
    const std::size_t first_offset = offsets.body + kFullBoxFields + kEntryCountSize;
    if (static_cast<std::uint64_t>(chunks) * offset_size > offsets.end - first_offset) {
        return std::nullopt;
    }

    // Samples per chunk: runs of chunks, each from its first chunk (counted from 1) to the next run.
    constexpr std::size_t kStscEntry = 12;
    if (stsc->end - stsc->body < kFullBoxFields + kEntryCountSize) {
        return std::nullopt;
    }
    const std::uint32_t runs = read32(moov, stsc->body + kFullBoxFields);
    const std::size_t first_run = stsc->body + kFullBoxFields + kEntryCountSize;
    if (static_cast<std::uint64_t>(runs) * kStscEntry > stsc->end - first_run || (runs == 0 && count > 0)) {
        return std::nullopt;
    }

    if (constant != 0 && static_cast<std::uint64_t>(count) * constant > file_size) {
        return std::nullopt;
    }
    std::vector<SampleLocation> list;
    list.reserve(std::min<std::uint32_t>(count, 1u << 20));
    std::uint32_t sample = 0;
    for (std::uint32_t run = 0; run < runs && sample < count; ++run) {
        const std::size_t at = first_run + run * kStscEntry;
        const std::uint32_t first_chunk = read32(moov, at);
        const std::uint32_t per_chunk = read32(moov, at + 4);
        const std::uint32_t next_chunk = run + 1 < runs ? read32(moov, at + kStscEntry) : chunks + 1;
        if (first_chunk == 0 || next_chunk <= first_chunk || next_chunk > chunks + 1) {
            return std::nullopt;
        }
        for (std::uint32_t chunk = first_chunk; chunk < next_chunk && sample < count; ++chunk) {
            const std::size_t entry = first_offset + (chunk - 1) * offset_size;
            std::uint64_t offset = stco ? read32(moov, entry) : read64(moov, entry);
            for (std::uint32_t i = 0; i < per_chunk && sample < count; ++i, ++sample) {
                const std::uint32_t size = size_of(sample);
                if (offset > file_size || size > file_size - offset) {
                    return std::nullopt;
                }
                list.push_back(SampleLocation{offset, size});
                offset += size;
            }
        }
    }
    if (sample != count) {
        return std::nullopt;  // the chunks hold fewer samples than the size table
    }
    return list;
}

}  // namespace bt::mp4
