#include "core/Fft.h"
#include "core/NoiseSuppressor.h"

#include "Check.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <random>
#include <vector>

using namespace bt;

namespace {

constexpr double kRate = 48000.0;
constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr std::size_t kLatency = NoiseSuppressor::latency_frames();

double energy(const std::vector<float>& v, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t i = from; i < to && i < v.size(); ++i) {
        sum += static_cast<double>(v[i]) * v[i];
    }
    return sum;
}

double db(double ratio) {
    return 10.0 * std::log10(std::max(ratio, 1e-30));
}

std::vector<float> white_noise(std::size_t n, float rms, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> dist(0.0f, rms);
    std::vector<float> v(n);
    for (float& x : v) {
        x = dist(rng);
    }
    return v;
}

// Paul Kellet's pink noise filter over white noise: -3 dB per octave like a fan or room rumble.
std::vector<float> pink_noise(std::size_t n, float rms, unsigned seed) {
    const std::vector<float> white = white_noise(n, 1.0f, seed);
    std::vector<float> v(n);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = white[i];
        b0 = 0.99886 * b0 + w * 0.0555179;
        b1 = 0.99332 * b1 + w * 0.0750759;
        b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522;
        b5 = -0.7616 * b5 - w * 0.0168980;
        v[i] = static_cast<float>(b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362);
        b6 = w * 0.115926;
    }
    const double scale = rms / std::sqrt(energy(v, 0, n) / static_cast<double>(n));
    for (float& x : v) {
        x = static_cast<float>(x * scale);
    }
    return v;
}

std::vector<float> run(std::vector<float> signal, bool enabled = true) {
    NoiseSuppressor ns(enabled);
    // Odd block sizes on purpose: the result must not depend on how the input is cut up.
    std::size_t at = 0;
    std::size_t block = 1;
    while (at < signal.size()) {
        const std::size_t count = std::min(block, signal.size() - at);
        ns.process(std::span<float>(signal.data() + at, count));
        at += count;
        block = block * 7 % 997 + 1;
    }
    return signal;
}

void fft_round_trip_and_known_values() {
    Fft fft(1024);
    std::vector<float> re(1024), im(1024, 0.0f);
    for (std::size_t n = 0; n < 1024; ++n) {
        re[n] = static_cast<float>(std::cos(kTwoPi * 5.0 * static_cast<double>(n) / 1024.0));
    }
    const std::vector<float> original = re;
    fft.transform(re, im, false);
    // A cosine at bin 5: N/2 in bins 5 and N-5, nothing elsewhere.
    CHECK(std::abs(re[5] - 512.0f) < 1e-2f);
    CHECK(std::abs(re[1019] - 512.0f) < 1e-2f);
    CHECK(std::abs(re[6]) < 1e-2f && std::abs(im[5]) < 1e-2f);
    fft.transform(re, im, true);
    float worst = 0.0f;
    for (std::size_t n = 0; n < 1024; ++n) {
        worst = std::max(worst, std::abs(re[n] - original[n]));
        worst = std::max(worst, std::abs(im[n]));
    }
    CHECK(worst < 1e-5f);
    // An impulse is flat.
    std::vector<float> ir(8, 0.0f), ii(8, 0.0f);
    ir[0] = 1.0f;
    Fft small(8);
    small.transform(ir, ii, false);
    for (std::size_t k = 0; k < 8; ++k) {
        CHECK(std::abs(ir[k] - 1.0f) < 1e-6f && std::abs(ii[k]) < 1e-6f);
    }
}

void off_is_an_exact_delay() {
    const std::vector<float> input = white_noise(48000, 0.3f, 1);
    const std::vector<float> output = run(input, false);
    bool exact = true;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const float expected = i < kLatency ? 0.0f : input[i - kLatency];
        exact = exact && output[i] == expected;
    }
    CHECK(exact);
}

double noise_reduction_db(const std::vector<float>& input) {
    const std::vector<float> output = run(input);
    // After 3 s to learn the floor, over the rest.
    const std::size_t from = 3 * 48000;
    return db(energy(output, from + kLatency, output.size()) / energy(input, from, output.size() - kLatency));
}

