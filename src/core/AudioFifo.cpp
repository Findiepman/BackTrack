#include "core/AudioFifo.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace bt {
AudioFifo::AudioFifo(std::size_t capacity_frames, AudioFifoTuning tuning) : tuning_(tuning) {
    if (capacity_frames == 0) {
        throw std::invalid_argument("AudioFifo capacity must not be 0");
    }
    buffer_.assign(std::bit_ceil(capacity_frames), 0.0f);
    mask_ = buffer_.size() - 1;
}

std::size_t AudioFifo::push(std::span<const float> frames) {
    const std::uint64_t write = write_.load(std::memory_order_relaxed);
    const std::uint64_t read = read_.load(std::memory_order_acquire);
    const std::size_t used = static_cast<std::size_t>(write - read);
    const std::size_t free = buffer_.size() - used;
    const std::size_t count = std::min(frames.size(), free);
    const std::size_t start = static_cast<std::size_t>(write) & mask_;
    const std::size_t first = std::min(count, buffer_.size() - start);
    if (first > 0) {
        std::memcpy(buffer_.data() + start, frames.data(), first * sizeof(float));
    }
    if (count > first) {
        std::memcpy(buffer_.data(), frames.data() + first, (count - first) * sizeof(float));
    }
    write_.store(write + count, std::memory_order_release);
    if (count < frames.size()) {
        overflow_frames_.fetch_add(frames.size() - count, std::memory_order_relaxed);
    }
    return count;
}

std::size_t AudioFifo::read_frames(std::size_t count, float* out) {
    const std::uint64_t read = read_.load(std::memory_order_relaxed);
    const std::size_t start = static_cast<std::size_t>(read) & mask_;
    const std::size_t first = std::min(count, buffer_.size() - start);
    if (first > 0) {
        std::memcpy(out, buffer_.data() + start, first * sizeof(float));
    }
    if (count > first) {
        std::memcpy(out + first, buffer_.data(), (count - first) * sizeof(float));
    }
    read_.store(read + count, std::memory_order_release);
    return count;
}

void AudioFifo::pull(std::span<float> out) {
    const std::size_t n = out.size();
    if (n == 0) {
        return;
    }
    pulled_frames_.fetch_add(n, std::memory_order_relaxed);
    std::uint64_t read = read_.load(std::memory_order_relaxed);
    std::size_t available = static_cast<std::size_t>(write_.load(std::memory_order_acquire) - read);

    if (priming_) {
        // Starts only once this block can be taken and target_frames are still left after it,
        // so playing never begins on the edge of running dry again.
        // (Never more than the FIFO can hold, or a huge block would wait forever.)
        if (available < std::min(n + tuning_.target_frames, buffer_.size())) {
            std::fill(out.begin(), out.end(), 0.0f);
            underflow_frames_.fetch_add(n, std::memory_order_relaxed);
            return;
        }
        priming_ = false;
        pending_correction_ = 0;
        window_started_ = false;
        correction_credit_ = 0;
        // A backlog that built up meanwhile (the burst after a stall) is not played late for a
        // whole window before the hard drop: it goes now.
        const std::size_t keep = n + tuning_.target_frames;
        if (available > keep + tuning_.hard_drop_frames) {
            const std::size_t drop = available - keep;
            read += drop;
            available = keep;
            read_.store(read, std::memory_order_release);
            dropped_frames_.fetch_add(drop, std::memory_order_relaxed);
        }
    }

    // The correction rate is one frame per frames_per_correction output frames, carried from
    // block to block, so it is the same for small and large blocks.
    std::int64_t step = 0;
    if (pending_correction_ != 0) {
        const auto per = static_cast<std::int64_t>(tuning_.frames_per_correction);
        correction_credit_ = std::min(correction_credit_ + static_cast<std::int64_t>(n), 4 * per);
        // At most one frame per 8 of the block, so a tiny block is never squeezed to nothing.
        const std::int64_t allowed = std::min(correction_credit_ / per, static_cast<std::int64_t>(n / 8));
        step = std::clamp(pending_correction_, -allowed, allowed);
        if (step > 0 && available < n + static_cast<std::size_t>(step)) {
            step = 0;  // the block is there, the extra frames are not: correct in a later block
        }
        correction_credit_ -= (step < 0 ? -step : step) * per;
    }
    const std::size_t take = static_cast<std::size_t>(static_cast<std::int64_t>(n) + step);

    if (available < take) {
        // Ran dry: what is there plays straight, the rest is silence, and the FIFO fills up to
        // target_frames again before it plays on (a click either way, but only one).
        const std::size_t got = read_frames(std::min(available, n), out.data());
        std::fill(out.begin() + static_cast<std::ptrdiff_t>(got), out.end(), 0.0f);
        underflow_frames_.fetch_add(n - got, std::memory_order_relaxed);
        underflows_.fetch_add(1, std::memory_order_relaxed);
        priming_ = true;
        pending_correction_ = 0;
        window_started_ = false;
        return;
    }

    if (step == 0) {
        read_frames(n, out.data());
    } else {
        // `take` input frames become `n` output frames, first and last exactly on input frames,
        // so consecutive blocks join without a step. Integer positions keep it exact.
        const std::size_t in_span = take - 1;
        const std::size_t out_span = n - 1;
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t numerator = i * in_span;
            const std::size_t k = numerator / out_span;
            const float fraction = static_cast<float>(numerator % out_span) / static_cast<float>(out_span);
            const float a = buffer_[static_cast<std::size_t>(read + k) & mask_];
            const float b = k + 1 < take ? buffer_[static_cast<std::size_t>(read + k + 1) & mask_] : a;
            out[i] = a + fraction * (b - a);
        }
        read_.store(read + take, std::memory_order_release);
        pending_correction_ -= step;
        corrected_frames_.fetch_add(step, std::memory_order_relaxed);
    }

    const std::size_t headroom = available - take;
    if (!window_started_ || headroom <= window_min_) {
        if (!window_started_) {
            window_started_ = true;
            window_pulled_ = 0;
        }
        window_min_ = headroom;
        steps_since_min_ = 0;
    } else {
        steps_since_min_ += step;
    }
    window_pulled_ += n;
    if (window_pulled_ >= tuning_.window_frames) {
        finish_window(window_min_, headroom);
        window_started_ = false;
    }
}

