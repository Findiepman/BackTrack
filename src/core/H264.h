#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace bt::h264 {

// NAL unit types (ITU-T H.264 table 7-1) that matter for a replay buffer.
inline constexpr std::uint8_t kNalSlice = 1;
inline constexpr std::uint8_t kNalIdrSlice = 5;
inline constexpr std::uint8_t kNalSei = 6;
inline constexpr std::uint8_t kNalSps = 7;
inline constexpr std::uint8_t kNalPps = 8;
inline constexpr std::uint8_t kNalAccessUnitDelimiter = 9;

struct NalUnit {
    std::uint8_t type = 0;
    std::span<const std::uint8_t> payload;  // NAL header byte onwards, start code and padding excluded
};

// Splits an Annex B byte stream (what Media Foundation H.264 encoders output) at its
// 00 00 01 / 00 00 00 01 start codes. Bytes before the first start code are ignored.
std::vector<NalUnit> split_annex_b(std::span<const std::uint8_t> stream);

struct AccessUnitInfo {
    bool has_sps = false;
    bool has_pps = false;
    bool has_idr = false;
    int slices = 0;  // IDR and non-IDR slices
};

AccessUnitInfo inspect(std::span<const std::uint8_t> access_unit);

// The SPS and PPS NAL units of a keyframe, each behind a 00 00 00 01 start code: the form
// Media Foundation expects in MF_MT_MPEG_SEQUENCE_HEADER. Empty when either is missing.
std::vector<std::uint8_t> parameter_sets(std::span<const std::uint8_t> keyframe);

}  // namespace bt::h264
