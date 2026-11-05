#pragma once

// Records the microphone (WASAPI shared-mode capture), resamples it to 48 kHz, takes out noise
// and hands it to AudioCapture through an AudioFifo, which mixes it into the one sound track.
//
// Threads:
//   - Constructor, start, stop, destructor: the owner's thread (AudioCapture's owner).
//   - start() spawns the microphone thread (COM MTA). It alone touches WASAPI, the resampler and
//     the noise suppressor, and it is the FIFO's only producer.
//   - Device notifications arrive on a Windows audio thread and only set a flag.
//   - status(), device_name(), set_noise_suppression(): any thread.
//
// Devices: an empty device id follows the Windows default microphone (the "console" role, what
// Sound settings calls the default device), also when it changes. A chosen device that does not
// exist (unplugged, another PC's settings) is replaced by the default and logged; when it shows
// up again it is used again. When Windows' privacy settings deny microphone access the status is
// Blocked and it is tried again every few seconds (the setting can be changed while Backtrack
// runs), not in a tight loop.

#include "core/AudioFifo.h"
#include "core/NoiseSuppressor.h"
#include "platform/Com.h"
#include "platform/Log.h"

#include <audioclient.h>
#include <mftransform.h>
#include <mmdeviceapi.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bt {

// Blocked: Windows' privacy settings (Settings > Privacy & security > Microphone) deny access.
// OnlyBluetooth: the only microphone is a Bluetooth headset's, which is not used unless chosen by
// name (opening it drops the headset's sound to call quality for as long as Backtrack records).
enum class MicStatus { Off, Starting, Recording, NoDevice, Blocked, Failed, OnlyBluetooth };

struct MicDevice {
    std::wstring id;  // the WASAPI endpoint id, what Settings::mic_device holds
    std::string name;  // friendly name, UTF-8 ("Microphone (USB Audio Device)")
    bool is_default = false;
    bool bluetooth = false;  // a Bluetooth headset's microphone
};

// Active capture devices, the Windows default first. Empty when there are none or the audio
// system cannot be reached. Any thread; initialises COM for itself when the thread has not.
std::vector<MicDevice> list_microphones();

struct MicStats {
    std::uint64_t packets = 0;
    std::uint64_t frames_out = 0;  // 48 kHz frames pushed into the FIFO
    std::uint64_t device_opens = 0;
};

class MicCapture {
public:
    // device_id empty: the Windows default microphone. `fifo` must outlive this object.
    MicCapture(std::wstring device_id, bool noise_suppression, AudioFifo& fifo);
    ~MicCapture();
    MicCapture(const MicCapture&) = delete;
    MicCapture& operator=(const MicCapture&) = delete;

    void start();
    void stop();

    void set_noise_suppression(bool on) { noise_suppression_ = on; }
    MicStatus status() const { return status_; }
    // The chosen microphone is not connected, so the Windows default is being recorded instead.
    bool fallback() const { return fallback_; }
    std::string device_name() const;  // empty until a device has been opened
    MicStats stats() const;

private:
    class DeviceNotifier;

    void thread_main();
    void run_loop();
    void open_device(IMMDeviceEnumerator* enumerator);
    void close_device();
    void read_packets();
    void resample(const BYTE* data, UINT32 frames);  // data null: silence
    void drain_resampler();
    void deliver(const BYTE* interleaved, std::size_t frames);  // 48 kHz float, channels_ per frame

    const std::wstring wanted_id_;
    AudioFifo& fifo_;
    std::atomic<bool> noise_suppression_;
    std::atomic<MicStatus> status_{MicStatus::Off};
    std::atomic<bool> fallback_{false};
    mutable std::mutex name_mutex_;
    std::string device_name_;

    // Microphone thread state.
    ComPtr<IAudioClient> audio_client_;
    ComPtr<IAudioCaptureClient> capture_client_;
    ComPtr<IMFTransform> resampler_;
    DWORD resampler_output_size_ = 0;
    UINT32 block_align_ = 0;
    UINT32 device_rate_ = 0;
    UINT32 channels_ = 0;  // of the resampler's float output, the same as the device's
    std::wstring device_id_;  // the device open now
    std::wstring last_device_id_;  // the device opened last; kept when it closes, preferred on reopen
    bool open_ = false;
    ULONGLONG opened_at_ms_ = 0;
    LONGLONG next_sample_time_ = 0;
    std::unique_ptr<NoiseSuppressor> suppressor_;
    static constexpr UINT32 kMaxWeighedChannels = 32;  // more than that: plain average of all
    std::array<float, kMaxWeighedChannels> channel_power_{};
    std::vector<float> mono_;  // one packet's 48 kHz mono samples, grows to the largest seen
    log::RateLimit error_log_{std::chrono::seconds(30)};
    bool warned_bluetooth_skipped_ = false;  // "not using the Bluetooth mic, recording X instead"
    bool warned_only_bluetooth_ = false;  // "the only microphone is a Bluetooth headset's"
    bool warned_missing_ = false;
    bool warned_blocked_ = false;

    UniqueHandle stop_event_;
    UniqueHandle wake_event_;
    std::atomic<bool> devices_changed_{false};
    std::atomic<bool> running_{false};  // false once the thread has ended, also on its own
    std::thread thread_;

    struct Counters {
        std::atomic<std::uint64_t> packets{0}, frames_out{0}, device_opens{0};
    } counters_;
};

}  // namespace bt
