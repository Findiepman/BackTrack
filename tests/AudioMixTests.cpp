#include "core/AudioMix.h"

#include "Check.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <stdexcept>
#include <vector>

using namespace bt;

namespace {

constexpr std::uint32_t kRamp = 480;

std::vector<std::int16_t> pattern(std::size_t frames) {
    std::vector<std::int16_t> v(2 * frames);
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i] = static_cast<std::int16_t>(static_cast<int>((i * 7919) % 65536) - 32768);
    }
    v[0] = -32768;
    v[1] = 32767;
    return v;
}

void unity_without_mic_is_untouched() {
    std::vector<std::int16_t> samples = pattern(1000);
    const std::vector<std::int16_t> original = samples;
    GainRamp system(1.0f, kRamp);
    GainRamp mic(1.0f, kRamp);
    const MixPeaks peaks = mix_block(samples, {}, system, mic);
    CHECK(samples == original);
    CHECK_EQ(peaks.system, 1.0f);  // -32768 is full scale
    CHECK_EQ(peaks.mic, 0.0f);

    // A microphone whose gain is settled at 0 (muted, or push-to-talk not held) changes nothing either.
    GainRamp muted(0.0f, kRamp);
    const std::vector<float> voice(1000, 0.5f);
    mix_block(samples, voice, system, muted);
    CHECK(samples == original);
}

void system_gain_scales() {
    std::vector<std::int16_t> samples = {1000, -1000, 32767, -32768, 3, -3};
    GainRamp system(0.5f, kRamp);
    GainRamp mic(1.0f, kRamp);
    mix_block(samples, {}, system, mic);
    CHECK_EQ(samples[0], std::int16_t{500});
    CHECK_EQ(samples[1], std::int16_t{-500});
    CHECK_EQ(samples[2], std::int16_t{16384});  // 16383.5 rounds to even
    CHECK_EQ(samples[3], std::int16_t{-16384});
    CHECK_EQ(samples[4], std::int16_t{2});  // 1.5 rounds to even
    GainRamp silent(0.0f, kRamp);
    mix_block(samples, {}, silent, mic);
    for (const std::int16_t s : samples) {
        CHECK_EQ(s, std::int16_t{0});
    }
}

void mic_goes_to_both_channels_and_saturates() {
    std::vector<std::int16_t> samples = {0, 100, 30000, -30000, -30000, 30000};
    const std::vector<float> voice = {0.25f, 0.5f, -0.5f};
    GainRamp system(1.0f, kRamp);
    GainRamp mic(1.0f, kRamp);
    const MixPeaks peaks = mix_block(samples, voice, system, mic);
    CHECK_EQ(samples[0], std::int16_t{8192});
    CHECK_EQ(samples[1], std::int16_t{8292});
    CHECK_EQ(samples[2], std::int16_t{32767});  // 46384: saturated
    CHECK_EQ(samples[3], std::int16_t{-13616});
    CHECK_EQ(samples[4], std::int16_t{-32768});  // -46384: saturated
    CHECK_EQ(samples[5], std::int16_t{13616});
    CHECK(std::abs(peaks.mic - 0.5f) < 1e-6f);

    // Volume 200 % on a loud voice saturates instead of wrapping around.
    std::vector<std::int16_t> quiet = {0, 0};
    const std::vector<float> loud = {0.9f};
    GainRamp doubled(2.0f, kRamp);
    mix_block(quiet, loud, system, doubled);
    CHECK_EQ(quiet[0], std::int16_t{32767});
    // NaN-free and huge values are clamped too.
    std::vector<std::int16_t> huge = {0, 0};
    const std::vector<float> spike = {-1e30f};
    mix_block(huge, spike, system, mic);
    CHECK_EQ(huge[0], std::int16_t{-32768});
}

void gate_ramps_without_a_click() {
    GainRamp gate(0.0f, kRamp);
    CHECK(gate.settled());
    gate.set_target(1.0f);
    CHECK(!gate.settled());
    std::vector<std::int16_t> samples(2 * 1000, 0);
    const std::vector<float> voice(1000, 0.5f);
    GainRamp system(1.0f, kRamp);
    mix_block(samples, voice, system, gate);
    CHECK(gate.settled());
    CHECK_EQ(gate.current(), 1.0f);  // landed exactly
    int largest_step = 0;
    for (std::size_t i = 1; i < 1000; ++i) {
        CHECK(samples[2 * i] >= samples[2 * (i - 1)]);  // rises steadily
        largest_step = std::max(largest_step, samples[2 * i] - samples[2 * (i - 1)]);
    }
    // 0.5 full scale over 480 frames: about 34 per frame; a hard switch would be 16384.
    CHECK(largest_step <= 36);
    CHECK_EQ(samples[2 * 479], std::int16_t{16384});
    CHECK_EQ(samples[2 * 999], std::int16_t{16384});

    // Releasing ramps down and ends exactly at 0.
    gate.set_target(0.0f);
    for (std::uint32_t i = 0; i < kRamp; ++i) {
        gate.next();
    }
    CHECK(gate.settled());
    CHECK_EQ(gate.current(), 0.0f);

    // A new target in the middle of a ramp starts from where it is: no jump.
    GainRamp volume(1.0f, kRamp);
    volume.set_target(0.0f);
    for (int i = 0; i < 240; ++i) {
        volume.next();
    }
    const float middle = volume.current();
    volume.set_target(1.0f);
    CHECK(std::abs(volume.next() - middle) < 0.01f);
}

