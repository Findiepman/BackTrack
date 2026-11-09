#pragma once

#include "core/Clock.h"

#include <cstdint>
#include <optional>

namespace bt {

// Notices a video encoder that takes frames but never gives packets back. AMD's hardware encoder
// can stop answering under GPU load without reporting an error (measured: an hour of clips with
// one stale second of video). Fed once per supervisor tick with two running counters:
//   offered:  frames the capture tried to hand to the encoder (also those dropped because the
//             encoder still held every input texture)
//   produced: packets the encoder put out
// A stall is `timeout` of frames being offered while nothing is produced. Time without offered
// frames (paused, lock screen) never counts, so a pause cannot be mistaken for a stall.
class StallWatch {
public:
    explicit StallWatch(Time100ns timeout) : timeout_(timeout) {}

    // Forget everything: a new encoder or capture starts its counters again.
    void reset() { have_baseline_ = false; }

    // True once the encoder has been stalled for at least `timeout`.
    bool stalled(Time100ns now, std::uint64_t offered, std::uint64_t produced) {
        if (!have_baseline_ || produced != produced_ || offered < offered_) {
            // First look, progress, or counters that started over (a new capture or encoder).
            have_baseline_ = true;
            produced_ = produced;
            offered_ = offered;
            waiting_since_.reset();
            return false;
        }
        if (offered == offered_) {
            // Nothing new was offered (paused, capture lost without a frame): no packet is owed,
            // and the stall clock starts again once frames come back.
            waiting_since_.reset();
            return false;
        }
        if (!waiting_since_) {
            waiting_since_ = now;
        }
        offered_ = offered;
        return now - *waiting_since_ >= timeout_;
    }

    // How long frames have gone in without a packet coming out (0 when not waiting).
    Time100ns waiting_for(Time100ns now) const { return waiting_since_ ? now - *waiting_since_ : 0; }

private:
    const Time100ns timeout_;
    bool have_baseline_ = false;
    std::uint64_t produced_ = 0;
    std::uint64_t offered_ = 0;
    std::optional<Time100ns> waiting_since_;  // when frames were first offered without a packet since
};

}  // namespace bt
