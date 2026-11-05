#pragma once

// Cuts a piece out of a clip without re-encoding: the compressed frames are copied as they are,
// so a trim takes about as long as copying the bytes and the picture does not lose any quality.
// The price: the cut can only start on a keyframe (Backtrack clips have one every second), so it
// starts on the last keyframe at or before the requested start. The end is exact.

#include <cstdint>
#include <filesystem>
#include <string>

namespace bt {

struct TrimRequest {
    std::filesystem::path source;
    std::filesystem::path destination;  // must not exist yet; written as <destination>.tmp first
    double in_s = 0.0;  // on the source's timeline, in seconds
    double out_s = 0.0;
};

struct TrimResult {
    bool ok = false;
    std::string error;  // one line for the user
    double start_s = 0.0;  // where the cut really starts in the source (the keyframe at or before in_s)
    double seconds = 0.0;  // length of the written clip
    std::uint64_t bytes = 0;
};

// Blocking: reads the source up to out_s and writes the destination. Call it on a worker thread
// with COM (MTA) initialised and MFStartup done. Never throws; on failure no destination file is
// left behind.
TrimResult trim_clip(const TrimRequest& request);

}  // namespace bt
