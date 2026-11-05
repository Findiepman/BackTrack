#pragma once

#include "core/Clock.h"

#include <cstddef>
#include <span>

namespace bt {

// One video frame of the file being trimmed, in the order the file stores them.
struct TrimFrame {
    Time100ns pts = 0;
    bool keyframe = false;
};

enum class TrimProblem {
    None,
    NoVideo,  // the file has no video frames
    NoKeyframe,  // no frame a player could start decoding from
    Reordered,  // timestamps go backwards: B-frames, which a cut without re-encoding cannot handle
    EmptyRange,  // out <= in, or nothing of the video lies before out
    StartPastEnd,  // in lies after the last frame
};

struct TrimPlan {
    TrimProblem problem = TrimProblem::None;
    std::size_t first = 0;  // first video frame of the cut: always a keyframe
    std::size_t end = 0;  // one past the last video frame (the last one with pts < out)
    Time100ns start = 0;  // pts of the first frame: where the cut really starts
};

// Which video frames a cut from `in` to `out` keeps without re-encoding. It has to start on a
// keyframe, so it starts on the last keyframe at or before `in` (or the first keyframe when `in`
// lies before it). The end is exact: every frame with pts < out. An `out` past the end keeps
// everything to the end.
TrimPlan plan_trim(std::span<const TrimFrame> video, Time100ns in, Time100ns out);

struct TrimRange {
    std::size_t first = 0;
    std::size_t end = 0;  // one past the last
};

// The audio packets that belong to a cut: pts in [start, out). Sound that starts before the first
// video frame is left out, so the picture and the sound keep their exact offset.
TrimRange trim_audio(std::span<const Time100ns> audio_pts, Time100ns start, Time100ns out);

// While a file is read front to back, everything before the last keyframe at or before `in` can
// be let go: plan_trim never starts before it. True when `frame` is such a keyframe.
inline bool trim_restarts_at(const TrimFrame& frame, Time100ns in) {
    return frame.keyframe && frame.pts <= in;
}

}  // namespace bt
