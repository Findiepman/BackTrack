#include "core/ClipBuffer.h"

#include "Check.h"

using namespace bt;

namespace {

constexpr Time100ns kSecond = kTicksPerSecond;
constexpr int kFps = 60;

Packet make(std::int64_t frame, bool keyframe, std::size_t bytes = 100) {
    return Packet{frame_offset(frame, kFps), frame_offset(frame + 1, kFps) - frame_offset(frame, kFps), keyframe,
                  std::vector<std::uint8_t>(bytes, keyframe ? std::uint8_t{0xAA} : std::uint8_t{0x55})};
}

// A 60 fps stream with a keyframe every second, like the encoder is configured (§3.1).
void push_seconds(ClipBuffer& buffer, std::int64_t from_frame, std::int64_t frames) {
    for (std::int64_t f = from_frame; f < from_frame + frames; ++f) {
        buffer.push(make(f, f % kFps == 0));
    }
}

void empty_buffer() {
    ClipBuffer buffer(30 * kSecond, 1'000'000);
    CHECK(buffer.extract(0, 100 * kSecond).empty());
    const ClipBufferStats s = buffer.stats();
    CHECK_EQ(s.packets, std::size_t{0});
    CHECK_EQ(s.bytes, std::size_t{0});
}

void waits_for_first_keyframe() {
    ClipBuffer buffer(30 * kSecond, 1'000'000);
    CHECK(!buffer.push(make(0, false)));
    CHECK(!buffer.push(make(1, false)));
    CHECK(buffer.push(make(2, true)));
    CHECK(buffer.push(make(3, false)));
    CHECK_EQ(buffer.stats().packets, std::size_t{2});
    CHECK_EQ(buffer.stats().dropped_waiting_for_keyframe, std::uint64_t{2});
    // Hotkey within the first second: only part of a GOP exists, but it starts on a keyframe.
    const auto clip = buffer.extract(-30 * kSecond, frame_offset(3, kFps));
    CHECK_EQ(clip.size(), std::size_t{2});
    CHECK(clip.front().keyframe);
}

void eviction_keeps_the_keyframe() {
    ClipBuffer buffer(5 * kSecond, 100'000'000);
    push_seconds(buffer, 0, 20 * kFps + 30);  // 20.5 s
    const ClipBufferStats s = buffer.stats();
    // Newest is frame 1229 (20.48 s). Threshold 15.48 s: last keyframe at or before it is 15 s.
    CHECK_EQ(s.oldest_pts, frame_offset(15 * kFps, kFps));
    CHECK_EQ(s.packets, std::size_t{20 * kFps + 30 - 15 * kFps});
    // So the buffer holds keep + part of one GOP, and starts on a keyframe.
    const auto all = buffer.extract(0, 100 * kSecond);
    CHECK(all.front().keyframe);
    CHECK_EQ(s.bytes, all.size() * std::size_t{100});

    // Exactly on the boundary: newest at 21 s - 1 frame + ... push up to frame 1260 (21 s).
    push_seconds(buffer, 20 * kFps + 30, 31);  // frames 1230..1260
    CHECK_EQ(buffer.stats().oldest_pts, frame_offset(16 * kFps, kFps));  // 21 - 5 = 16, a keyframe
}

void extraction_starts_on_a_keyframe() {
    ClipBuffer buffer(30 * kSecond, 100'000'000);
    push_seconds(buffer, 0, 40 * kFps);  // 0..40 s, keeps 9..40 s
    const Time100ns now = frame_offset(40 * kFps - 1, kFps);

    // Ask for 10 s ending now: starts at the last keyframe at or before now - 10 s.
    const auto clip = buffer.extract(now - 10 * kSecond, now);
    CHECK(clip.front().keyframe);
    CHECK_EQ(clip.front().pts, frame_offset(29 * kFps, kFps));  // now - 10 s = 29.98 s -> keyframe at 29 s
    CHECK_EQ(clip.back().pts, now);
    const Time100ns length = clip.back().pts + clip.back().duration - clip.front().pts;
    CHECK(length >= 10 * kSecond && length <= 11 * kSecond);  // N to N+1 seconds

    // A start exactly on a keyframe starts there, not one GOP earlier.
    const auto exact = buffer.extract(frame_offset(30 * kFps, kFps), now);
    CHECK_EQ(exact.front().pts, frame_offset(30 * kFps, kFps));

    // `end` cuts off later packets.
    const auto middle = buffer.extract(frame_offset(20 * kFps, kFps), frame_offset(25 * kFps, kFps));
    CHECK_EQ(middle.front().pts, frame_offset(20 * kFps, kFps));
    CHECK_EQ(middle.back().pts, frame_offset(25 * kFps, kFps));
    CHECK_EQ(middle.size(), std::size_t{5 * kFps + 1});

    // Asking further back than the buffer holds starts at the oldest keyframe.
    const auto everything = buffer.extract(0, now);
    CHECK_EQ(everything.front().pts, buffer.stats().oldest_pts);
    CHECK(everything.front().keyframe);

    // End before anything in the buffer: nothing.
    CHECK(buffer.extract(0, frame_offset(5 * kFps, kFps)).empty());

    // Extraction copies: the buffer is unchanged and the copy is independent.
    auto copy = buffer.extract(now - 10 * kSecond, now);
    copy.front().data[0] = 0;
    CHECK_EQ(buffer.extract(now - 10 * kSecond, now).front().data[0], std::uint8_t{0xAA});
}

void hard_byte_cap() {
    // Cap of 50 packets of 100 bytes; keyframes every second (60 packets) cannot fit one GOP.
    ClipBuffer buffer(30 * kSecond, 5000);
    push_seconds(buffer, 0, 10 * kFps);
    const ClipBufferStats s = buffer.stats();
    CHECK(s.bytes <= 5000);
    CHECK(s.dropped_by_byte_cap > 0);
    // Whatever is left starts on a keyframe, or the buffer is empty and waiting for one.
    const auto clip = buffer.extract(0, 100 * kSecond);
    CHECK(clip.empty() || clip.front().keyframe);

    // A broken encoder that never sends another keyframe: memory stays bounded forever.
    ClipBuffer broken(30 * kSecond, 5000);
    broken.push(make(0, true));
    for (std::int64_t f = 1; f < 100'000; ++f) {
        broken.push(make(f, false));
    }
    CHECK(broken.stats().bytes <= 5000);
    CHECK(broken.stats().dropped_by_byte_cap > 0);
    // The cap emptied it, so it waits for a keyframe instead of storing undecodable frames.
    CHECK_EQ(broken.stats().packets, std::size_t{0});
    CHECK(!broken.push(make(100'000, false)));
    CHECK(broken.push(make(100'001, true)));
}

void single_huge_gop() {
    // Keyframe only every 100 s: the buffer must keep the whole GOP to stay decodable.
    ClipBuffer buffer(10 * kSecond, 100'000'000);
    for (std::int64_t f = 0; f < 150 * kFps; ++f) {
        buffer.push(make(f, f % (100 * kFps) == 0));
    }
    const ClipBufferStats s = buffer.stats();
    CHECK_EQ(s.oldest_pts, frame_offset(100 * kFps, kFps));
    CHECK_EQ(s.packets, std::size_t{50 * kFps});
    const Time100ns now = frame_offset(150 * kFps - 1, kFps);
    const auto clip = buffer.extract(now - 10 * kSecond, now);
    CHECK_EQ(clip.front().pts, frame_offset(100 * kFps, kFps));  // longer than asked, but decodable
    CHECK(clip.front().keyframe);
}

void audio_every_packet_a_keyframe() {
    // AAC at 48 kHz: 1024 samples per packet = 213333.33 ticks; every packet is a sync point.
    ClipBuffer buffer(5 * kSecond, 100'000'000);
    const Time100ns packet = kTicksPerSecond * 1024 / 48000;
    for (int i = 0; i < 2000; ++i) {
        CHECK(buffer.push(Packet{i * packet, packet, true, std::vector<std::uint8_t>(10, 1)}));
    }
    const ClipBufferStats s = buffer.stats();
    // Exactly `keep` of audio, plus less than one packet: the oldest is the last one at or before
    // newest - keep.
    CHECK(s.newest_pts - s.oldest_pts >= 5 * kSecond);
    CHECK(s.newest_pts - s.oldest_pts < 5 * kSecond + packet);
    const Time100ns video_start = s.newest_pts - 2 * kSecond;
    const auto clip = buffer.extract(video_start, s.newest_pts);
    CHECK(clip.front().pts <= video_start);
    CHECK(clip.front().pts > video_start - packet);
}

void timestamps_not_monotonic() {
    ClipBuffer buffer(30 * kSecond, 1'000'000);
    CHECK(buffer.push(make(0, true)));
    CHECK(buffer.push(make(1, false)));
    CHECK(buffer.push(make(2, false)));
    CHECK(!buffer.push(make(2, false)));  // same pts
    CHECK(!buffer.push(make(1, false)));  // backwards
    // After the glitch, P-frames are refused until the next keyframe.
    CHECK(!buffer.push(make(3, false)));
    CHECK(buffer.push(make(4, true)));
    CHECK(buffer.push(make(5, false)));
    const ClipBufferStats s = buffer.stats();
    CHECK_EQ(s.dropped_out_of_order, std::uint64_t{2});
    CHECK_EQ(s.dropped_waiting_for_keyframe, std::uint64_t{1});
    CHECK_EQ(s.packets, std::size_t{5});
    const auto clip = buffer.extract(0, 100 * kSecond);
    for (std::size_t i = 1; i < clip.size(); ++i) {
        CHECK(clip[i].pts > clip[i - 1].pts);
    }

    // Audio: a backwards packet is dropped, the next one is accepted at once.
    ClipBuffer audio(30 * kSecond, 1'000'000);
    CHECK(audio.push(Packet{100, 10, true, {1}}));
    CHECK(!audio.push(Packet{50, 10, true, {1}}));
    CHECK(audio.push(Packet{110, 10, true, {1}}));
}

void clear_waits_for_keyframe() {
    ClipBuffer buffer(30 * kSecond, 1'000'000);
    push_seconds(buffer, 0, 120);
    buffer.clear();
    CHECK_EQ(buffer.stats().packets, std::size_t{0});
    CHECK_EQ(buffer.stats().bytes, std::size_t{0});
    // Timestamps may restart after a clear (new pipeline).
    CHECK(!buffer.push(make(1, false)));
    CHECK(buffer.push(make(0, true)));
}

// The shortest and longest clip lengths the config allows (§5.2): each keeps exactly its length
// plus at most one GOP, and a clip of that length comes out as N to N+1 seconds.
void config_extremes() {
    for (const int clip_seconds : {5, 300}) {
        ClipBuffer buffer(clip_seconds * kSecond, 100'000'000);
        const std::int64_t frames = static_cast<std::int64_t>(clip_seconds + 20) * kFps;
        push_seconds(buffer, 0, frames);
        const ClipBufferStats s = buffer.stats();
        const Time100ns held = s.newest_pts - s.oldest_pts;
        CHECK(held >= clip_seconds * kSecond);
        CHECK(held < (clip_seconds + 1) * kSecond);
        const Time100ns now = frame_offset(frames - 1, kFps);
        const auto clip = buffer.extract(now - clip_seconds * kSecond, now);
        const Time100ns length = clip.back().pts + clip.back().duration - clip.front().pts;
        CHECK(clip.front().keyframe);
        CHECK(length >= clip_seconds * kSecond && length <= (clip_seconds + 1) * kSecond);
    }
}

}  // namespace

int main() {
    config_extremes();
    empty_buffer();
    waits_for_first_keyframe();
    eviction_keeps_the_keyframe();
    extraction_starts_on_a_keyframe();
    hard_byte_cap();
    single_huge_gop();
    audio_every_packet_a_keyframe();
    timestamps_not_monotonic();
    clear_waits_for_keyframe();
    return bt_test::finish("ClipBufferTests");
}
