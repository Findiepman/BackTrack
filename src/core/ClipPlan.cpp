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
    return plan_clip_tracks(video, std::span<const std::vector<Packet>>(&audio, 1));
}

ClipPlan plan_clip_tracks(const std::vector<Packet>& video, std::span<const std::vector<Packet>> audio_tracks) {
    ClipPlan plan;
    plan.track_samples.assign(audio_tracks.size(), 0);
    if (video.empty() || !video.front().keyframe) {
        return plan;
    }
    const Time100ns base = video.front().pts;

    // Next packet of each audio track; the ones before the first video frame are skipped.
    std::vector<std::size_t> next(audio_tracks.size(), 0);
    std::size_t total = video.size();
    for (std::size_t t = 0; t < audio_tracks.size(); ++t) {
        const std::vector<Packet>& audio = audio_tracks[t];
        while (next[t] < audio.size() && audio[next[t]].pts < base) {
            ++next[t];
        }
        plan.audio_dropped += next[t];
        total += audio.size() - next[t];
    }

    plan.samples.reserve(total);
    std::size_t v = 0;
    while (true) {
        // The earliest head: video on a tie, then the lower track. At most a few tracks, so a
        // plain scan per sample.
        constexpr std::size_t kNone = static_cast<std::size_t>(-1);
        std::size_t track = kNone;
        for (std::size_t t = 0; t < audio_tracks.size(); ++t) {
            if (next[t] < audio_tracks[t].size() &&
                (track == kNone || audio_tracks[t][next[t]].pts < audio_tracks[track][next[track]].pts)) {
                track = t;
            }
        }
        const bool take_video =
            v < video.size() && (track == kNone || video[v].pts <= audio_tracks[track][next[track]].pts);
        if (take_video) {
            const Packet& p = video[v];
            const Time100ns duration = span_until_next(video, v);
            plan.samples.push_back(ClipSample{false, 0, v, p.pts - base, duration, p.keyframe});
            plan.video_duration = p.pts - base + duration;
            ++plan.video_samples;
            ++v;
        } else if (track != kNone) {
            const std::vector<Packet>& audio = audio_tracks[track];
            const std::size_t a = next[track];
            plan.samples.push_back(ClipSample{true, track, a, audio[a].pts - base, span_until_next(audio, a), true});
            ++plan.audio_samples;
            ++plan.track_samples[track];
            ++next[track];
        } else {
            break;
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
