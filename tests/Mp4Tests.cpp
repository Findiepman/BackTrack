#include "core/Mp4.h"

#include "Check.h"

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

}  // namespace

int main() {
    shifts_every_chunk_offset();
    wide_tables_and_overflow();
    refuses_what_it_does_not_understand();
    box_headers();
    return bt_test::finish("Mp4Tests");
}
