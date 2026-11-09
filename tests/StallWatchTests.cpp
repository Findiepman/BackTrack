#include "core/StallWatch.h"

#include "Check.h"

using bt::StallWatch;
using bt::Time100ns;

namespace {

constexpr Time100ns kSecond = bt::kTicksPerSecond;
constexpr Time100ns kTick = kSecond / 2;  // the recorder's supervisor tick
constexpr Time100ns kTimeout = 5 * kSecond;
constexpr std::uint64_t kFramesPerTick = 30;

// Frames go in, packets come out: never a stall, however long it runs.
void healthy_encoder() {
    StallWatch watch(kTimeout);
    std::uint64_t offered = 0;
    std::uint64_t produced = 0;
    for (int i = 0; i < 1000; ++i) {
        offered += kFramesPerTick;
        produced += kFramesPerTick - 2;  // a few frames inside the encoder is normal
        CHECK(!watch.stalled(i * kTick, offered, produced));
    }
}

// The user's case: frames keep being offered, no packet ever comes out.
void stalled_encoder() {
    StallWatch watch(kTimeout);
    std::uint64_t offered = 100;
    const std::uint64_t produced = 90;
    CHECK(!watch.stalled(0, offered, produced));
    Time100ns now = 0;
    bool reported = false;
    Time100ns reported_at = 0;
    for (int i = 1; i <= 30 && !reported; ++i) {
        now = i * kTick;
        offered += kFramesPerTick;
        reported = watch.stalled(now, offered, produced);
        reported_at = now;
    }
    CHECK(reported);
    // Reported once the timeout has passed since the first unanswered tick, not before.
    CHECK(reported_at >= kTick + kTimeout);
    CHECK(reported_at <= kTick + kTimeout + kTick);
    CHECK(watch.waiting_for(now) >= kTimeout);
}

// A hung encoder that holds every input texture: the capture's frames all count as dropped,
// which the recorder adds to `offered`, so this is the same as above.
void stall_seen_through_dropped_frames() {
    StallWatch watch(kTimeout);
    std::uint64_t delivered = 500;
    std::uint64_t dropped = 0;
    CHECK(!watch.stalled(0, delivered + dropped, 400));
    bool reported = false;
    for (int i = 1; i <= 20; ++i) {
        dropped += kFramesPerTick;
        reported = watch.stalled(i * kTick, delivered + dropped, 400) || reported;
    }
    CHECK(reported);
}

// Paused or the screen gone: nothing is offered, nothing is owed, however long.
void nothing_offered_is_not_a_stall() {
    StallWatch watch(kTimeout);
    for (int i = 0; i < 200; ++i) {
        CHECK(!watch.stalled(i * kTick, 1000, 900));
    }
}

// A pause in the middle of a slow moment must not count as stall time once frames come back.
void pause_restarts_the_clock() {
    StallWatch watch(kTimeout);
    CHECK(!watch.stalled(0, 100, 90));
    CHECK(!watch.stalled(kTick, 130, 90));  // one tick without output: waiting starts
    for (int i = 2; i < 100; ++i) {
        CHECK(!watch.stalled(i * kTick, 130, 90));  // paused for ~50 s
    }
    // Resumed: frames offered again, the encoder needs one tick to answer.
    CHECK(!watch.stalled(100 * kTick, 160, 90));
    CHECK(!watch.stalled(101 * kTick, 190, 150));
}

// A new capture or encoder starts its counters at zero: a fresh baseline, not a stall.
void counters_starting_over() {
    StallWatch watch(kTimeout);
    CHECK(!watch.stalled(0, 5000, 4900));
    CHECK(!watch.stalled(kTick, 5030, 4900));
    CHECK(!watch.stalled(2 * kTick, 10, 4900));  // capture rebuilt: offered went back
    CHECK(!watch.stalled(3 * kTick, 40, 0));  // encoder rebuilt: produced went back
    CHECK(!watch.stalled(4 * kTick, 70, 25));
}

// reset() forgets a stall in progress.
void reset_forgets() {
    StallWatch watch(kTimeout);
    std::uint64_t offered = 0;
    for (int i = 0; i < 9; ++i) {
        offered += kFramesPerTick;
        CHECK(!watch.stalled(i * kTick, offered, 0));
    }
    watch.reset();
    for (int i = 9; i < 18; ++i) {
        offered += kFramesPerTick;
        CHECK(!watch.stalled(i * kTick, offered, 0));
    }
    CHECK(watch.waiting_for(18 * kTick) < kTimeout);
}

}  // namespace

int main() {
    healthy_encoder();
    stalled_encoder();
    stall_seen_through_dropped_frames();
    nothing_offered_is_not_a_stall();
    pause_restarts_the_clock();
    counters_starting_over();
    reset_forgets();
    return bt_test::finish("StallWatchTests");
}
