#pragma once

#include "core/ClipBuffer.h"

#include <cstddef>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace bt {

struct ClipSample {
    bool audio = false;
    std::size_t index = 0;  // into the video or audio packet list
    Time100ns time = 0;  // rebased: the first video keyframe is at 0
    Time100ns duration = 0;
    bool keyframe = false;
};

struct ClipPlan {
    std::vector<ClipSample> samples;  // in write order: interleaved by time, video first on a tie
    std::size_t video_samples = 0;
    std::size_t audio_samples = 0;
    std::size_t audio_dropped = 0;  // audio packets that start before the first video frame
    Time100ns video_duration = 0;  // end of the last video frame
    Time100ns duration = 0;  // end of the last sample of either stream
};

// §7: what goes into one clip file, in which order, at which time. Empty (no samples) when the
// video list is empty or does not start on a keyframe, which a player could not decode.
// Audio that starts before the first video frame is dropped; audio after the last video frame
// is kept so the sound does not cut off early.
ClipPlan plan_clip(const std::vector<Packet>& video, const std::vector<Packet>& audio);

// Expands the strftime placeholders of the `filename` setting. Config only lets placeholders
// through that are safe in a file name. Returns an empty string when the result would be empty
// or too long.
std::string expand_clip_name(std::string_view pattern, const std::tm& time);

// "clip.mp4", 2 -> "clip_2.mp4": the name to try when `clip.mp4` already exists, so two saves
// in the same second both survive.
std::string numbered_clip_name(std::string_view name, int number);

}  // namespace bt
