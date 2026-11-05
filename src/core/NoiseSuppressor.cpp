#include "core/NoiseSuppressor.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace bt {
namespace {

constexpr std::uint32_t kCrossfadeFrames = 480;  // 10 ms at 48 kHz

// Smoothing of the power per bin before its minimum is taken: about 50 ms of memory.
constexpr float kPowerSmoothing = 0.8f;
// 24 frames of 10.7 ms per sub-window, 6 sub-windows: the minimum covers 1.3 to 1.5 s. Long enough
// that every bin of continuous speech has a quiet moment in it, short enough to follow a fan
// that speeds up.
constexpr std::size_t kFramesPerSubWindow = 24;
// The minimum of a noisy power is below its mean. Measured on white and pink noise with these
// constants (NoiseSuppressorTests prints it): the minimum is about 0.55 of the mean; a slightly
// higher factor also takes the noise's own peaks, which is what keeps the leftover steady.
constexpr float kMinimumBias = 2.2f;
// Decision-directed a priori SNR (Ephraim and Malah): weight of the previous frame's cleaned power.
constexpr float kDecisionDirected = 0.98f;
constexpr float kGainFloor = 0.1f;  // -20 dB: noise is turned down, never cut to dead silence
constexpr float kPowerFloor = 1e-12f;  // no denormals in the recursions on digital silence

// 46.875 Hz per bin. The voice band holds the fundamental and first formant of every voice; the
// high band is where a key click has its energy and voiced speech has little.
constexpr std::size_t kVoiceLowBin = 3;  // 140 Hz
constexpr std::size_t kVoiceHighBin = 22;  // 1030 Hz
constexpr std::size_t kClickLowBin = 64;  // 3 kHz
constexpr std::size_t kClickHighBin = 342;  // 16 kHz
constexpr float kVoiceSnr = 4.0f;  // 6 dB above the noise floor in the voice band
constexpr int kVoiceFrames = 3;  // that long in a row (32 ms) is a voice, not a click's thump
constexpr float kClickSnr = 8.0f;  // the high band 9 dB above its noise floor...
constexpr float kClickJump = 4.0f;  // ...and 6 dB above the frame before
constexpr int kNoVoiceFrames = 28;  // no voice in the last 300 ms: between words
constexpr int kClickHoldFrames = 4;  // a click and its ring, ~40 ms
constexpr float kClickGain = 0.25f;  // -12 dB

}  // namespace

NoiseSuppressor::NoiseSuppressor(bool enabled)
    : fft_(kFrame), enabled_(enabled), wet_(enabled ? 1.0f : 0.0f, kCrossfadeFrames) {
    for (std::size_t n = 0; n < kFrame; ++n) {
        // Periodic Hann, square-rooted: applied twice (analysis and synthesis) it is a Hann
        // window, whose copies 50 % apart add up to exactly 1.
        const double hann = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) / kFrame);
        window_[n] = static_cast<float>(std::sqrt(hann));
    }
    prev_gain_.fill(1.0f);
    prev_snr_.fill(1.0f);
}

void NoiseSuppressor::clear_audio() {
    input_.fill(0.0f);
    overlap_.fill(0.0f);
    ready_.fill(0.0f);
    dry_.fill(0.0f);
    dry_pos_ = 0;
    hop_fill_ = 0;
    prev_high_ = 0.0f;
    click_hold_ = 0;
}

void NoiseSuppressor::set_enabled(bool enabled) {
    enabled_ = enabled;
    wet_.set_target(enabled ? 1.0f : 0.0f);
}

void NoiseSuppressor::process(std::span<float> samples) {
    for (float& sample : samples) {
        float x = sample;
        // A bad sample must not poison the recursions forever. Below 1e-15 (-300 dBFS) is zero: tiny
        // inputs times the window edges, the gain floor and 1/N would end up as slow denormals.
        if (!std::isfinite(x) || std::abs(x) < 1e-15f) {
            x = 0.0f;
        }
        const float processed = ready_[hop_fill_];
        const float dry = dry_[dry_pos_];
        dry_[dry_pos_] = x;
        dry_pos_ = (dry_pos_ + 1) % kFrame;

        input_[kHop + hop_fill_] = x;
        if (++hop_fill_ == kHop) {
            process_frame();
            std::copy(input_.begin() + kHop, input_.end(), input_.begin());
            hop_fill_ = 0;
        }

        const float wet = wet_.next();
        sample = wet == 1.0f ? processed : wet == 0.0f ? dry : wet * processed + (1.0f - wet) * dry;
    }
}

