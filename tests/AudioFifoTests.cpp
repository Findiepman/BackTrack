#include "core/AudioFifo.h"

#include "Check.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <thread>
#include <vector>

using namespace bt;

namespace {

constexpr std::size_t kCapacity = 65536;
constexpr std::size_t kBlock = 480;  // 10 ms at 48 kHz

std::vector<float> counting(std::size_t from, std::size_t count) {
    std::vector<float> v(count);
    for (std::size_t i = 0; i < count; ++i) {
        v[i] = static_cast<float>(from + i);
    }
    return v;
}

void primes_then_plays_in_order() {
    AudioFifo fifo(kCapacity);
    const AudioFifoTuning tuning;
    std::vector<float> out(kBlock, -1.0f);
    fifo.pull(out);  // nothing yet: silence
    CHECK(std::all_of(out.begin(), out.end(), [](float v) { return v == 0.0f; }));
    CHECK_EQ(fifo.stats().underflows, std::uint64_t{0});  // waiting to start is not running dry

    // Less than one block plus the target: still waiting.
    fifo.push(counting(1, kBlock + tuning.target_frames - 1));
    fifo.pull(out);
    CHECK_EQ(out[0], 0.0f);
    fifo.push(counting(kBlock + tuning.target_frames, 1));
    fifo.pull(out);
    for (std::size_t i = 0; i < kBlock; ++i) {
        CHECK_EQ(out[i], static_cast<float>(1 + i));
    }
    CHECK_EQ(fifo.size(), std::size_t{tuning.target_frames});
}

void underflow_gives_zeros_and_waits_again() {
    AudioFifo fifo(kCapacity);
    const AudioFifoTuning tuning;
    // Enough to start, then whole blocks, then a block of which only 240 frames are there.
    const std::size_t whole = (tuning.target_frames + 240) / kBlock;
    const std::size_t total = kBlock * (whole + 1) + 240;
    fifo.push(counting(1, kBlock + tuning.target_frames + 240));
    CHECK_EQ(kBlock + tuning.target_frames + 240, total);
    std::vector<float> out(kBlock);
    for (std::size_t b = 0; b <= whole; ++b) {
        fifo.pull(out);
        CHECK_EQ(out[0], static_cast<float>(b * kBlock + 1));
    }
    std::vector<float> last(kBlock, -1.0f);
    fifo.pull(last);
    CHECK_EQ(last[0], static_cast<float>(total - 239));
    CHECK_EQ(last[239], static_cast<float>(total));
    CHECK_EQ(last[240], 0.0f);
    CHECK_EQ(last[kBlock - 1], 0.0f);
    AudioFifoStats s = fifo.stats();
    CHECK_EQ(s.underflows, std::uint64_t{1});
    CHECK_EQ(s.underflow_frames, std::uint64_t{240});
    // Primes again: one block of new data is not enough to start.
    fifo.push(counting(5000, kBlock));
    fifo.pull(out);
    CHECK_EQ(out[0], 0.0f);
    CHECK_EQ(fifo.stats().underflows, std::uint64_t{1});
    fifo.push(counting(5000 + kBlock, tuning.target_frames));
    fifo.pull(out);
    CHECK_EQ(out[0], 5000.0f);
}

void overflow_is_counted() {
    AudioFifo fifo(1000);  // rounded up to 1024
    CHECK_EQ(fifo.capacity(), std::size_t{1024});
    CHECK_EQ(fifo.push(counting(0, 1000)), std::size_t{1000});
    CHECK_EQ(fifo.push(counting(1000, 100)), std::size_t{24});
    CHECK_EQ(fifo.stats().overflow_frames, std::uint64_t{76});
    CHECK_EQ(fifo.size(), std::size_t{1024});
}

void reset_consumer_forgets_backlog() {
    AudioFifo fifo(kCapacity);
    fifo.push(counting(0, 30000));
    fifo.reset_consumer();
    CHECK_EQ(fifo.size(), std::size_t{0});
    std::vector<float> out(kBlock, -1.0f);
    fifo.pull(out);
    CHECK_EQ(out[0], 0.0f);
}

void wraps_around_the_ring() {
    AudioFifo fifo(1024, AudioFifoTuning{.target_frames = 100, .margin_frames = 50, .window_frames = 1u << 30});
    std::vector<float> out(300);
    std::size_t next_in = 1;
    std::size_t next_out = 1;
    fifo.push(counting(next_in, 400));
    next_in += 400;
    for (int round = 0; round < 50; ++round) {
        fifo.pull(out);
        for (const float v : out) {
            CHECK_EQ(v, static_cast<float>(next_out));
            ++next_out;
        }
        fifo.push(counting(next_in, 300));
        next_in += 300;
    }
}

// Deterministic jitter for the simulations.
struct Lcg {
    std::uint32_t state = 12345;
    std::uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
    double unit() { return static_cast<double>(next() % 1000000) / 1000000.0; }
};

struct DriftResult {
    std::uint64_t underflows_after_warmup = 0;
    std::size_t max_fill_after_warmup = 0;
    std::size_t min_fill_after_warmup = SIZE_MAX;
    float max_step_after_warmup = 0.0f;
    AudioFifoStats stats;
};

// Two clocks: the producer (a microphone) delivers 10 ms packets at `ratio` times the consumer's
// rate, both with up to 8 ms of scheduling jitter, and now and then a poll that comes 20 ms late
// and takes two packets or blocks at once. The signal is a 97 Hz sine, so any gap or jump shows
// up as a step between consecutive output samples (not 100 Hz: its period is exactly one block,
// and a gap could then always fall on a zero crossing and hide).
DriftResult simulate(double ratio, double hours, std::size_t extra_backlog = 0) {
    AudioFifo fifo(kCapacity);
    Lcg rng;
    DriftResult result;
    constexpr double kTickUs = 10000.0;
    constexpr double kTwoPi = 2.0 * std::numbers::pi;
    const double producer_rate = 48000.0 * ratio;  // frames per second of consumer time
    double produced_exact = 0.0;
    std::uint64_t produced = 0;
    std::vector<float> chunk;
    chunk.reserve(4 * kBlock);
    std::vector<float> out(2 * kBlock);
    float previous = 0.0f;
    bool have_previous = false;

    if (extra_backlog > 0) {
        std::vector<float> backlog(extra_backlog);
        for (std::size_t i = 0; i < extra_backlog; ++i) {
            backlog[i] = static_cast<float>(std::sin(kTwoPi * 97.0 * static_cast<double>(produced++) / producer_rate));
        }
        fifo.push(backlog);
        produced_exact = static_cast<double>(produced);  // the live packets follow the backlog
    }

    const auto ticks = static_cast<std::uint64_t>(hours * 3600.0 * 1e6 / kTickUs);
    const std::uint64_t warmup_ticks = 6000;  // 60 s
    int producer_owed = 0;
    std::size_t consumer_owed = 0;
    double next_producer = 3000.0;
    double next_consumer = 0.0;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        const double tick_end = static_cast<double>(tick + 1) * kTickUs;
        // Events within this tick in time order: one producer packet and one consumer block each.
        const double producer_at = next_producer + rng.unit() * 8000.0;
        const double consumer_at = next_consumer + rng.unit() * 8000.0;
        // A late poll (one in 200) skips this tick and does its work together with the next one:
        // 10 ms late. Longer stalls: stall_runs_dry_once_and_recovers.
        const bool producer_late = producer_owed == 0 && rng.next() % 200 == 0;
        const bool consumer_late = consumer_owed == 0 && rng.next() % 200 == 0;
        auto produce = [&] {
            ++producer_owed;
            if (producer_late) {
                return;
            }
            for (; producer_owed > 0; --producer_owed) {
                produced_exact += producer_rate * kTickUs / 1e6;
                chunk.clear();
                while (static_cast<double>(produced) < produced_exact) {
                    chunk.push_back(static_cast<float>(
                        std::sin(kTwoPi * 97.0 * static_cast<double>(produced) / producer_rate)));
                    ++produced;
                }
                fifo.push(chunk);
            }
        };
        auto consume = [&] {
            ++consumer_owed;
            if (consumer_late) {
                return;
            }
            const std::size_t frames = consumer_owed * kBlock;
            if (out.size() < frames) {
                out.resize(frames);
            }
            consumer_owed = 0;
            std::span<float> block(out.data(), frames);
            fifo.pull(block);
            if (tick >= warmup_ticks) {
                for (const float v : block) {
                    if (have_previous) {
                        result.max_step_after_warmup = std::max(result.max_step_after_warmup, std::abs(v - previous));
                    }
                    previous = v;
                    have_previous = true;
                }
                result.max_fill_after_warmup = std::max(result.max_fill_after_warmup, fifo.size());
                result.min_fill_after_warmup = std::min(result.min_fill_after_warmup, fifo.size());
            }
        };
        if (producer_at < consumer_at) {
            produce();
            consume();
        } else {
            consume();
            produce();
        }
        if (tick == warmup_ticks) {
            result.underflows_after_warmup = fifo.stats().underflows;
        }
        next_producer = tick_end;
        next_consumer = tick_end;
    }
    result.stats = fifo.stats();
    result.underflows_after_warmup = result.stats.underflows - result.underflows_after_warmup;
    return result;
}

