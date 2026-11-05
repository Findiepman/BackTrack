#pragma once

// The row of small pictures under the trim bar.

#include <cstdint>
#include <filesystem>
#include <vector>

namespace bt {

// `count` evenly spaced frames, each BGRA width x height (top-down rows, stride width*4), scaled
// to fill (crop to keep aspect). Empty vector on failure. Blocking; worker thread with COM (MTA)
// initialised and MFStartup done.
//
// Each picture is the keyframe at or before its position (frame i sits at (i + 0.5) / count of
// the length), which is one decode per picture: fast, and at most a GOP (one second in a
// Backtrack clip) early.
std::vector<std::vector<std::uint8_t>> extract_frames(const std::filesystem::path& file, int count, int width,
                                                      int height);

}  // namespace bt
