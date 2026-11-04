#include "core/H264.h"

#include "Check.h"

#include <vector>

using namespace bt;

namespace {

void splits_three_and_four_byte_start_codes() {
    // AUD, SPS, PPS, IDR with mixed start codes, as AMD's encoder emits for a keyframe.
    const std::vector<std::uint8_t> au = {
        0, 0, 0, 1, 0x09, 0x10,                    // AUD
        0, 0, 0, 1, 0x67, 0x64, 0x00, 0x28,        // SPS
        0, 0, 1, 0x68, 0xEE, 0x3C, 0x80,           // PPS
        0, 0, 1, 0x65, 0x88, 0x84, 0x00, 0x33,     // IDR slice
    };
    const auto units = h264::split_annex_b(au);
    CHECK_EQ(units.size(), std::size_t{4});
    CHECK_EQ(units[0].type, h264::kNalAccessUnitDelimiter);
    CHECK_EQ(units[1].type, h264::kNalSps);
    CHECK_EQ(units[1].payload.size(), std::size_t{4});
    CHECK_EQ(units[2].type, h264::kNalPps);
    CHECK_EQ(units[2].payload.size(), std::size_t{4});  // trailing zero of the next 4-byte code not included
    CHECK_EQ(units[3].type, h264::kNalIdrSlice);
    CHECK_EQ(units[3].payload.back(), std::uint8_t{0x33});

    const h264::AccessUnitInfo info = h264::inspect(au);
    CHECK(info.has_sps && info.has_pps && info.has_idr);
    CHECK_EQ(info.slices, 1);

    const std::vector<std::uint8_t> expected = {0, 0, 0, 1, 0x67, 0x64, 0x00, 0x28, 0, 0, 0, 1, 0x68, 0xEE, 0x3C, 0x80};
    CHECK(h264::parameter_sets(au) == expected);
    // A P-frame has no parameter sets.
    CHECK(h264::parameter_sets(std::vector<std::uint8_t>{0, 0, 1, 0x41, 0x9A}).empty());
    // SPS without PPS is not usable.
    CHECK(h264::parameter_sets(std::vector<std::uint8_t>{0, 0, 1, 0x67, 0x64}).empty());
}

void p_frame_and_noise() {
    const std::vector<std::uint8_t> p = {0x12, 0x34, 0, 0, 1, 0x41, 0x9A, 0x02};  // junk before the start code
    const auto units = h264::split_annex_b(p);
    CHECK_EQ(units.size(), std::size_t{1});
    CHECK_EQ(units[0].type, h264::kNalSlice);
    const h264::AccessUnitInfo info = h264::inspect(p);
    CHECK(!info.has_idr && !info.has_sps);
    CHECK_EQ(info.slices, 1);

    CHECK(h264::split_annex_b(std::vector<std::uint8_t>{}).empty());
    CHECK(h264::split_annex_b(std::vector<std::uint8_t>{0, 0, 1}).empty());
    CHECK(h264::split_annex_b(std::vector<std::uint8_t>{0, 0, 0, 0}).empty());
    // Multi-slice frame.
    const std::vector<std::uint8_t> two = {0, 0, 1, 0x65, 1, 0, 0, 1, 0x65, 2};
    CHECK_EQ(h264::inspect(two).slices, 2);
}

void emulation_prevention_and_trailing_zeros() {
    // 00 00 03 inside a NAL is emulation prevention, never a start code, and stays in the payload.
    // trailing_zero_8bits after a NAL belong to no unit.
    const std::vector<std::uint8_t> au = {
        0, 0, 1, 0x65, 0x11, 0, 0, 3, 0, 0, 3, 1, 0x22, 0, 0,  // IDR containing 00 00 03 twice, then two zero bytes
        0, 0, 0, 1, 0x41, 0x33, 0, 0, 0,                       // P slice followed by trailing zeros at the end
    };
    const auto units = h264::split_annex_b(au);
    CHECK_EQ(units.size(), std::size_t{2});
    CHECK_EQ(units[0].type, h264::kNalIdrSlice);
    CHECK_EQ(units[0].payload.size(), std::size_t{10});  // 65 11 00 00 03 00 00 03 01 22
    CHECK_EQ(units[0].payload.back(), std::uint8_t{0x22});
    CHECK_EQ(units[1].type, h264::kNalSlice);
    CHECK_EQ(units[1].payload.size(), std::size_t{2});
}

void levels() {
    // Values checked against table A-1 by hand.
    CHECK_EQ(h264::required_level(1920, 1080, 30, 8000), 40);
    CHECK_EQ(h264::required_level(1920, 1080, 60, 20000), 42);
    CHECK_EQ(h264::required_level(1920, 1080, 60, 70000), 50);    // 62.5 Mbit/s is the 4.2 High limit
    CHECK_EQ(h264::required_level(1920, 1080, 120, 20000), 51);   // 979,200 MB/s fits 5.1
    CHECK_EQ(h264::required_level(1920, 1080, 144, 20000), 52);   // AMD signalled 5.1 here (measured)
    CHECK_EQ(h264::required_level(2560, 1440, 144, 40000), 52);
    CHECK_EQ(h264::required_level(3440, 1440, 144, 50000), 60);
    CHECK_EQ(h264::required_level(3840, 2160, 60, 50000), 52);
    CHECK_EQ(h264::required_level(3840, 2160, 144, 100000), 61);
    CHECK_EQ(h264::required_level(1024, 600, 60, 20000), 32);
    CHECK_EQ(h264::required_level(7680, 4320, 144, 100000), 0);    // beyond 6.2
}

}  // namespace

int main() {
    levels();
    splits_three_and_four_byte_start_codes();
    p_frame_and_noise();
    emulation_prevention_and_trailing_zeros();
    return bt_test::finish("H264Tests");
}
