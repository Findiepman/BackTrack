#include "core/ClipPlan.h"

#include <array>

namespace bt {
namespace {

// Longer than any path Windows accepts without the \\?\ prefix; strftime returns 0 beyond it.
constexpr std::size_t kMaxExpandedName = 260;

// A sample lasts until the next one starts. A frame the encoder dropped (or a tick the capture
// skipped) leaves a gap in the timestamps; if the durations still said 1/fps, an MP4 writer that
// builds its timing from durations would close every gap and the picture would run ahead of the
// sound. The last sample keeps its own duration.
Time100ns span_until_next(const std::vector<Packet>& packets, std::size_t i) {
    // A repeated or backwards stamp would give 0 or less, which the MP4 writer refuses; the
    // packet's own duration stands in then.
    const Time100ns span = i + 1 < packets.size() ? packets[i + 1].pts - packets[i].pts : 0;
    return span > 0 ? span : packets[i].duration;
}

}  // namespace

ClipPlan plan_clip(const std::vector<Packet>& video, const std::vector<Packet>& audio) {
    ClipPlan plan;
    if (video.empty() || !video.front().keyframe) {
        return plan;
    }
    const Time100ns base = video.front().pts;

    std::size_t first_audio = 0;
    while (first_audio < audio.size() && audio[first_audio].pts < base) {
        ++first_audio;
    }
    plan.audio_dropped = first_audio;

    plan.samples.reserve(video.size() + audio.size() - first_audio);
    std::size_t v = 0;
    std::size_t a = first_audio;
    while (v < video.size() || a < audio.size()) {
        const bool take_video = a >= audio.size() || (v < video.size() && video[v].pts <= audio[a].pts);
        if (take_video) {
            const Packet& p = video[v];
            const Time100ns duration = span_until_next(video, v);
            plan.samples.push_back(ClipSample{false, v, p.pts - base, duration, p.keyframe});
            plan.video_duration = p.pts - base + duration;
            ++plan.video_samples;
            ++v;
        } else {
            const Packet& p = audio[a];
            plan.samples.push_back(ClipSample{true, a, p.pts - base, span_until_next(audio, a), true});
            ++plan.audio_samples;
            ++a;
        }
        const ClipSample& s = plan.samples.back();
        if (s.time + s.duration > plan.duration) {
            plan.duration = s.time + s.duration;
        }
    }
    return plan;
}

std::string expand_clip_name(std::string_view pattern, const std::tm& time) {
    std::array<char, kMaxExpandedName + 1> buffer{};
    const std::string format(pattern);
    const std::size_t length = std::strftime(buffer.data(), buffer.size(), format.c_str(), &time);
    return std::string(buffer.data(), length);
}

std::string numbered_clip_name(std::string_view name, int number) {
    const std::size_t dot = name.rfind('.');
    const std::string_view stem = dot == std::string_view::npos ? name : name.substr(0, dot);
    const std::string_view extension = dot == std::string_view::npos ? std::string_view{} : name.substr(dot);
    return std::string(stem) + "_" + std::to_string(number) + std::string(extension);
}

}  // namespace bt
