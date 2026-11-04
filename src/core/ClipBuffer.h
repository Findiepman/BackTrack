#pragma once

#include "core/Clock.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace bt {

// One compressed packet: an H.264 access unit or an AAC frame. Plain bytes and timestamps, so the
// buffer knows nothing about Media Foundation (§4).
struct Packet {
    Time100ns pts = 0;
    Time100ns duration = 0;
    bool keyframe = false;
    std::vector<std::uint8_t> data;
};

struct ClipBufferStats {
    std::size_t packets = 0;
    std::size_t bytes = 0;
    Time100ns oldest_pts = 0;  // 0 when empty
    Time100ns newest_pts = 0;  // 0 when empty
    std::uint64_t dropped_out_of_order = 0;  // pts not after the previous packet
    std::uint64_t dropped_waiting_for_keyframe = 0;  // non-keyframes that could not be decoded
    std::uint64_t dropped_by_byte_cap = 0;  // evicted early because the buffer hit max_bytes
};

// §3.1: ring buffer of compressed packets that always starts on a keyframe.
//
// Keeps everything from the last keyframe at or before (newest - keep), so it holds `keep` plus
// up to one GOP. A packet in the middle of a GOP is never dropped on its own: the frames after it
// would not decode. max_bytes is the safety net for an encoder that never emits a keyframe.
//
// Thread safe. push() holds the lock for microseconds (a move and a short eviction scan),
// extract() for the time it takes to copy the clip (§3.8).
class ClipBuffer {
public:
    ClipBuffer(Time100ns keep, std::size_t max_bytes);

    // Returns false when the packet was dropped: its pts is not after the previous packet's, or
    // the buffer is waiting for a keyframe (at the start, or after a drop broke the GOP).
    bool push(Packet packet);

    // Copies the packets for a clip covering [start, end]: from the last keyframe at or before
    // `start` (or the oldest keyframe when the buffer does not reach back that far) up to the last
    // packet with pts <= end. Empty when the buffer holds no keyframe at or before `end`.
    std::vector<Packet> extract(Time100ns start, Time100ns end) const;

    ClipBufferStats stats() const;

    // Drops everything and waits for a keyframe again (used when the video size changes).
    void clear();

private:
    void evict_locked();

    mutable std::mutex mutex_;
    const Time100ns keep_;
    const std::size_t max_bytes_;
    std::deque<Packet> packets_;
    std::size_t bytes_ = 0;
    bool have_last_pts_ = false;
    Time100ns last_pts_ = 0;
    bool waiting_for_keyframe_ = true;
    std::uint64_t dropped_out_of_order_ = 0;
    std::uint64_t dropped_waiting_ = 0;
    std::uint64_t dropped_by_cap_ = 0;
};

}  // namespace bt
