#include "core/AudioMix.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace bt {
namespace {

constexpr float kInt16Scale = 32768.0f;
constexpr float kInt16Min = -32768.0f;
constexpr float kInt16Max = 32767.0f;

std::int16_t saturate(float value) {
    // Clamped before the conversion: converting an out-of-range float to an integer is undefined.
    return static_cast<std::int16_t>(std::lrintf(std::clamp(value, kInt16Min, kInt16Max)));
}

}  // namespace

GainRamp::GainRamp(float initial, std::uint32_t ramp_frames)
    : current_(initial), target_(initial), ramp_frames_(std::max<std::uint32_t>(1, ramp_frames)) {}

void GainRamp::set_target(float target) {
    if (target == target_) {
        return;
    }
    target_ = target;
    remaining_ = ramp_frames_;
    step_ = (target_ - current_) / static_cast<float>(ramp_frames_);
}

void PeakMeter::add(float block_peak, std::size_t frames) {
    const float decayed =
        level_ * std::exp2(-static_cast<float>(frames) / std::max(1.0f, half_life_frames_));
    level_ = std::clamp(std::max(block_peak, decayed), 0.0f, 1.0f);
    if (level_ < 1e-6f) {
        level_ = 0.0f;  // no endless tail of tiny (eventually denormal) values
    }
}

MixPeaks mix_block(std::span<std::int16_t> stereo, std::span<const float> mic, GainRamp& system_gain,
                   GainRamp& mic_gain, std::span<std::int16_t> system_part, std::span<std::int16_t> mic_part) {
    const std::size_t frames = stereo.size() / 2;
    if (!mic.empty() && mic.size() != frames) {
        throw std::invalid_argument("mix_block: the mic block must have one sample per stereo frame");
    }
    if ((!system_part.empty() && system_part.size() != stereo.size()) ||
        (!mic_part.empty() && mic_part.size() != stereo.size())) {
        throw std::invalid_argument("mix_block: a separate part must be as large as the stereo block");
    }
    MixPeaks peaks;
    const bool mic_silent = mic.empty() || (mic_gain.settled() && mic_gain.current() == 0.0f);
    if (mic_silent && system_gain.settled() && system_gain.current() == 1.0f) {
        int largest = 0;
        for (const std::int16_t s : stereo) {
            largest = std::max(largest, std::abs(static_cast<int>(s)));
        }
        peaks.system = static_cast<float>(largest) / kInt16Scale;
        // The gains do not move while they are settled, so skipping their steps changes nothing.
        if (!system_part.empty()) {
            std::copy(stereo.begin(), stereo.end(), system_part.begin());
        }
        std::fill(mic_part.begin(), mic_part.end(), std::int16_t{0});
        return peaks;
    }
    for (std::size_t i = 0; i < frames; ++i) {
        const float gs = system_gain.next();
        const float gm = mic_gain.next();
        const float left = static_cast<float>(stereo[2 * i]) * gs;
        const float right = static_cast<float>(stereo[2 * i + 1]) * gs;
        peaks.system = std::max(peaks.system, std::max(std::abs(left), std::abs(right)));
        float voice = 0.0f;
        if (!mic.empty()) {
            voice = mic[i] * gm * kInt16Scale;
            peaks.mic = std::max(peaks.mic, std::abs(voice));
        }
        stereo[2 * i] = saturate(left + voice);
        stereo[2 * i + 1] = saturate(right + voice);
        if (!system_part.empty()) {
            system_part[2 * i] = saturate(left);
            system_part[2 * i + 1] = saturate(right);
        }
        if (!mic_part.empty()) {
            mic_part[2 * i] = saturate(voice);
            mic_part[2 * i + 1] = mic_part[2 * i];
        }
    }
    peaks.system = std::min(1.0f, peaks.system / kInt16Scale);
    peaks.mic = std::min(1.0f, peaks.mic / kInt16Scale);
    return peaks;
}

}  // namespace bt