void NoiseSuppressor::process_frame() {
    for (std::size_t n = 0; n < kFrame; ++n) {
        re_[n] = input_[n] * window_[n];
        im_[n] = 0.0f;
    }
    fft_.transform(re_, im_, false);

    std::array<float, kBins> power{};
    for (std::size_t k = 0; k < kBins; ++k) {
        power[k] = std::max(re_[k] * re_[k] + im_[k] * im_[k], kPowerFloor);
    }

    // Minimum statistics.
    if (first_frame_) {
        first_frame_ = false;
        smoothed_ = power;
        sub_min_ = power;
    } else {
        for (std::size_t k = 0; k < kBins; ++k) {
            smoothed_[k] = std::max(kPowerSmoothing * smoothed_[k] + (1.0f - kPowerSmoothing) * power[k], kPowerFloor);
            sub_min_[k] = std::min(sub_min_[k], smoothed_[k]);
        }
    }
    if (++frames_in_sub_ == kFramesPerSubWindow) {
        frames_in_sub_ = 0;
        sub_windows_[sub_window_next_] = sub_min_;
        sub_window_next_ = (sub_window_next_ + 1) % kSubWindows;
        sub_window_count_ = std::min(sub_window_count_ + 1, kSubWindows);
        stored_min_ = sub_windows_[0];
        for (std::size_t w = 1; w < sub_window_count_; ++w) {
            for (std::size_t k = 0; k < kBins; ++k) {
                stored_min_[k] = std::min(stored_min_[k], sub_windows_[w][k]);
            }
        }
        sub_min_ = smoothed_;
    }

    std::array<float, kBins> noise{};
    for (std::size_t k = 0; k < kBins; ++k) {
        const float minimum = sub_window_count_ > 0 ? std::min(stored_min_[k], sub_min_[k]) : sub_min_[k];
        noise[k] = std::max(kMinimumBias * minimum, kPowerFloor);
    }

    // Key clicks between words.
    float voice = 0.0f;
    float voice_noise = 0.0f;
    for (std::size_t k = kVoiceLowBin; k < kVoiceHighBin; ++k) {
        voice += power[k];
        voice_noise += noise[k];
    }
    float high = 0.0f;
    float high_noise = 0.0f;
    for (std::size_t k = kClickLowBin; k < kClickHighBin; ++k) {
        high += power[k];
        high_noise += noise[k];
    }
    const bool voice_now = voice > kVoiceSnr * voice_noise;
    voiced_run_ = voice_now ? voiced_run_ + 1 : 0;
    if (voiced_run_ >= kVoiceFrames) {
        frames_since_voice_ = 0;
    } else if (frames_since_voice_ < kNoVoiceFrames + 1) {
        ++frames_since_voice_;
    }
    const bool onset = high > kClickSnr * high_noise && high > kClickJump * prev_high_;
    prev_high_ = high;
    if (onset && !voice_now && frames_since_voice_ > kNoVoiceFrames) {
        click_hold_ = kClickHoldFrames;
    }
    if (voice_now) {
        click_hold_ = 0;  // a voice is starting: never turn it down
    }
    const float gain_cap = click_hold_ > 0 ? kClickGain : 1.0f;
    if (click_hold_ > 0) {
        --click_hold_;
    }

    for (std::size_t k = 0; k < kBins; ++k) {
        const float snr = power[k] / noise[k];
        const float prior = kDecisionDirected * prev_gain_[k] * prev_gain_[k] * prev_snr_[k] +
                            (1.0f - kDecisionDirected) * std::max(snr - 1.0f, 0.0f);
        const float gain = std::min(std::max(prior / (1.0f + prior), kGainFloor), gain_cap);
        prev_gain_[k] = gain;
        prev_snr_[k] = std::min(snr, 1e12f);  // bounded: full scale over a silent floor
        re_[k] *= gain;
        im_[k] *= gain;
    }
    for (std::size_t k = 1; k < kFrame / 2; ++k) {
        re_[kFrame - k] = re_[k];
        im_[kFrame - k] = -im_[k];
    }
    fft_.transform(re_, im_, true);

    for (std::size_t n = 0; n < kFrame; ++n) {
        overlap_[n] += re_[n] * window_[n];
    }
    std::copy(overlap_.begin(), overlap_.begin() + kHop, ready_.begin());
    std::copy(overlap_.begin() + kHop, overlap_.end(), overlap_.begin());
    std::fill(overlap_.begin() + kHop, overlap_.end(), 0.0f);
}

}  // namespace bt