void steady_noise_is_reduced() {
    const double white = noise_reduction_db(white_noise(10 * 48000, 0.05f, 2));
    const double pink = noise_reduction_db(pink_noise(10 * 48000, 0.05f, 3));
    const double quiet_white = noise_reduction_db(white_noise(10 * 48000, 0.0005f, 4));  // -66 dBFS hiss
    std::vector<float> hum(10 * 48000);
    for (std::size_t i = 0; i < hum.size(); ++i) {
        const double t = static_cast<double>(i) / kRate;
        hum[i] = static_cast<float>(0.05 * std::sin(kTwoPi * 50.0 * t) + 0.03 * std::sin(kTwoPi * 150.0 * t) +
                                    0.02 * std::sin(kTwoPi * 250.0 * t));
    }
    const std::vector<float> hum_noise = white_noise(hum.size(), 0.005f, 5);
    for (std::size_t i = 0; i < hum.size(); ++i) {
        hum[i] += hum_noise[i];
    }
    const double mains = noise_reduction_db(hum);
    std::printf("  noise reduction: white %.1f dB, pink %.1f dB, quiet hiss %.1f dB, mains hum %.1f dB\n", white,
                pink, quiet_white, mains);
    CHECK(white < -10.0);
    CHECK(pink < -10.0);
    CHECK(quiet_white < -10.0);
    CHECK(mains < -10.0);
}

// A voice-like sound: a 140 Hz buzz with falling harmonics up to 4 kHz, gliding in pitch, in
// syllables of 180 ms with 120 ms between them, and a 400 ms pause every 5 syllables.
std::vector<float> speech_like(std::size_t n, std::vector<bool>& voiced) {
    std::vector<float> v(n, 0.0f);
    voiced.assign(n, false);
    double phase = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kRate;
        const double cycle = std::fmod(t, 1.9);  // 5 syllables of 0.3 s plus a 0.4 s pause
        const double in_syllable = std::fmod(cycle, 0.3);
        if (cycle >= 1.5 || in_syllable >= 0.18) {
            continue;
        }
        const double f0 = 140.0 + 30.0 * std::sin(kTwoPi * 0.7 * t);
        phase += kTwoPi * f0 / kRate;
        const double envelope = std::sin(std::numbers::pi * in_syllable / 0.18);
        double sample = 0.0;
        for (int h = 1; h * f0 < 4000.0; ++h) {
            sample += std::sin(phase * h) / h;
        }
        v[i] = static_cast<float>(0.15 * envelope * sample);
        voiced[i] = envelope > 0.3;
    }
    return v;
}

void speech_is_kept() {
    const std::size_t n = 12 * 48000;
    std::vector<bool> voiced;
    const std::vector<float> speech = speech_like(n, voiced);
    const std::vector<float> noise = pink_noise(n, 0.01f, 6);  // speech about 20 dB above the noise
    std::vector<float> mixed(n);
    for (std::size_t i = 0; i < n; ++i) {
        mixed[i] = speech[i] + noise[i];
    }
    const std::vector<float> output = run(mixed);
    double speech_in = 0.0, speech_out = 0.0, gap_in = 0.0, gap_out = 0.0, error = 0.0;
    for (std::size_t i = 3 * 48000; i + kLatency < n; ++i) {
        const double out = output[i + kLatency];
        if (voiced[i]) {
            speech_in += static_cast<double>(speech[i]) * speech[i];
            speech_out += out * out;
            error += (out - speech[i]) * (out - speech[i]);
        } else if (speech[i] == 0.0f) {
            gap_in += static_cast<double>(noise[i]) * noise[i];
            gap_out += out * out;
        }
    }
    const double kept = db(speech_out / speech_in);
    const double gaps = db(gap_out / gap_in);
    const double snr_in = db(speech_in / energy(noise, 3 * 48000, n));
    const double distortion = db(error / speech_in);
    std::printf("  speech in noise: speech energy kept %.1f dB, error vs clean speech %.1f dB, noise between "
                "syllables %.1f dB (input SNR %.1f dB)\n",
                kept, distortion, gaps, snr_in);
    CHECK(kept > -2.0 && kept < 1.0);
    CHECK(distortion < -10.0);  // what comes out is the speech, not a muffled version of it
    CHECK(gaps < -8.0);
}