void report(const char* name, const DriftResult& r) {
    std::printf("  %s: fill %zu..%zu frames (%.1f..%.1f ms), headroom %u, corrected %lld, dropped %llu, "
                "underflows %llu (after warm-up %llu), largest step %.4f\n",
                name, r.min_fill_after_warmup, r.max_fill_after_warmup, r.min_fill_after_warmup / 48.0,
                r.max_fill_after_warmup / 48.0, r.stats.headroom_frames,
                static_cast<long long>(r.stats.corrected_frames),
                static_cast<unsigned long long>(r.stats.dropped_frames),
                static_cast<unsigned long long>(r.stats.underflows),
                static_cast<unsigned long long>(r.underflows_after_warmup), r.max_step_after_warmup);
}

// A 97 Hz sine at 48 kHz changes by at most 2 pi 97 / 48000 = 0.0127 per sample; a resampled
// block a little more. A dropout or a jump is far larger.
constexpr float kMaxSmoothStep = 0.02f;
// Bounded latency: never more than the target, its margin and two late packets.
constexpr std::size_t kMaxFill = 960 + 240 + 4 * kBlock;

void drift_faster_producer_stays_bounded() {
    const DriftResult r = simulate(1.0005, 1.0);
    report("mic clock 1.0005", r);
    CHECK_EQ(r.underflows_after_warmup, std::uint64_t{0});
    CHECK(r.max_fill_after_warmup <= kMaxFill);
    CHECK(r.stats.corrected_frames > 0);  // it consumed extra to keep up
    // An hour at +500 ppm is 86 400 frames too many; all of it taken out by slow correction.
    CHECK(r.stats.corrected_frames > 80000);
    CHECK_EQ(r.stats.dropped_frames, std::uint64_t{0});
    CHECK(r.max_step_after_warmup < kMaxSmoothStep);
}

