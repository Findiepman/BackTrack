#include "core/Clock.h"

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

}  // namespace

int main() {
    counter_conversion();
    frame_offsets_do_not_drift();
    return bt_test::finish("ClockTests");
}
