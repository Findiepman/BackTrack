#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace bt {

// A gain that moves to a new value in a straight line over ramp_frames instead of jumping, so
// muting, push-to-talk and volume changes never click. It lands exactly on the target, so a gain
// of exactly 1 or 0 can be recognised and skipped.
class GainRamp {
public:
    GainRamp(float initial, std::uint32_t ramp_frames);

    void set_target(float target);
    float target() const { return target_; }
    float current() const { return current_; }
    bool settled() const { return remaining_ == 0; }

    // Advances one frame and returns the gain for it.
    float next() {
        if (remaining_ != 0) {
            --remaining_;
            current_ = remaining_ == 0 ? target_ : current_ + step_;
        }
        return current_;
    }

private:
    float current_;
    float target_;
    float step_ = 0.0f;
    std::uint32_t ramp_frames_;
    std::uint32_t remaining_ = 0;
};

// The level shown in the UI: the highest peak, falling by half every `half_life_frames` after it.
class PeakMeter {
public:
    explicit PeakMeter(float half_life_frames) : half_life_frames_(half_life_frames) {}

    // block_peak: the largest absolute sample of a block of `frames`, full scale = 1.
    void add(float block_peak, std::size_t frames);
    float level() const { return level_; }  // 0..1
    void reset() { level_ = 0.0f; }

private:
    float half_life_frames_;
    float level_ = 0.0f;
};

struct MixPeaks {
    float system = 0.0f;  // largest |sample| of the system part after its gain, full scale = 1
    float mic = 0.0f;  // the same for the microphone part
};

// Mixes one block in place: stereo = stereo * system_gain + mic * mic_gain on both channels,
// saturated to int16. `stereo` is interleaved L R L R; `mic` is mono at the same rate with one
// sample per stereo frame (full scale = 1), or empty when there is no microphone. Both gains
// advance one step per frame. While system_gain is settled at exactly 1 and the microphone adds
// nothing (empty, or mic_gain settled at 0) the samples are not touched at all, so the sound is
// bit for bit what it was without a microphone.
//
// Separate tracks: `system_part` and `mic_part` (each empty, or as large as `stereo`) receive the
// two parts on their own, from the same gains on the same frame: stereo * system_gain, and the
// microphone * mic_gain on both channels, each saturated to int16. `stereo` gets exactly the mix
// it gets without them, so the mixed track is bit for bit the same with separate tracks on or off.
MixPeaks mix_block(std::span<std::int16_t> stereo, std::span<const float> mic, GainRamp& system_gain,
                   GainRamp& mic_gain, std::span<std::int16_t> system_part = {}, std::span<std::int16_t> mic_part = {});

}  // namespace bt