void drift_slower_producer_stays_bounded() {
    const DriftResult r = simulate(0.9995, 1.0);
    report("mic clock 0.9995", r);
    CHECK_EQ(r.underflows_after_warmup, std::uint64_t{0});
    CHECK(r.max_fill_after_warmup <= kMaxFill);
    CHECK(r.stats.corrected_frames < -80000);  // it stretched instead of running dry
    CHECK(r.max_step_after_warmup < kMaxSmoothStep);
}

void same_clock_needs_almost_nothing() {
    const DriftResult r = simulate(1.0, 0.5);
    report("same clock", r);
    CHECK_EQ(r.underflows_after_warmup, std::uint64_t{0});
    CHECK(r.max_fill_after_warmup <= kMaxFill);
    CHECK(std::llabs(r.stats.corrected_frames) < 2000);
}

void backlog_is_dropped_at_once() {
    // A 500 ms backlog at the start (WASAPI handed over what it had buffered): dropped in the
    // first second, not caught up over minutes.
    const DriftResult r = simulate(1.0002, 0.1, 24000);
    report("500 ms backlog", r);
    CHECK(r.stats.dropped_frames > 20000);
    CHECK(r.max_fill_after_warmup <= kMaxFill);
    CHECK_EQ(r.underflows_after_warmup, std::uint64_t{0});
}

