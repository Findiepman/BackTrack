#include "core/Clock.h"
#include "core/WindowedMinimum.h"

#include "Check.h"

using namespace bt;

namespace {

void counter_conversion() {
    constexpr std::int64_t kTenMhz = 10'000'000;
    CHECK_EQ(counter_to_100ns(0, kTenMhz), 0);
    CHECK_EQ(counter_to_100ns(kTenMhz, kTenMhz), kTicksPerSecond);
    CHECK_EQ(counter_to_100ns(12345, kTenMhz), 12345);

    // A 3 GHz counter after a year of uptime: the naive counter * 10^7 would overflow int64.
    constexpr std::int64_t kThreeGhz = 3'000'000'000;
    constexpr std::int64_t kYearSeconds = 365LL * 24 * 3600;
    CHECK_EQ(counter_to_100ns(kYearSeconds * kThreeGhz, kThreeGhz), kYearSeconds * kTicksPerSecond);
    CHECK_EQ(counter_to_100ns(kThreeGhz + kThreeGhz / 2, kThreeGhz), kTicksPerSecond + kTicksPerSecond / 2);
}

void frame_offsets_do_not_drift() {
    // 60 fps: 10^7 / 60 is not whole, but every 60th frame lands exactly on a whole second.
    for (std::int64_t second = 0; second < 24 * 3600; second += 997) {
        CHECK_EQ(frame_offset(second * 60, 60), seconds_to_100ns(second));
    }
    for (const int fps : {30, 60, 120, 144}) {
        // 24 hours of frames, index math stays exact.
        const std::int64_t frames_per_day = 24LL * 3600 * fps;
        CHECK_EQ(frame_offset(frames_per_day, fps), seconds_to_100ns(24LL * 3600));
        // Durations differ by at most one 100 ns tick from the ideal.
        const Time100ns d = frame_offset(frames_per_day + 1, fps) - frame_offset(frames_per_day, fps);
        CHECK(d == kTicksPerSecond / fps || d == kTicksPerSecond / fps + 1);
    }
}

void windowed_minimum() {
    WindowedMinimum min(2 * kTicksPerSecond);
    CHECK(!min.value().has_value());
    min.add(0, 50);
    CHECK_EQ(*min.value(), Time100ns{50});
    min.add(kTicksPerSecond, 70);  // larger: the old minimum stays
    CHECK_EQ(*min.value(), Time100ns{50});
    min.add(kTicksPerSecond + 1, 30);  // smaller: takes over
    CHECK_EQ(*min.value(), Time100ns{30});
    min.add(2 * kTicksPerSecond, 90);
    CHECK_EQ(*min.value(), Time100ns{30});
    // 3.5 s: the 30 from 1 s is older than the window, the 90 from 2 s is the smallest left.
    min.add(3 * kTicksPerSecond + kTicksPerSecond / 2, 100);
    CHECK_EQ(*min.value(), Time100ns{90});
    // Negative values (a Bluetooth device stamps packets in the future) work the same way.
    min.add(4 * kTicksPerSecond, -1'700'000);
    CHECK_EQ(*min.value(), Time100ns{-1'700'000});
    min.reset();
    CHECK(!min.value().has_value());

    // Jittery reads around a true latency of 10 ms: the minimum finds it.
    WindowedMinimum jitter(2 * kTicksPerSecond);
    for (int i = 0; i < 200; ++i) {
        const Time100ns late = (i * 7919 % 160) * kTicksPerMillisecond / 10;  // 0..16 ms of poll delay
        jitter.add(i * 10 * kTicksPerMillisecond, 10 * kTicksPerMillisecond + late);
    }
    CHECK_EQ(*jitter.value(), 10 * kTicksPerMillisecond);
}

}  // namespace

int main() {
    counter_conversion();
    frame_offsets_do_not_drift();
    windowed_minimum();
    return bt_test::finish("ClockTests");
}
