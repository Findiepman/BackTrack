#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bt {

// How the FIFO keeps its delay steady. All in frames (48 kHz: 48 frames per millisecond).
//
// The controlled value is the *headroom*: the fewest frames left in the FIFO right after a pull,
// over the last `window_frames` pulled. The producer (the microphone) delivers in ~10 ms packets
// and the consumer (the loopback stream) pulls in ~10 ms blocks at its own moments, so the fill
// level saw-tooths; its minimum is what is left over once the jitter is taken out. The average
// delay through the FIFO is the headroom plus about one packet.
struct AudioFifoTuning {
    // 20 ms: even at the low edge of the band (15 ms) a microphone packet that comes 10 ms late
    // (a poll that woke up late) does not make it run dry.
    std::uint32_t target_frames = 960;
    std::uint32_t margin_frames = 240;  // within target +- 5 ms nothing is corrected
    std::uint32_t window_frames = 48000;  // one second between corrections
    // More headroom than target + this (a stall, a burst at startup) is dropped at once instead
    // of being caught up slowly (50 ms takes 13 s at the slow rate below; more would be audible
    // as a late voice for too long).
    std::uint32_t hard_drop_frames = 2400;  // 50 ms
    // A slow correction consumes at most one frame more or less per this many frames it outputs:
    // 1/256 = 0.4 %, a pitch change of 7 cents nobody hears, and 8 times faster than the
    // 500 ppm by which two sound card clocks differ at worst.
    std::uint32_t frames_per_correction = 256;
};

struct AudioFifoStats {
    std::uint64_t pushed_frames = 0;
    std::uint64_t pulled_frames = 0;  // output frames handed to the consumer, zeros included
    std::uint64_t overflow_frames = 0;  // pushed while full: thrown away (the consumer stopped)
    std::uint64_t underflow_frames = 0;  // zeros handed out because nothing was there
    std::uint64_t underflows = 0;  // pulls that ran dry while running
    std::uint64_t dropped_frames = 0;  // thrown away at once (hard drop)
    std::int64_t corrected_frames = 0;  // net slow correction: > 0 consumed extra, < 0 stretched
    std::uint32_t headroom_frames = 0;  // the last window's smallest fill after a pull
};

// A single-producer, single-consumer FIFO of mono float samples between two sound cards' clocks.
//
// push() is called by one thread (the producer), pull() and reset_consumer() by one other thread
// (the consumer). stats() may be called from any thread. No allocation after construction and
// no locks: both sides only take turns on two atomic counters.
//
// The two devices' clocks differ by up to a few hundred ppm, so over hours the producer would
// run away from (or behind) the consumer. pull() therefore always hands out exactly the frames
// asked for and keeps the headroom near target_frames: a little too much is consumed slightly
// faster (or too little slightly slower) by resampling the block with linear interpolation, far
// too much is dropped at once, and running dry hands out zeros and waits until target_frames are
// back before playing again.
class AudioFifo {
public:
    explicit AudioFifo(std::size_t capacity_frames, AudioFifoTuning tuning = {});
    AudioFifo(const AudioFifo&) = delete;
    AudioFifo& operator=(const AudioFifo&) = delete;

    // Producer. Returns the frames stored; the rest did not fit and is counted as overflow.
    std::size_t push(std::span<const float> frames);

    // Consumer. Fills `out` completely (zeros where nothing was available).
    void pull(std::span<float> out);

    // Consumer. Forgets everything queued and waits for target_frames again, e.g. after the
    // consumer was stopped while the producer kept pushing.
    void reset_consumer();

    // Frames queued now (a snapshot; the producer may be pushing).
    std::size_t size() const;
    std::size_t capacity() const { return buffer_.size(); }
    AudioFifoStats stats() const;

private:
    std::size_t read_frames(std::size_t count, float* out);  // consumer: copies and consumes
    void finish_window(std::size_t headroom, std::size_t headroom_now);

    std::vector<float> buffer_;  // capacity is a power of two
    std::size_t mask_ = 0;
    AudioFifoTuning tuning_;

    // Monotonic counters: write_ only by the producer, read_ only by the consumer.
    std::atomic<std::uint64_t> write_{0};
    std::atomic<std::uint64_t> read_{0};

    // Consumer only.
    bool priming_ = true;
    std::int64_t pending_correction_ = 0;  // > 0: frames still to consume extra, < 0: to stretch
    std::size_t window_pulled_ = 0;
    std::size_t window_min_ = 0;
    std::int64_t steps_since_min_ = 0;  // slow correction applied after window_min_ was seen
    std::int64_t correction_credit_ = 0;  // output frames earned towards the next corrected frame
    bool window_started_ = false;

    // Counters, written by one side each, read by stats() from anywhere.
    std::atomic<std::uint64_t> overflow_frames_{0};  // producer
    std::atomic<std::uint64_t> pulled_frames_{0}, underflow_frames_{0}, underflows_{0}, dropped_frames_{0};
    std::atomic<std::int64_t> corrected_frames_{0};
    std::atomic<std::uint32_t> headroom_frames_{0};
};

}  // namespace bt