void stays_in_band_without_overshoot() {
    // Without jitter, steady drift either way keeps the headroom within the band and never
    // pushes it past the target. Stretching raises the level during a window, so a correction that
    // forgot what it did since the window's minimum would overshoot.
    for (const double ratio : {0.9995, 1.0005}) {
        AudioFifo fifo(kCapacity);
        std::vector<float> out(kBlock);
        std::vector<float> chunk;
        double exact = 0.0;
        std::uint64_t produced = 0;
        double sum = 0.0;
        int samples = 0;
        std::uint32_t largest = 0;
        std::uint32_t smallest = UINT32_MAX;
        for (int tick = 0; tick < 6 * 60 * 100; ++tick) {
            exact += kBlock * ratio;
            chunk.assign(static_cast<std::size_t>(static_cast<std::uint64_t>(exact) - produced), 0.25f);
            produced += chunk.size();
            fifo.push(chunk);
            fifo.pull(out);
            if (tick >= 2 * 60 * 100 && tick % 100 == 0) {
                const std::uint32_t h = fifo.stats().headroom_frames;
                sum += h;
                ++samples;
                largest = std::max(largest, h);
                smallest = std::min(smallest, h);
            }
        }
        const double mean = sum / samples;
        std::printf("  settled at clock %.4f: headroom %u..%u, mean %.0f frames (target 960)\n", ratio, smallest,
                    largest, mean);
        // A dead band: the drift carries the level to the band's edge (plus up to one window of
        // drift, 24 frames at 500 ppm), a correction brings it back to near the target, and never
        // past it to the other side.
        CHECK(smallest + 240 + 48 >= 960 && largest <= 960 + 240 + 48);
        if (ratio < 1.0) {
            CHECK(largest <= 960 + 60);
        } else {
            CHECK(smallest + 60 >= 960);
        }
    }
}

void stall_runs_dry_once_and_recovers() {
    // The microphone thread stalls for 100 ms (a busy PC), then delivers the 10 packets at once:
    // one short dropout, then the extra delay is dropped at once instead of
    // staying in the voice for good.
    AudioFifo fifo(kCapacity);
    std::vector<float> packet(kBlock, 0.5f);
    std::vector<float> out(kBlock);
    int owed = 0;
    std::size_t max_fill_late = 0;
    for (int tick = 0; tick < 6000; ++tick) {
        ++owed;
        if (tick < 1000 || tick >= 1010) {
            for (; owed > 0; --owed) {
                fifo.push(packet);
            }
        }
        fifo.pull(out);
        if (tick > 1250) {  // 2.5 s after the stall
            max_fill_late = std::max(max_fill_late, fifo.size());
        }
    }
    const AudioFifoStats s = fifo.stats();
    std::printf("  100 ms stall: %llu underflow, %llu frames dropped afterwards, fill afterwards up to %zu\n",
                static_cast<unsigned long long>(s.underflows), static_cast<unsigned long long>(s.dropped_frames),
                max_fill_late);
    CHECK_EQ(s.underflows, std::uint64_t{1});
    CHECK(max_fill_late <= kMaxFill);
}

