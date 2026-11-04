#pragma once

#include "core/Clock.h"

#include <deque>
#include <optional>
#include <utility>

namespace bt {

// The smallest value added during the last `window` of time. Used to find a device's real audio
// latency from noisy measurements: a packet can be read late (the poll wakes up late) but never
// early, so the minimum over a couple of seconds is the true latency without the jitter.
// Amortised O(1) per add (monotonic queue).
class WindowedMinimum {
public:
    explicit WindowedMinimum(Time100ns window) : window_(window) {}

    void add(Time100ns at, Time100ns value) {
        while (!entries_.empty() && entries_.back().second >= value) {
            entries_.pop_back();
        }
        entries_.emplace_back(at, value);
        while (entries_.front().first < at - window_) {
            entries_.pop_front();
        }
    }

    std::optional<Time100ns> value() const {
        if (entries_.empty()) {
            return std::nullopt;
        }
        return entries_.front().second;
    }

    void reset() { entries_.clear(); }

private:
    Time100ns window_;
    std::deque<std::pair<Time100ns, Time100ns>> entries_;  // (time added, value), values increasing
};

}  // namespace bt
