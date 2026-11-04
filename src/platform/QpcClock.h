#pragma once

#include "core/Clock.h"

#include <windows.h>

namespace bt {

// §3.3: the one clock every video and audio timestamp comes from.
inline Time100ns qpc_now() {
    // The counter frequency is fixed at boot, so it is read once.
    static const std::int64_t frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return counter_to_100ns(counter.QuadPart, frequency);
}

}  // namespace bt
