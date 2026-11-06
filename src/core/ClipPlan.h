#pragma once

#include "core/ClipBuffer.h"

#include <cstddef>
#include <ctime>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bt {

struct ClipSample {
    bool audio = false;
    std::size_t track = 0;  // which audio track (audio samples only)
    std::size_t index = 0;  // into the video or that audio track's packet list
    Time100ns time = 0;  // rebased: the first video keyframe is at 0
    Time100ns duration = 0;
    bool keyframe = false;
};

struct ClipPlan {
    std::vector<ClipSample> samples;  // in write order: interleaved by time, video first on a tie
    std::size_t video_samples = 0;
    std::size_t audio_samples = 0;  // of all audio tracks together
    std::vector<std::size_t> track_samples;  // per audio track
    std::size_t audio_dropped = 0;  // audio packets that start before the first video frame (all tracks)
    Time100ns video_duration = 0;  // end of the last video frame
    Time100ns duration = 0;  // end of the last sample of either stream
};

// §7: what goes into one clip file, in which order, at which time. Empty (no samples) when the
// video list is empty or does not start on a keyframe, which a player could not decode.
// Audio that starts before the first video frame is dropped; audio after the last video frame
// is kept so the sound does not cut off early.
ClipPlan plan_clip(const std::vector<Packet>& video, const std::vector<Packet>& audio);

// The same with any number of audio tracks (separate tracks: the mix, the system sound, the
// microphone), each handled exactly like the one above. On equal times video comes first, then the
// tracks in their order.
ClipPlan plan_clip_tracks(const std::vector<Packet>& video, std::span<const std::vector<Packet>> audio_tracks);

// Expands the strftime placeholders of the `filename` setting. Config only lets placeholders
// through that are safe in a file name. Returns an empty string when the result would be empty
// or too long.
std::string expand_clip_name(std::string_view pattern, const std::tm& time);

// "clip.mp4", 2 -> "clip_2.mp4": the name to try when `clip.mp4` already exists, so two saves
// in the same second both survive.
std::string numbered_clip_name(std::string_view name, int number);

}  // namespace bt
