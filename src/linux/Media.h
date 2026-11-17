#pragma once

// Reading clip files with FFmpeg for the library and the clip page: length and picture size,
// still pictures (the library thumbnail, the filmstrip) and the lossless trim. The Linux side of
// platform/MediaProbe, Filmstrip and Trim. Blocking: call on a worker thread. Nothing throws.

#include <cstdint>
#include <string>
#include <vector>

struct AVFrame;
struct SwsContext;

namespace bt::linux_app {

// Makes `sws` (YUV to RGB) use the frame's own colour matrix and range instead of swscale's
// default BT.601 TV range: Backtrack's clips and most HD video are BT.709, and with the wrong
// matrix every picture is off in colour. Call after the context is (re)made.
void use_frame_colors(SwsContext* sws, const AVFrame* frame);

struct MediaFacts {
    bool ok = false;  // false: not a playable video (damaged, still being written, not an MP4)
    double duration_s = 0.0;
    int width = 0;
    int height = 0;
};

MediaFacts probe_media(const std::string& path);

// One picture, 32-bit pixels in memory order B, G, R, 0xFF (QImage::Format_RGB32), `width` x
// `height`, rows top down without padding. Scaled to fill: the picture keeps its shape and the
// sides that stick out are cut off, as the library cards and the filmstrip show it.
struct Still {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;  // empty: no picture
};

// The picture at `fraction` (0..1) of the clip's length: the keyframe at or before it, so one
// decode (at most a second early in a Backtrack clip).
Still read_still(const std::string& path, double fraction, int width, int height);

// `count` evenly spaced pictures (picture i at (i + 0.5) / count of the length), the same way.
// Fewer (or none) when the file cannot be read.
std::vector<Still> read_filmstrip(const std::string& path, int count, int width, int height);

struct TrimResult {
    bool ok = false;
    std::string error;  // one line for the user
    double start_s = 0.0;  // where the cut really starts in the source (the keyframe at or before in)
    double seconds = 0.0;  // length of the written clip
    std::uint64_t bytes = 0;
};

// Copies the piece from the keyframe at or before `in_s` up to `out_s` into `destination`
// without re-encoding (core/TrimPlan decides which frames). `destination` must not exist yet; it
// is written under a hidden temporary name first, so a failed trim leaves no file behind.
TrimResult trim_clip(const std::string& source, const std::string& destination, double in_s, double out_s);

}  // namespace bt::linux_app
