#pragma once

#include "core/ClipBuffer.h"
#include "linux/VideoEncoder.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace bt {
class AudioFifo;
}

namespace bt::linux_app {

struct AudioConfig {
    bool system = true;  // what you hear: the default output's monitor
    int system_volume = 100;  // percent
    bool mic = true;  // the default microphone
    int mic_volume = 100;
    bool noise_suppression = true;
    int bitrate_kbps = 160;
};

// Sound through the PulseAudio API (KDE Plasma and Arch run PipeWire with its PulseAudio server,
// pipewire-pulse): the default output's monitor (game, voice chat, music) and the default
// microphone, mixed into one AAC track (core/AudioMix, the same mix as on Windows).
//
// The monitor is the clock: it runs even when nothing plays (silence), so the track has no gaps.
// The microphone comes through core/AudioFifo, which evens out the two devices' clocks. With the
// system sound switched off the monitor still runs as the clock, at volume 0.
class AudioRecorder {
public:
    AudioRecorder();
    ~AudioRecorder();
    AudioRecorder(const AudioRecorder&) = delete;
    AudioRecorder& operator=(const AudioRecorder&) = delete;

    // Starts recording into `out`. False (and nothing running) when there is no sound server or
    // the AAC encoder cannot open; the recorder then records video only.
    bool start(const AudioConfig& config, ClipBuffer& out, std::string& error);
    void stop();

    CodecParameters parameters() const;
    std::string description() const;
    // Set when the sound stopped while recording (the sound server went away).
    std::string error() const;

private:
    void system_loop();
    void mic_loop();
    void set_error(std::string message);

    AudioConfig config_;
    ClipBuffer* out_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> mic_running_{false};
    std::thread system_thread_;
    std::thread mic_thread_;
    std::unique_ptr<AudioFifo> mic_fifo_;
    struct Streams;  // the pa_simple handles and the AAC encoder, in the .cpp
    std::unique_ptr<Streams> streams_;
    mutable std::mutex mutex_;  // guards parameters_, error_, mic_ok_
    CodecParameters parameters_;
    std::string error_;
    bool mic_ok_ = false;
};

}  // namespace bt::linux_app