void separate_parts_match_the_mix() {
    // Separate tracks: the mix must be bit for bit what it is without them, and the two parts the
    // same gains on the same frames, while the gains ramp (mute, push-to-talk, volume) and settled.
    const std::size_t frames = 2000;
    std::vector<float> voice(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        voice[i] = 0.9f * std::sin(static_cast<float>(i) * 0.05f);
    }
    for (int scenario = 0; scenario < 4; ++scenario) {
        const float system_from = scenario == 1 ? 0.3f : 1.0f;
        const float system_to = scenario == 2 ? 1.7f : 1.0f;
        const float mic_from = scenario == 3 ? 1.0f : 0.0f;
        const float mic_to = scenario == 0 ? 0.0f : 1.4f;
        GainRamp system_a(system_from, kRamp);
        GainRamp mic_a(mic_from, kRamp);
        GainRamp system_b(system_from, kRamp);
        GainRamp mic_b(mic_from, kRamp);
        system_a.set_target(system_to);
        mic_a.set_target(mic_to);
        system_b.set_target(system_to);
        mic_b.set_target(mic_to);
        std::vector<std::int16_t> plain = pattern(frames);
        std::vector<std::int16_t> mixed = plain;
        const std::vector<std::int16_t> original = plain;
        std::vector<std::int16_t> system_part(plain.size(), 1);
        std::vector<std::int16_t> mic_part(plain.size(), 1);
        // Two blocks each, so a ramp crosses a block boundary.
        const std::size_t half = frames / 2;
        for (std::size_t block = 0; block < 2; ++block) {
            const std::size_t at = block * half * 2;
            const std::span<const float> v(voice.data() + block * half, half);
            const MixPeaks a = mix_block(std::span(plain).subspan(at, half * 2), v, system_a, mic_a);
            const MixPeaks b = mix_block(std::span(mixed).subspan(at, half * 2), v, system_b, mic_b,
                                         std::span(system_part).subspan(at, half * 2),
                                         std::span(mic_part).subspan(at, half * 2));
            CHECK_EQ(a.system, b.system);
            CHECK_EQ(a.mic, b.mic);
        }
        CHECK(mixed == plain);
        bool system_ok = true;
        bool mic_ok = true;
        bool mic_stereo = true;
        GainRamp system_c(system_from, kRamp);
        GainRamp mic_c(mic_from, kRamp);
        system_c.set_target(system_to);
        mic_c.set_target(mic_to);
        for (std::size_t i = 0; i < frames; ++i) {
            const float gs = system_c.next();
            const float gm = mic_c.next();
            const auto expect = [](float v) {
                return static_cast<std::int16_t>(std::lrintf(std::clamp(v, -32768.0f, 32767.0f)));
            };
            system_ok = system_ok && system_part[2 * i] == expect(static_cast<float>(original[2 * i]) * gs) &&
                        system_part[2 * i + 1] == expect(static_cast<float>(original[2 * i + 1]) * gs);
            mic_ok = mic_ok && mic_part[2 * i] == expect(voice[i] * gm * 32768.0f);
            mic_stereo = mic_stereo && mic_part[2 * i] == mic_part[2 * i + 1];
        }
        CHECK(system_ok);
        CHECK(mic_ok);
        CHECK(mic_stereo);
        if (scenario == 0) {
            // Unity and a closed microphone: the system part is the input itself, the mic part silence.
            CHECK(system_part == original);
            CHECK(std::all_of(mic_part.begin(), mic_part.end(), [](std::int16_t s) { return s == 0; }));
        }
    }
    // A part of the wrong size is refused rather than written past its end.
    std::vector<std::int16_t> samples(20);
    std::vector<std::int16_t> small(10);
    GainRamp system(1.0f, kRamp);
    GainRamp mic(1.0f, kRamp);
    bool threw = false;
    try {
        mix_block(samples, {}, system, mic, small, {});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

void peak_meter_decays() {
    PeakMeter meter(4800.0f);
    CHECK_EQ(meter.level(), 0.0f);
    meter.add(0.8f, 480);
    CHECK_EQ(meter.level(), 0.8f);
    meter.add(0.1f, 4800);  // one half-life later the old peak is at 0.4, above the new block
    CHECK(std::abs(meter.level() - 0.4f) < 1e-4f);
    meter.add(0.9f, 10);
    CHECK_EQ(meter.level(), 0.9f);
    meter.add(2.0f, 10);
    CHECK_EQ(meter.level(), 1.0f);  // clamped to full scale
    for (int i = 0; i < 1000; ++i) {
        meter.add(0.0f, 4800);
    }
    CHECK_EQ(meter.level(), 0.0f);  // reaches exactly zero
}

}  // namespace

int main() {
    unity_without_mic_is_untouched();
    system_gain_scales();
    mic_goes_to_both_channels_and_saturates();
    gate_ramps_without_a_click();
    separate_parts_match_the_mix();
    peak_meter_decays();
    return bt_test::finish("AudioMixTests");
}
