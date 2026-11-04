#pragma once

#include <cstdint>

namespace bt {

// Every timestamp in Backtrack is in 100 ns units on the QueryPerformanceCounter timeline (§3.3).
// 100 ns is Media Foundation's native sample time unit, so nothing is ever converted twice.
using Time100ns = std::int64_t;

inline constexpr Time100ns kTicksPerSecond = 10'000'000;
inline constexpr Time100ns kTicksPerMillisecond = 10'000;

// Converts a raw performance counter reading. Split into whole seconds and a remainder so
// counter * 10^7 cannot overflow int64 even after months of uptime.
constexpr Time100ns counter_to_100ns(std::int64_t counter, std::int64_t frequency) {
    const std::int64_t whole_seconds = counter / frequency;
    const std::int64_t remainder = counter % frequency;
    return whole_seconds * kTicksPerSecond + remainder * kTicksPerSecond / frequency;
}

// Start of frame `index` relative to frame 0. Computed from the index instead of adding up
// durations because 10^7 / 60 is not a whole number: a running sum drifts by a frame per hour.
constexpr Time100ns frame_offset(std::int64_t index, int fps) {
    return index * kTicksPerSecond / fps;
}

constexpr Time100ns seconds_to_100ns(std::int64_t seconds) {
    return seconds * kTicksPerSecond;
}

}  // namespace bt
