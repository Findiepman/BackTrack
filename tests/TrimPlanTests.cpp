#include "core/TrimPlan.h"

#include "Check.h"

#include <vector>

using namespace bt;

namespace {

constexpr Time100ns kFrame = kTicksPerSecond / 10;  // 10 fps keeps the numbers readable
constexpr Time100ns kSecond = kTicksPerSecond;

// `seconds` of video at 10 fps with a keyframe every second, like a Backtrack clip.
std::vector<TrimFrame> clip(int seconds, Time100ns first_pts = 0) {
    std::vector<TrimFrame> frames;
    for (int i = 0; i < seconds * 10; ++i) {
        frames.push_back(TrimFrame{first_pts + i * kFrame, i % 10 == 0});
    }
    return frames;
}

void starts_on_the_keyframe_before_in() {
    const auto v = clip(10);
    const TrimPlan plan = plan_trim(v, 2 * kSecond + 4 * kFrame, 7 * kSecond);
    CHECK(plan.problem == TrimProblem::None);
    CHECK_EQ(plan.first, std::size_t{20});
    CHECK_EQ(plan.start, 2 * kSecond);
    CHECK_EQ(plan.end, std::size_t{70});  // the frame at exactly 7 s is not included
}

void in_exactly_on_a_keyframe() {
    const auto v = clip(10);
    const TrimPlan plan = plan_trim(v, 3 * kSecond, 4 * kSecond);
    CHECK(plan.problem == TrimProblem::None);
    CHECK_EQ(plan.first, std::size_t{30});
    CHECK_EQ(plan.start, 3 * kSecond);
    CHECK_EQ(plan.end, std::size_t{40});
    // One tick before the keyframe goes back a whole second.
    CHECK_EQ(plan_trim(v, 3 * kSecond - 1, 4 * kSecond).first, std::size_t{20});
}

void in_before_the_first_keyframe() {
    // A file whose first frames are not keyframes (cut out of a longer recording by another tool).
    std::vector<TrimFrame> v = clip(3);
    v[0].keyframe = false;  // keyframes now at 1 s and 2 s
    const TrimPlan plan = plan_trim(v, 2 * kFrame, 2 * kSecond + 5 * kFrame);
    CHECK(plan.problem == TrimProblem::None);
    CHECK_EQ(plan.first, std::size_t{10});
    CHECK_EQ(plan.start, kSecond);
    CHECK_EQ(plan.end, std::size_t{25});
    // Negative in: from the start.
    const TrimPlan from_start = plan_trim(clip(3), -kSecond, kSecond);
    CHECK(from_start.problem == TrimProblem::None);
    CHECK_EQ(from_start.first, std::size_t{0});
    CHECK_EQ(from_start.end, std::size_t{10});
    // `out` before the first keyframe leaves nothing.
    CHECK(plan_trim(v, 0, 5 * kFrame).problem == TrimProblem::EmptyRange);
}

void out_past_the_end_keeps_the_rest() {
    const auto v = clip(5);
    const TrimPlan plan = plan_trim(v, 0, 100 * kSecond);
    CHECK(plan.problem == TrimProblem::None);
    CHECK_EQ(plan.first, std::size_t{0});
    CHECK_EQ(plan.end, v.size());
}

void empty_or_backwards_ranges() {
    const auto v = clip(5);
    CHECK(plan_trim(v, 2 * kSecond, 2 * kSecond).problem == TrimProblem::EmptyRange);
    CHECK(plan_trim(v, 3 * kSecond, 2 * kSecond).problem == TrimProblem::EmptyRange);
}

void in_past_the_end() {
    const auto v = clip(5);
    CHECK(plan_trim(v, 6 * kSecond, 7 * kSecond).problem == TrimProblem::StartPastEnd);
    // On the last frame itself: a one-frame clip from the last keyframe.
    const TrimPlan last = plan_trim(v, v.back().pts, v.back().pts + kFrame);
    CHECK(last.problem == TrimProblem::None);
    CHECK_EQ(last.first, std::size_t{40});
    CHECK_EQ(last.end, v.size());
}

void tiny_range_inside_one_second() {
    const auto v = clip(5);
    const TrimPlan plan = plan_trim(v, 3 * kSecond + 7 * kFrame, 3 * kSecond + 8 * kFrame);
    CHECK(plan.problem == TrimProblem::None);
    CHECK_EQ(plan.first, std::size_t{30});
    CHECK_EQ(plan.end, std::size_t{38});
}

void refuses_what_cannot_be_cut() {
    CHECK(plan_trim({}, 0, kSecond).problem == TrimProblem::NoVideo);
    std::vector<TrimFrame> no_keys = clip(2);
    for (TrimFrame& f : no_keys) f.keyframe = false;
    CHECK(plan_trim(no_keys, 0, kSecond).problem == TrimProblem::NoKeyframe);
    // B-frames: decode order is not presentation order.
    const std::vector<TrimFrame> reordered = {{0, true}, {3 * kFrame, false}, {kFrame, false}, {2 * kFrame, false}};
    CHECK(plan_trim(reordered, 0, kSecond).problem == TrimProblem::Reordered);
    const std::vector<TrimFrame> repeated = {{0, true}, {0, false}};
    CHECK(plan_trim(repeated, 0, kSecond).problem == TrimProblem::Reordered);
}

void works_on_any_timeline() {
    // Timestamps that do not start at 0 (an MP4 with an offset).
    const Time100ns base = 1000 * kSecond;
    const auto v = clip(4, base);
    const TrimPlan plan = plan_trim(v, base + kSecond + kFrame, base + 2 * kSecond);
    CHECK(plan.problem == TrimProblem::None);
    CHECK_EQ(plan.start, base + kSecond);
    CHECK_EQ(plan.end, std::size_t{20});
}

void audio_range() {
    constexpr Time100ns kAac = 213'333;
    std::vector<Time100ns> audio;
    for (int i = 0; i < 50; ++i) audio.push_back(179'166 + i * kAac);  // starts 17.9 ms after the video
    const TrimRange all = trim_audio(audio, 0, 100 * kSecond);
    CHECK_EQ(all.first, std::size_t{0});
    CHECK_EQ(all.end, audio.size());
    const TrimRange part = trim_audio(audio, 179'166 + 5 * kAac, 179'166 + 9 * kAac);
    CHECK_EQ(part.first, std::size_t{5});  // exactly at start: kept
    CHECK_EQ(part.end, std::size_t{9});  // exactly at out: left out
    const TrimRange straddling = trim_audio(audio, 179'166 + 5 * kAac - 1, kSecond);
    CHECK_EQ(straddling.first, std::size_t{5});
    const TrimRange none = trim_audio(audio, 100 * kSecond, 101 * kSecond);
    CHECK_EQ(none.first, none.end);
    const TrimRange empty = trim_audio({}, 0, kSecond);
    CHECK_EQ(empty.first, std::size_t{0});
    CHECK_EQ(empty.end, std::size_t{0});
}

void restart_rule_matches_the_plan() {
    // Reading front to back and letting go of everything before each restart point must never
    // let go of the frame plan_trim picks.
    std::vector<TrimFrame> v = clip(6);
    v[0].keyframe = false;
    for (const Time100ns in : {Time100ns{-1}, Time100ns{0}, kFrame, kSecond, kSecond + 1, 3 * kSecond + 5 * kFrame,
                               v.back().pts}) {
        std::size_t kept_from = 0;
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (trim_restarts_at(v[i], in)) kept_from = i;
        }
        const TrimPlan plan = plan_trim(v, in, in + 2 * kSecond);
        CHECK(plan.problem == TrimProblem::None);
        CHECK(plan.first >= kept_from);
    }
}

}  // namespace

int main() {
    starts_on_the_keyframe_before_in();
    in_exactly_on_a_keyframe();
    in_before_the_first_keyframe();
    out_past_the_end_keeps_the_rest();
    empty_or_backwards_ranges();
    in_past_the_end();
    tiny_range_inside_one_second();
    refuses_what_cannot_be_cut();
    works_on_any_timeline();
    audio_range();
    restart_rule_matches_the_plan();
    return bt_test::finish("TrimPlanTests");
}
