#include "core/ClipPlan.h"

#include <array>

namespace bt {
namespace {

// Longer than any path Windows accepts without the \\?\ prefix; strftime returns 0 beyond it.
constexpr std::size_t kMaxExpandedName = 260;

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
            plan.samples.push_back(ClipSample{false, v, p.pts - base, p.duration, p.keyframe});
            plan.video_duration = p.pts - base + p.duration;
            ++plan.video_samples;
            ++v;
        } else {
            const Packet& p = audio[a];
            plan.samples.push_back(ClipSample{true, a, p.pts - base, p.duration, true});
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
