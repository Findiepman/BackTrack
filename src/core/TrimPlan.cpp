#include "core/TrimPlan.h"

namespace bt {

TrimPlan plan_trim(std::span<const TrimFrame> video, Time100ns in, Time100ns out) {
    TrimPlan plan;
    if (video.empty()) {
        plan.problem = TrimProblem::NoVideo;
        return plan;
    }
    for (std::size_t i = 1; i < video.size(); ++i) {
        if (video[i].pts <= video[i - 1].pts) {
            plan.problem = TrimProblem::Reordered;
            return plan;
        }
    }
    if (out <= in) {
        plan.problem = TrimProblem::EmptyRange;
        return plan;
    }
    if (in > video.back().pts) {
        plan.problem = TrimProblem::StartPastEnd;
        return plan;
    }

    bool found = false;
    for (std::size_t i = 0; i < video.size(); ++i) {
        if (!video[i].keyframe) {
            continue;
        }
        if (video[i].pts <= in || !found) {
            plan.first = i;
            found = true;
        }
        if (video[i].pts >= in) {
            break;
        }
    }
    if (!found) {
        plan.problem = TrimProblem::NoKeyframe;
        return plan;
    }
    plan.start = video[plan.first].pts;

    plan.end = plan.first;
    while (plan.end < video.size() && video[plan.end].pts < out) {
        ++plan.end;
    }
    if (plan.end == plan.first) {
        // `in` lies before the first keyframe and `out` does not reach it.
        plan.problem = TrimProblem::EmptyRange;
    }
    return plan;
}

TrimRange trim_audio(std::span<const Time100ns> audio_pts, Time100ns start, Time100ns out) {
    TrimRange range;
    while (range.first < audio_pts.size() && audio_pts[range.first] < start) {
        ++range.first;
    }
    range.end = range.first;
    while (range.end < audio_pts.size() && audio_pts[range.end] < out) {
        ++range.end;
    }
    return range;
}

}  // namespace bt
