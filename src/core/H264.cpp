#include "core/H264.h"

namespace bt::h264 {
namespace {

constexpr std::uint8_t kNalTypeMask = 0x1F;

// Position of the next 00 00 01 at or after `from`, or stream.size().
std::size_t find_start_code(std::span<const std::uint8_t> stream, std::size_t from) {
    for (std::size_t i = from; i + 2 < stream.size(); ++i) {
        if (stream[i] == 0 && stream[i + 1] == 0 && stream[i + 2] == 1) {
            return i;
        }
    }
    return stream.size();
}

}  // namespace

std::vector<NalUnit> split_annex_b(std::span<const std::uint8_t> stream) {
    std::vector<NalUnit> units;
    std::size_t code = find_start_code(stream, 0);
    while (code < stream.size()) {
        const std::size_t begin = code + 3;
        const std::size_t next = find_start_code(stream, begin);
        std::size_t end = next;
        // A 4-byte start code (00 00 00 01) and trailing_zero_bytes leave zeros before the next
        // 00 00 01; they belong to no NAL unit.
        while (end > begin && stream[end - 1] == 0) {
            --end;
        }
        if (end > begin) {
            units.push_back(NalUnit{static_cast<std::uint8_t>(stream[begin] & kNalTypeMask),
                                    stream.subspan(begin, end - begin)});
        }
        code = next;
    }
    return units;
}

AccessUnitInfo inspect(std::span<const std::uint8_t> access_unit) {
    AccessUnitInfo info;
    for (const NalUnit& unit : split_annex_b(access_unit)) {
        switch (unit.type) {
            case kNalSps: info.has_sps = true; break;
            case kNalPps: info.has_pps = true; break;
            case kNalIdrSlice:
                info.has_idr = true;
                ++info.slices;
                break;
            case kNalSlice: ++info.slices; break;
            default: break;
        }
    }
    return info;
}

int required_level(int width, int height, int fps, int bitrate_kbps) {
    struct Level {
        int idc;
        long long max_macroblocks_per_second;
        long long max_frame_macroblocks;
        long long max_kbps;  // Baseline/Main; High allows 1.25x
    };
    // ITU-T H.264 table A-1. Level 1b is left out: it is never the answer for screen video.
    constexpr Level kLevels[] = {
        {10, 1485, 99, 64},           {11, 3000, 396, 192},         {12, 6000, 396, 384},
        {13, 11880, 396, 768},        {20, 11880, 396, 2000},       {21, 19800, 792, 4000},
        {22, 20250, 1620, 4000},      {30, 40500, 1620, 10000},     {31, 108000, 3600, 14000},
        {32, 216000, 5120, 20000},    {40, 245760, 8192, 20000},    {41, 245760, 8192, 50000},
        {42, 522240, 8704, 50000},    {50, 589824, 22080, 135000},  {51, 983040, 36864, 240000},
        {52, 2073600, 36864, 240000}, {60, 4177920, 139264, 240000}, {61, 8355840, 139264, 480000},
        {62, 16711680, 139264, 800000},
    };
    constexpr long long kMacroblock = 16;
    constexpr long long kHighProfileFactorPercent = 125;
    const long long frame_macroblocks =
        ((width + kMacroblock - 1) / kMacroblock) * ((height + kMacroblock - 1) / kMacroblock);
    const long long per_second = frame_macroblocks * fps;
    for (const Level& level : kLevels) {
        if (frame_macroblocks <= level.max_frame_macroblocks && per_second <= level.max_macroblocks_per_second &&
            static_cast<long long>(bitrate_kbps) * 100 <= level.max_kbps * kHighProfileFactorPercent) {
            return level.idc;
        }
    }
    return 0;
}

std::vector<std::uint8_t> parameter_sets(std::span<const std::uint8_t> keyframe) {
    constexpr std::uint8_t kStartCode[] = {0, 0, 0, 1};
    std::vector<std::uint8_t> out;
    bool sps = false;
    bool pps = false;
    for (const NalUnit& unit : split_annex_b(keyframe)) {
        if (unit.type != kNalSps && unit.type != kNalPps) {
            continue;
        }
        sps = sps || unit.type == kNalSps;
        pps = pps || unit.type == kNalPps;
        out.insert(out.end(), std::begin(kStartCode), std::end(kStartCode));
        out.insert(out.end(), unit.payload.begin(), unit.payload.end());
    }
    if (!sps || !pps) {
        out.clear();
    }
    return out;
}

std::optional<std::vector<NalUnit>> split_length_prefixed(std::span<const std::uint8_t> sample) {
    constexpr std::size_t kLengthBytes = 4;
    std::vector<NalUnit> units;
    std::size_t at = 0;
    while (at < sample.size()) {
        if (sample.size() - at < kLengthBytes) {
            return std::nullopt;
        }
        const std::size_t length = std::size_t{sample[at]} << 24 | std::size_t{sample[at + 1]} << 16 |
                                   std::size_t{sample[at + 2]} << 8 | std::size_t{sample[at + 3]};
        at += kLengthBytes;
        if (length == 0 || length > sample.size() - at) {
            return std::nullopt;
        }
        units.push_back(NalUnit{static_cast<std::uint8_t>(sample[at] & kNalTypeMask), sample.subspan(at, length)});
        at += length;
    }
    return units;
}

std::vector<std::uint8_t> parameter_set_bytes(std::span<const NalUnit> units) {
    constexpr std::uint8_t kStartCode[] = {0, 0, 0, 1};
    std::vector<std::uint8_t> out;
    for (const NalUnit& unit : units) {
        if (unit.type == kNalSps || unit.type == kNalPps) {
            out.insert(out.end(), std::begin(kStartCode), std::end(kStartCode));
            out.insert(out.end(), unit.payload.begin(), unit.payload.end());
        }
    }
    return out;
}

}  // namespace bt::h264