void AudioFifo::finish_window(std::size_t headroom, std::size_t headroom_now) {
    headroom_frames_.store(static_cast<std::uint32_t>(std::min<std::size_t>(headroom, UINT32_MAX)),
                           std::memory_order_relaxed);
    // The minimum is from some moment in the window; slow corrections made after it moved the
    // level since (stretching raises it, consuming extra lowers it). Counted in, so a correction is
    // not asked for twice and the level settles on the target instead of overshooting.
    const std::int64_t excess = static_cast<std::int64_t>(headroom) - steps_since_min_ -
                                static_cast<std::int64_t>(tuning_.target_frames);
    const auto margin = static_cast<std::int64_t>(tuning_.margin_frames);
    if (excess > static_cast<std::int64_t>(tuning_.hard_drop_frames)) {
        // Far too much queued (the consumer stalled, or the producer delivered a backlog). The
        // window's minimum is never more than what is queued now, so these frames exist.
        const auto drop = static_cast<std::size_t>(std::min<std::int64_t>(excess, static_cast<std::int64_t>(headroom_now)));
        read_.store(read_.load(std::memory_order_relaxed) + drop, std::memory_order_release);
        dropped_frames_.fetch_add(drop, std::memory_order_relaxed);
        pending_correction_ = 0;
        return;
    }
    // Outside the band: correct towards the target. Once correcting, carry on until within a
    // quarter of the margin, so the level settles at the target instead of at the band's edge.
    const bool correcting = pending_correction_ != 0 && (pending_correction_ > 0) == (excess > 0);
    if (excess > margin || excess < -margin || (correcting && (excess > margin / 4 || excess < -margin / 4))) {
        pending_correction_ = excess;
    } else {
        pending_correction_ = 0;
    }
}

bool AudioFifo::pull_gap(std::span<float> out) {
    const std::size_t n = out.size();
    pulled_frames_.fetch_add(n, std::memory_order_relaxed);
    std::size_t got = 0;
    if (!gap_dry_) {
        const std::uint64_t read = read_.load(std::memory_order_relaxed);
        const std::size_t available = static_cast<std::size_t>(write_.load(std::memory_order_acquire) - read);
        got = read_frames(std::min(available, n), out.data());
        gap_dry_ = got < n;
    }
    std::fill(out.begin() + static_cast<std::ptrdiff_t>(got), out.end(), 0.0f);
    underflow_frames_.fetch_add(n - got, std::memory_order_relaxed);
    return gap_dry_;
}

void AudioFifo::reset_consumer() {
    read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
    gap_dry_ = false;
    priming_ = true;
    pending_correction_ = 0;
    window_started_ = false;
}

std::size_t AudioFifo::size() const {
    const std::uint64_t read = read_.load(std::memory_order_acquire);
    const std::uint64_t write = write_.load(std::memory_order_acquire);
    return write >= read ? static_cast<std::size_t>(write - read) : 0;
}

AudioFifoStats AudioFifo::stats() const {
    AudioFifoStats s;
    s.pushed_frames = write_.load(std::memory_order_relaxed);
    s.pulled_frames = pulled_frames_.load(std::memory_order_relaxed);
    s.overflow_frames = overflow_frames_.load(std::memory_order_relaxed);
    s.underflow_frames = underflow_frames_.load(std::memory_order_relaxed);
    s.underflows = underflows_.load(std::memory_order_relaxed);
    s.dropped_frames = dropped_frames_.load(std::memory_order_relaxed);
    s.corrected_frames = corrected_frames_.load(std::memory_order_relaxed);
    s.headroom_frames = headroom_frames_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace bt
