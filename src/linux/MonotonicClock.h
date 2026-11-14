#pragma once

#include "core/Clock.h"

#include <time.h>

namespace bt::linux_app {

// The Linux timeline for every timestamp: CLOCK_MONOTONIC in 100 ns units (the Windows version
// uses QueryPerformanceCounter the same way). Never jumps with the wall clock.
inline Time100ns now_100ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<Time100ns>(ts.tv_sec) * kTicksPerSecond + ts.tv_nsec / 100;
}

}  // namespace bt::linux_app
