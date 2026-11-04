#include "core/ClipPlan.h"

#include "Check.h"

using namespace bt;

namespace {

constexpr Time100ns kFrame = 166'667;  // about 1/60 s
constexpr Time100ns kAac = 213'333;  // 1024 samples at 48 kHz

Packet video(Time100ns pts, bool keyframe) {
    return Packet{pts, kFrame, keyframe, make_packet_bytes({1, 2, 3})};
}

Packet audio(Time100ns pts) {
    return Packet{pts, kAac, true, make_packet_bytes({9})};
}

void refuses_clips_that_cannot_decode() {
    CHECK(plan_clip({}, {audio(0)}).samples.empty());
    CHECK(plan_clip({video(100, false), video(200, true)}, {}).samples.empty());
}

void rebases_to_the_first_keyframe() {
    const Time100ns base = 50 * kTicksPerSecond;  // QPC time an hour into a session
    const std::vector<Packet> v = {video(base, true), video(base + kFrame, false), video(base + 2 * kFrame, false)};
    const ClipPlan plan = plan_clip(v, {});
    CHECK_EQ(plan.samples.size(), std::size_t{3});
    CHECK_EQ(plan.samples[0].time, Time100ns{0});
    CHECK(plan.samples[0].keyframe);
    CHECK_EQ(plan.samples[2].time, 2 * kFrame);
    CHECK(!plan.samples[2].audio);
    CHECK_EQ(plan.video_duration, 3 * kFrame);
    CHECK_EQ(plan.duration, 3 * kFrame);
    CHECK_EQ(plan.audio_samples, std::size_t{0});
}

void interleaves_and_trims_audio() {
    const Time100ns base = 1'000'000;
    std::vector<Packet> v;
    for (int i = 0; i < 6; ++i) v.push_back(video(base + i * kFrame, i == 0));
    std::vector<Packet> a;
    // Audio from 2 packets before the video until well after it.
    for (int i = -2; i < 8; ++i) a.push_back(audio(base + i * kAac));

    const ClipPlan plan = plan_clip(v, a);
    CHECK_EQ(plan.audio_dropped, std::size_t{2});  // started before the first video frame
    CHECK_EQ(plan.audio_samples, std::size_t{8});  // includes audio after the last video frame
    CHECK_EQ(plan.video_samples, std::size_t{6});
    CHECK_EQ(plan.samples.size(), std::size_t{14});

    // Video first on a tie: both streams start at 0.
    CHECK(!plan.samples[0].audio);
    CHECK(plan.samples[1].audio);
    CHECK_EQ(plan.samples[1].time, Time100ns{0});
    CHECK_EQ(plan.samples[1].index, std::size_t{2});

    // Write order is non-decreasing in time.
    for (std::size_t i = 1; i < plan.samples.size(); ++i) {
        CHECK(plan.samples[i].time >= plan.samples[i - 1].time);
    }
    CHECK_EQ(plan.video_duration, 6 * kFrame);
    CHECK_EQ(plan.duration, 8 * kAac);  // the audio runs past the video
}

void dropped_frames_keep_their_time() {
    // Frame 2 was dropped by the encoder: frame 1 lasts until frame 3 starts.
    const std::vector<Packet> v = {video(0, true), video(kFrame, false), video(3 * kFrame, false)};
    const ClipPlan plan = plan_clip(v, {});
    CHECK_EQ(plan.samples[0].duration, kFrame);
    CHECK_EQ(plan.samples[1].duration, 2 * kFrame);
    CHECK_EQ(plan.samples[2].duration, kFrame);  // the last keeps its own
    CHECK_EQ(plan.video_duration, 4 * kFrame);
    // Durations add up to the timeline: the sum equals the end of the last frame.
    Time100ns sum = 0;
    for (const ClipSample& s : plan.samples) sum += s.duration;
    CHECK_EQ(sum, plan.video_duration);
}

void durations_are_never_zero() {
    // Two packets with the same stamp (never produced on purpose, but the writer must not fail).
    const ClipPlan plan = plan_clip({video(0, true), video(0, false), video(kFrame, false)}, {});
    for (const ClipSample& s : plan.samples) {
        CHECK(s.duration > 0);
    }
}

void audio_exactly_at_the_keyframe_is_kept() {
    const ClipPlan plan = plan_clip({video(500, true)}, {audio(499), audio(500)});
    CHECK_EQ(plan.audio_dropped, std::size_t{1});
    CHECK_EQ(plan.audio_samples, std::size_t{1});
}

void file_names() {
    std::tm t{};
    t.tm_year = 2026 - 1900;
    t.tm_mon = 10;  // November
    t.tm_mday = 4;
    t.tm_hour = 9;
    t.tm_min = 5;
    t.tm_sec = 7;
    CHECK_EQ(expand_clip_name("clip_%Y-%m-%d_%H-%M-%S.mp4", t), std::string("clip_2026-11-04_09-05-07.mp4"));
    CHECK_EQ(expand_clip_name("100%%.mp4", t), std::string("100%.mp4"));
    CHECK_EQ(expand_clip_name("", t), std::string());
    CHECK(expand_clip_name(std::string(400, 'a'), t).empty());  // too long

    CHECK_EQ(numbered_clip_name("clip.mp4", 2), std::string("clip_2.mp4"));
    CHECK_EQ(numbered_clip_name("my.game.clip.mp4", 3), std::string("my.game.clip_3.mp4"));
    CHECK_EQ(numbered_clip_name("noext", 2), std::string("noext_2"));
}

}  // namespace

int main() {
    refuses_clips_that_cannot_decode();
    rebases_to_the_first_keyframe();
    interleaves_and_trims_audio();
    audio_exactly_at_the_keyframe_is_kept();
    dropped_frames_keep_their_time();
    durations_are_never_zero();
    file_names();
    return bt_test::finish("ClipPlanTests");
}
