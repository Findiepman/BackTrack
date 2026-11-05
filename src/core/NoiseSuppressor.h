#pragma once

#include "core/AudioMix.h"
#include "core/Fft.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bt {

// Takes steady background noise (fans, hum, hiss) out of a mono 48 kHz microphone signal and
// softens keyboard clicks between words, keeping speech clear.
//
// How: short-time Fourier transform (1024-point frames, 50 % overlap, square-root Hann window
// for analysis and synthesis, which reconstructs the input exactly when nothing is changed).
// Per frequency bin the noise floor is the minimum of the smoothed power over the last ~1.5 s
// (minimum statistics: speech always has gaps in every bin within that time, steady noise does
// not), and the gain is a Wiener gain on a decision-directed SNR estimate (it follows the
// previous frame's cleaned power, which keeps leftover noise from twinkling: "musical noise"),
// never below -20 dB so what is left sounds natural. A frame whose high band jumps up suddenly
// while there was no voice in the last 300 ms (a key click between words) is turned down further.
//
// The output is the input delayed by exactly latency_frames(), whether suppression is on or off,
// so switching it live only crossfades and never shifts the voice in time. Allocates only in the
// constructor; safe with silence, full-scale input and NaN/infinite samples (taken as 0).
class NoiseSuppressor {
public:
    static constexpr std::size_t kFrame = 1024;
    static constexpr std::size_t kHop = kFrame / 2;
    static constexpr std::size_t kBins = kFrame / 2 + 1;

    explicit NoiseSuppressor(bool enabled = true);

    // Live switch; crossfades over 10 ms. Off still runs the analysis, so the noise floor is
    // already known when it is switched on again.
    void set_enabled(bool enabled);
    bool enabled() const { return enabled_; }

    // Forgets the audio still inside (the last latency_frames() of input), keeping the learned
    // noise floor: after a pause or a device switch, old voice must not come out first.
    void clear_audio();

    // In place, any block size.
    void process(std::span<float> samples);

    static constexpr std::size_t latency_frames() { return kFrame; }

private:
    void process_frame();

    Fft fft_;
    std::array<float, kFrame> window_{};  // sqrt-Hann
    std::array<float, kFrame> input_{};  // the last kFrame input samples, oldest first
    std::array<float, kFrame> overlap_{};  // overlap-add accumulator
    std::array<float, kHop> ready_{};  // finished output, played out during the next hop
    std::array<float, kFrame> dry_{};  // the input delayed by kFrame, for the crossfade
    std::size_t dry_pos_ = 0;
    std::size_t hop_fill_ = 0;  // new input samples since the last frame (= read position in ready_)
    std::array<float, kFrame> re_{};
    std::array<float, kFrame> im_{};

    // Per bin.
    std::array<float, kBins> smoothed_{};  // smoothed power
    std::array<float, kBins> sub_min_{};  // minimum in the current sub-window
    std::array<float, kBins> stored_min_{};  // minimum over the finished sub-windows
    static constexpr std::size_t kSubWindows = 6;
    std::array<std::array<float, kBins>, kSubWindows> sub_windows_{};
    std::size_t sub_window_count_ = 0;  // finished sub-windows so far (up to kSubWindows)
    std::size_t sub_window_next_ = 0;
    std::size_t frames_in_sub_ = 0;
    std::array<float, kBins> prev_gain_{};
    std::array<float, kBins> prev_snr_{};
    bool first_frame_ = true;

    // Click softening.
    float prev_high_ = 0.0f;
    int voiced_run_ = 0;  // frames in a row with the voice band above the noise
    int frames_since_voice_ = 1000;
    int click_hold_ = 0;

    bool enabled_;
    GainRamp wet_;
};

}  // namespace bt