void key_clicks_between_words_are_softened() {
    const std::size_t n = 10 * 48000;
    std::vector<float> signal = pink_noise(n, 0.002f, 7);
    std::vector<float> clicks(n, 0.0f);
    std::mt19937 rng(8);
    std::normal_distribution<float> dist(0.0f, 1.0f);
    // A click every 270 ms: 4 ms of high-passed noise with a fast decay.
    for (std::size_t start = 48000; start + 400 < n; start += 12960) {
        float previous = 0.0f;
        for (std::size_t i = 0; i < 200; ++i) {
            const float w = dist(rng);
            const float hp = w - previous;
            previous = w;
            clicks[start + i] = 0.15f * hp * std::exp(-static_cast<float>(i) / 50.0f);
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        signal[i] += clicks[i];
    }
    const std::vector<float> output = run(signal);
    // Only the clicks themselves (the steady noise around them is turned down anyway): each click's
    // 4 ms plus its ring, in and out.
    double in = 0.0;
    double out = 0.0;
    for (std::size_t start = 48000; start + 600 + kLatency < n; start += 12960) {
        if (start < 3 * 48000) {
            continue;  // still learning the noise floor
        }
        in += energy(signal, start, start + 600);
        out += energy(output, start + kLatency, start + 600 + kLatency);
    }
    const double reduction = db(out / in);
    std::printf("  key clicks between words: %.1f dB on the clicks\n", reduction);
    // A Wiener gain alone lets a loud click through almost whole; the click softening takes ~12 dB.
    CHECK(reduction < -6.0);
}

void survives_silence_full_scale_and_garbage() {
    std::vector<float> silence(5 * 48000, 0.0f);
    silence = run(silence);
    bool zeros = true;
    for (const float v : silence) {
        zeros = zeros && v == 0.0f;
    }
    CHECK(zeros);

    // Full-scale square wave, then digital silence (the recursions must come back down cleanly),
    // then a quiet signal again.
    std::vector<float> loud(6 * 48000, 0.0f);
    for (std::size_t i = 0; i < 2 * 48000; ++i) {
        loud[i] = (i / 100) % 2 == 0 ? 1.0f : -1.0f;
    }
    const std::vector<float> tail = white_noise(48000, 0.01f, 9);
    for (std::size_t i = 0; i < tail.size(); ++i) {
        loud[5 * 48000 + i] = tail[i];
    }
    loud[100] = std::numeric_limits<float>::quiet_NaN();
    loud[200] = std::numeric_limits<float>::infinity();
    loud[300] = 1e-40f;  // a denormal
    const std::vector<float> out = run(loud);
    bool finite = true;
    float largest = 0.0f;
    for (const float v : out) {
        finite = finite && std::isfinite(v);
        largest = std::max(largest, std::abs(v));
    }
    CHECK(finite);
    CHECK(largest < 1.5f);
    CHECK(energy(out, 5 * 48000 + kLatency, out.size()) > 0.0);  // still alive afterwards
}

void switching_crossfades() {
    // On -> off -> on in the middle of noise: no sample jumps beyond what the signals themselves do.
    std::vector<float> input = white_noise(4 * 48000, 0.05f, 10);
    NoiseSuppressor ns(true);
    std::vector<float> output = input;
    ns.process(std::span<float>(output.data(), 2 * 48000));
    CHECK(ns.enabled());
    ns.set_enabled(false);
    CHECK(!ns.enabled());
    ns.process(std::span<float>(output.data() + 2 * 48000, 48000));
    ns.set_enabled(true);
    ns.process(std::span<float>(output.data() + 3 * 48000, 48000));
    // While off (after the 10 ms crossfade) it is exactly the delayed input.
    bool exact = true;
    for (std::size_t i = 2 * 48000 + 480; i < 3 * 48000; ++i) {
        exact = exact && output[i] == input[i - kLatency];
    }
    CHECK(exact);
    // Back on: suppressing straight away, no relearning.
    const double after = db(energy(output, 3 * 48000 + 480, 4 * 48000) / energy(input, 3 * 48000, 4 * 48000));
    CHECK(after < -10.0);
    // No jump at either switch: the steps around them are no bigger than the noise's own.
    float largest_input_step = 0.0f;
    for (std::size_t i = 1; i < input.size(); ++i) {
        largest_input_step = std::max(largest_input_step, std::abs(input[i] - input[i - 1]));
    }
    for (const std::size_t at : {std::size_t{2 * 48000}, std::size_t{3 * 48000}}) {
        float largest = 0.0f;
        for (std::size_t i = at - 480; i < at + 960; ++i) {
            largest = std::max(largest, std::abs(output[i] - output[i - 1]));
        }
        CHECK(largest <= largest_input_step);
    }
}

void clear_audio_forgets_old_voice() {
    // After a pause the suppressor must not play what it still held from before it.
    NoiseSuppressor ns(false);
    std::vector<float> loud(4800, 0.5f);
    ns.process(loud);
    ns.clear_audio();
    std::vector<float> quiet(2048, 0.0f);
    ns.process(quiet);
    bool zeros = true;
    for (const float v : quiet) {
        zeros = zeros && v == 0.0f;
    }
    CHECK(zeros);
}

}  // namespace

int main() {
    fft_round_trip_and_known_values();
    off_is_an_exact_delay();
    steady_noise_is_reduced();
    speech_is_kept();
    key_clicks_between_words_are_softened();
    survives_silence_full_scale_and_garbage();
    switching_crossfades();
    clear_audio_forgets_old_voice();
    return bt_test::finish("NoiseSuppressorTests");
}
