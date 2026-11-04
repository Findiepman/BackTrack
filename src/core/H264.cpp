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

}  // namespace bt::h264