void gap_fill_does_not_scatter_live_voice() {
    // The loopback fills a 3 s gap in 100 ms chunks, far faster than real time. The FIFO holds
    // the 1 s that was said during the gap (values 1..48000); meanwhile the microphone keeps pushing
    // live voice (values 900000+). The fill must play everything in order until it runs dry, then
    // only zeros: never re-prime and drop later live voice into the past.
    AudioFifo fifo(kCapacity);
    fifo.push(counting(1, 48000));
    std::vector<float> chunk(4800);
    std::vector<float> played;
    bool dry = false;
    std::size_t live = 900000;
    for (int i = 0; i < 30; ++i) {
        dry = fifo.pull_gap(chunk) || dry;
        played.insert(played.end(), chunk.begin(), chunk.end());
        fifo.push(counting(live, 480));  // live voice arriving while the fill runs
        live += 480;
    }
    CHECK(dry);
    bool in_order = true;
    for (std::size_t i = 0; i < 48000; ++i) {
        in_order = in_order && played[i] == static_cast<float>(i + 1);
    }
    CHECK(in_order);
    // Frames that arrived before it ran dry follow on in order (they are the newest voice, right
    // behind the queued second); from the first zero on, only zeros: no re-priming that would drop
    // later live voice into the past.
    std::size_t i = 48000;
    float expected = 900000.0f;
    while (i < played.size() && played[i] != 0.0f) {
        CHECK_EQ(played[i], expected);
        expected += 1.0f;
        ++i;
    }
    CHECK(i < played.size());
    bool zeros_after = true;
    for (; i < played.size(); ++i) {
        zeros_after = zeros_after && played[i] == 0.0f;
    }
    CHECK(zeros_after);
    CHECK_EQ(fifo.stats().underflows, std::uint64_t{0});

    // After the gap: forget what came in during the fill, then the live voice plays from now on.
    fifo.reset_consumer();
    fifo.push(counting(2000000, kBlock + 960));
    std::vector<float> out(kBlock);
    fifo.pull(out);
    CHECK_EQ(out[0], 2000000.0f);

    // A gap that does not run dry leaves the FIFO running as it was.
    AudioFifo enough(kCapacity);
    enough.push(counting(1, 2000));
    std::vector<float> small(480);
    CHECK(!enough.pull_gap(small));
    CHECK_EQ(small[0], 1.0f);
    CHECK_EQ(enough.size(), std::size_t{1520});
}

void two_threads() {
    // The real thing: a producer and a consumer thread at the same time. Every value comes out in
    // order (no slow correction can start: the window is longer than the run). The hard drop is
    // switched off too: when the test machine is busy the consumer thread can be held up while the
    // producer fills the FIFO, and the next priming then drops that backlog on purpose
    // (backlog_is_dropped_at_once), which shows up here as a jump in the count (measured: with 16
    // copies running at once about half the runs jumped by exactly the dropped frames, and two
    // drops kept `seen` from ever reaching the end, so the test hung).
    AudioFifo fifo(4096, AudioFifoTuning{.target_frames = 512, .margin_frames = 256, .window_frames = 1u << 30,
                                         .hard_drop_frames = 1u << 30});
    constexpr std::size_t kTotal = 2'000'000;
    std::thread producer([&fifo] {
        std::size_t next = 1;
        std::vector<float> chunk(256);
        while (next <= kTotal) {
            const std::size_t count = std::min<std::size_t>(chunk.size(), kTotal - next + 1);
            for (std::size_t i = 0; i < count; ++i) {
                chunk[i] = static_cast<float>(next + i);
            }
            std::size_t done = 0;
            while (done < count) {
                done += fifo.push(std::span<const float>(chunk.data() + done, count - done));
                if (done < count) {
                    std::this_thread::yield();
                }
            }
            next += count;
        }
    });
    std::vector<float> out(200);
    float expected = 1.0f;
    bool in_order = true;
    std::uint64_t seen = 0;
    // Exact float counting is only reliable below 2^24.
    while (seen < kTotal - 4096 && expected < 16'000'000.0f) {
        fifo.pull(out);
        for (const float v : out) {
            if (v == 0.0f) {
                continue;  // priming or ran dry: silence, never a wrong value
            }
            if (v != expected) {
                in_order = false;
            }
            expected = v + 1.0f;
            ++seen;
        }
    }
    producer.join();
    CHECK(in_order);
    CHECK_EQ(fifo.stats().dropped_frames, std::uint64_t{0});
}

}  // namespace

int main() {
    primes_then_plays_in_order();
    underflow_gives_zeros_and_waits_again();
    overflow_is_counted();
    reset_consumer_forgets_backlog();
    wraps_around_the_ring();
    drift_faster_producer_stays_bounded();
    drift_slower_producer_stays_bounded();
    same_clock_needs_almost_nothing();
    backlog_is_dropped_at_once();
    stays_in_band_without_overshoot();
    stall_runs_dry_once_and_recovers();
    gap_fill_does_not_scatter_live_voice();
    two_threads();
    return bt_test::finish("AudioFifoTests");
}
