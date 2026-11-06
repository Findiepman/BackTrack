#pragma once

#include <cstdint>
#include <optional>
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

// The lowest H.264 level (level_idc: 31 = 3.1, 52 = 5.2, 60 = 6.0) whose limits (ITU-T H.264
// table A-1, High profile bitrate factor 1.25) cover this resolution, frame rate and bitrate.
// Encoders left to choose signal too low a level (AMD: 5.1 for 1080p144, which needs 5.2), and
// phone hardware decoders then refuse or stutter. 0 when even level 6.2 is not enough.
int required_level(int width, int height, int fps, int bitrate_kbps);

// The SPS and PPS NAL units of a keyframe, each behind a 00 00 00 01 start code: the form
// Media Foundation expects in MF_MT_MPEG_SEQUENCE_HEADER. Empty when either is missing.
std::vector<std::uint8_t> parameter_sets(std::span<const std::uint8_t> keyframe);

// Splits one sample as an MP4 file stores it: NAL units each behind a 4-byte big-endian length
// (avcC lengthSizeMinusOne = 3). Empty optional when the lengths do not add up to the sample.
std::optional<std::vector<NalUnit>> split_length_prefixed(std::span<const std::uint8_t> sample);

// Every SPS and PPS among `units`, in order, each behind 00 00 00 01 (the form of parameter_sets);
// empty when there is none. Unlike parameter_sets, an SPS without a PPS is returned too.
std::vector<std::uint8_t> parameter_set_bytes(std::span<const NalUnit> units);

}  // namespace bt::h264
