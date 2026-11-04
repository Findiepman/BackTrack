#include "core/ClipBuffer.h"

#include <utility>

namespace bt {

ClipBuffer::ClipBuffer(Time100ns keep, std::size_t max_bytes) : keep_(keep), max_bytes_(max_bytes) {}

bool ClipBuffer::push(Packet packet) {
    std::lock_guard lock(mutex_);
    if (have_last_pts_ && packet.pts <= last_pts_) {
        // Going back in time would break the MP4 timeline. Without B-frames (§3.1) this only
        // happens on an encoder glitch. Whatever follows may reference the dropped frame, so
        // nothing is accepted again until the next keyframe.
        ++dropped_out_of_order_;
        waiting_for_keyframe_ = true;
        return false;
    }
    if (waiting_for_keyframe_) {
        if (!packet.keyframe) {
            ++dropped_waiting_;
            return false;
        }
        waiting_for_keyframe_ = false;
    }
    have_last_pts_ = true;
    last_pts_ = packet.pts;
    bytes_ += packet.size();
    packets_.push_back(std::move(packet));
    evict_locked();
    return true;
}

void ClipBuffer::evict_locked() {
    // Time rule: drop everything before the last keyframe at or before newest - keep. The scan
    // stops at the threshold, so it covers about one GOP, not the whole buffer.
    const Time100ns threshold = packets_.back().pts - keep_;
    std::size_t cut = 0;
    for (std::size_t i = 0; i < packets_.size() && packets_[i].pts <= threshold; ++i) {
        if (packets_[i].keyframe) {
            cut = i;
        }
    }
    for (std::size_t i = 0; i < cut; ++i) {
        bytes_ -= packets_.front().size();
        packets_.pop_front();
    }

    // Byte rule: only reached when keyframes are missing or far apart. Drop from the front, then
    // on to the next keyframe so the buffer still starts on one.
    if (bytes_ > max_bytes_) {
        while (!packets_.empty() && (bytes_ > max_bytes_ || !packets_.front().keyframe)) {
            bytes_ -= packets_.front().size();
            packets_.pop_front();
            ++dropped_by_cap_;
        }
        if (packets_.empty()) {
            // No keyframe left: the next packets cannot be decoded until one arrives.
            waiting_for_keyframe_ = true;
        }
    }
}

std::vector<Packet> ClipBuffer::extract(Time100ns start, Time100ns end) const {
    std::lock_guard lock(mutex_);
    constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    std::size_t first = kNone;
    for (std::size_t i = 0; i < packets_.size() && packets_[i].pts <= start; ++i) {
        if (packets_[i].keyframe) {
            first = i;
        }
    }
    if (first == kNone) {
        // The buffer does not reach back to `start` (just started, or a long GOP was evicted):
        // the clip starts at the oldest keyframe instead.
        for (std::size_t i = 0; i < packets_.size(); ++i) {
            if (packets_[i].keyframe) {
                first = i;
                break;
            }
        }
    }
    std::vector<Packet> clip;
    if (first == kNone || packets_[first].pts > end) {
        return clip;
    }
    std::size_t last = first;
    while (last + 1 < packets_.size() && packets_[last + 1].pts <= end) {
        ++last;
    }
    clip.reserve(last - first + 1);
    for (std::size_t i = first; i <= last; ++i) {
        clip.push_back(packets_[i]);
    }
    return clip;
}

ClipBufferStats ClipBuffer::stats() const {
    std::lock_guard lock(mutex_);
    ClipBufferStats s;
    s.packets = packets_.size();
    s.bytes = bytes_;
    s.oldest_pts = packets_.empty() ? 0 : packets_.front().pts;
    s.newest_pts = packets_.empty() ? 0 : packets_.back().pts;
    s.dropped_out_of_order = dropped_out_of_order_;
    s.dropped_waiting_for_keyframe = dropped_waiting_;
    s.dropped_by_byte_cap = dropped_by_cap_;
    return s;
}

void ClipBuffer::clear() {
    std::lock_guard lock(mutex_);
    packets_.clear();
    bytes_ = 0;
    have_last_pts_ = false;
    last_pts_ = 0;
    waiting_for_keyframe_ = true;
}

}  // namespace bt
