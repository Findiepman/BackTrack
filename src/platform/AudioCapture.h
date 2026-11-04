#pragma once

// Records what the default playback device plays (WASAPI loopback) and encodes it to AAC (§3.3, §6).
//
// Threads:
//   - Constructor and destructor: the owner's thread, after MFStartup. The AAC encoder lives as
//     long as this object, so the audio timeline stays continuous across pauses and device changes.
//   - start() spawns the audio thread. It alone touches WASAPI, the resampler and the AAC encoder,
//     and it calls PacketCallback.
//   - The default-device notification arrives on a Windows audio thread and only sets a flag.
//
// Timing (§3.3): packets are stamped on the owner's recording clock (QPC minus clock_offset) at the
// moment they were mixed, which is when the game made the sound and showed the matching frame (the
// QPC time WASAPI reports is when the device will play it, up to ~170 ms later on Bluetooth). The
// timeline advances by the number of samples and is checked against that time for every packet: a
// gap (nothing playing, a discontinuity, a device switch, a pause) is filled with silence, running
// ahead (the sound card's clock is faster than QPC) trims samples. Audio and video therefore stay
// within kDriftTolerance of each other for as long as the program runs.

#include "core/ClipBuffer.h"
#include "core/WindowedMinimum.h"
#include "platform/Com.h"
#include "platform/Log.h"

#include <audioclient.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bt {

struct AudioStats {
    std::uint64_t packets_out = 0;
    std::uint64_t silence_inserted_ms = 0;  // gaps filled (nothing playing, device switch, drift)
    std::uint64_t trimmed_ms = 0;  // samples dropped because the device clock ran ahead
    std::uint64_t device_opens = 0;
    std::uint64_t silent_packets = 0;  // AUDCLNT_BUFFERFLAGS_SILENT, turned into real zeros
};

class AudioCapture {
public:
    using PacketCallback = std::function<void(Packet&& packet)>;

    // max_gap_fill: how much of a long gap (the sound device was gone) is filled with silence:
    // as much as the owner keeps in its buffer, so a clip never contains a hole in the sound.
    AudioCapture(int bitrate_kbps, Time100ns max_gap_fill, PacketCallback on_packet);
    ~AudioCapture();
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    // clock_offset: subtracted from every QPC time, see ScreenCapture::start.
    void start(Time100ns clock_offset);
    void stop();

    // The AAC output type, with MF_MT_USER_DATA (AudioSpecificConfig) for the MP4 writer (§7).
    ComPtr<IMFMediaType> output_type() const { return output_type_; }
    AudioStats stats() const;

    // True when the audio thread could not reach the Windows audio system at all and ended.
    bool failed() const { return failed_; }

private:
    class DeviceNotifier;

    void thread_main();
    void run_loop();
    void open_device(IMMDeviceEnumerator* enumerator);
    void close_device();
    void keep_render_alive();
    void read_packets();
    void handle_packet(const BYTE* data, UINT32 frames, DWORD flags, UINT64 qpc_position, Time100ns arrival);
    void encode_pcm(const BYTE* data, UINT32 frames);  // data null: silence
    void drain_resampler();
    void encode_aac(IMFSample* pcm);
    void drain_aac();
    Time100ns timeline() const;

    PacketCallback on_packet_;
    const Time100ns max_gap_fill_;
    ComPtr<IMFTransform> aac_;
    ComPtr<IMFMediaType> output_type_;
    DWORD aac_output_size_ = 0;

    // Audio thread state.
    ComPtr<IAudioClient> capture_client_audio_;
    ComPtr<IAudioCaptureClient> capture_client_;
    ComPtr<IAudioClient> render_client_audio_;
    ComPtr<IAudioRenderClient> render_client_;
    UINT32 render_buffer_frames_ = 0;
    ComPtr<IMFTransform> resampler_;
    DWORD resampler_output_size_ = 0;
    UINT32 device_rate_ = 0;
    UINT32 block_align_ = 0;
    std::wstring device_id_;
    bool open_ = false;
    bool anchored_ = false;
    Time100ns anchor_ = 0;  // recording-clock time of the first sample ever
    Time100ns segment_base_ = 0;  // timeline offset where the current device format began
    std::uint64_t segment_frames_ = 0;  // frames fed since then, at device_rate_
    Time100ns clock_offset_ = 0;
    // Two seconds of packets: long enough that at least one was read right after it was mixed.
    WindowedMinimum device_latency_{2 * kTicksPerSecond};
    log::RateLimit error_log_{std::chrono::seconds(30)};
    log::RateLimit reopen_log_{std::chrono::seconds(30)};
    std::vector<BYTE> packet_copy_;
    Time100ns aac_offset_ = 0;  // correction applied to AAC stamps (stays 0 unless a flush needed it)
    Time100ns aac_expected_after_flush_ = 0;
    bool check_aac_after_flush_ = false;

    UniqueHandle stop_event_;
    UniqueHandle wake_event_;
    std::atomic<bool> device_changed_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    std::thread thread_;

    struct Counters {
        std::atomic<std::uint64_t> packets_out{0}, silence_inserted_ms{0}, trimmed_ms{0}, device_opens{0},
            silent_packets{0};
    } counters_;
};

}  // namespace bt
