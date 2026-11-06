#pragma once

// Records what a playback device plays (WASAPI loopback) and encodes it to AAC (§3.3, §6).
//
// Threads:
//   - Constructor and destructor: the owner's thread, after MFStartup. The AAC encoder lives as
//     long as this object, so the audio timeline stays continuous across pauses and device changes.
//   - start() spawns the audio thread. It alone touches WASAPI, the resampler and the AAC encoder,
//     and it calls PacketCallback.
//   - Device notifications arrive on a Windows audio thread and only set a flag.
//
// Device: an empty system_device follows the Windows default playback device, also when it
// changes. A chosen device that is not there (unplugged, another PC's settings) is replaced by the
// default and system_fallback() says so; when it shows up again it is used again (as MicCapture
// does for microphones). The silent keep-alive stream always plays on the device being recorded.
//
// Timing (§3.3): packets are stamped on the owner's recording clock (QPC minus clock_offset) at the
// moment they were mixed, which is when the game made the sound and showed the matching frame (the
// QPC time WASAPI reports is when the device will play it, up to ~170 ms later on Bluetooth). The
// timeline advances by the number of samples and is checked against that time for every packet: a
// gap (nothing playing, a discontinuity, a device switch, a pause) is filled with silence, running
// ahead (the sound card's clock is faster than QPC) trims samples. Audio and video therefore stay
// within kDriftTolerance of each other for as long as the program runs.
//
// Microphone: MicCapture records it on its own thread into an AudioFifo. Every block the
// resampler hands to the AAC encoder (48 kHz 16-bit stereo, already on the timeline) takes exactly
// as many microphone frames out of the FIFO and mixes them in, so the stamps above stay exactly as
// they are and the loopback stream is the one clock. That is why the loopback runs even when the
// game sound is not recorded (system gain 0). The FIFO keeps the voice a steady few tens of
// milliseconds behind the moment it is mixed however far the two sound cards' clocks drift apart.
// With no microphone and the game sound at 100 % the samples are not changed at all.
//
// Separate tracks (with both the game sound and the microphone recorded): the same blocks also go,
// split into their two parts by the same gains (AudioMix's mix_block), through two more AAC
// encoders: track 1 is the system sound alone, track 2 the microphone alone, track 0 stays the
// mix. All three get the same blocks with the same stamps, so they stay sample for sample in step.

#include "core/AudioFifo.h"
#include "core/AudioMix.h"
#include "core/ClipBuffer.h"
#include "core/Hotkey.h"
#include "core/WindowedMinimum.h"
#include "platform/Com.h"
#include "platform/Log.h"
#include "platform/MicCapture.h"

#include <audioclient.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mmdeviceapi.h>

#include <array>
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
    bool mic = false;  // the fields below are only filled in when there is a microphone
    MicStats mic_capture;
    AudioFifoStats mic_fifo;
};

// What goes into the sound track(s) and how loud. Volumes are percent (0..200).
struct AudioMixOptions {
    bool system_sound = true;  // false: what you hear is not recorded, the loopback only keeps time
    std::wstring system_device;  // playback endpoint id; empty: the Windows default playback device
    int system_volume = 100;
    bool mic = false;
    std::wstring mic_device;  // empty: the Windows default microphone
    int mic_volume = 100;
    bool noise_suppression = true;
    bool push_to_talk = false;
    Hotkey push_to_talk_key;  // virtual_key 0: none (with push-to-talk on the microphone then stays closed)
    // Besides the mix, a track with only the system sound and one with only the microphone. Only
    // when both are recorded; otherwise the mix is the one source anyway and stays the only track.
    bool separate_tracks = false;
};

// The sound tracks in the order they go into the clip. With separate tracks off only kMixTrack.
inline constexpr std::size_t kMixTrack = 0;
inline constexpr std::size_t kSystemTrack = 1;
inline constexpr std::size_t kMicTrack = 2;

struct PlaybackDevice {
    std::wstring id;  // the WASAPI endpoint id, what Settings::system_device holds
    std::string name;  // friendly name, UTF-8 ("Speakers (Realtek(R) Audio)")
    bool is_default = false;
};

// Active playback devices, the Windows default first. Empty when there are none or the audio
// system cannot be reached. Any thread; initialises COM for itself when the thread has not.
std::vector<PlaybackDevice> list_playback_devices();

class AudioCapture {
public:
    // track: kMixTrack, kSystemTrack or kMicTrack (less than track_count()).
    using PacketCallback = std::function<void(std::size_t track, Packet&& packet)>;

    // max_gap_fill: how much of a long gap (the sound device was gone) is filled with silence:
    // as much as the owner keeps in its buffer, so a clip never contains a hole in the sound.
    AudioCapture(int bitrate_kbps, Time100ns max_gap_fill, const AudioMixOptions& mix, PacketCallback on_packet);
    ~AudioCapture();
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    // clock_offset: subtracted from every QPC time, see ScreenCapture::start. Also starts and
    // stops the microphone: while paused it is not recorded (and Windows shows it is not in use).
    void start(Time100ns clock_offset);
    void stop();

    // Live, from any thread; the audio thread picks them up with its next block (~10 ms) and ramps
    // to them over 10 ms, so none of them clicks.
    void set_volumes(int system_percent, int mic_percent);
    void set_noise_suppression(bool on);
    void set_mic_muted(bool muted) { mic_muted_ = muted; }
    bool mic_muted() const { return mic_muted_; }
    void set_push_to_talk(bool on, Hotkey key);

    // For the UI meters, any thread: peaks 0..1 after the volume (and the gate for the mic).
    float system_level() const { return system_level_; }
    float mic_level() const { return mic_level_; }
    bool has_mic() const { return mic_ != nullptr; }
    MicStatus mic_status() const { return mic_ ? mic_->status() : MicStatus::Off; }
    std::string mic_name() const { return mic_ ? mic_->device_name() : std::string(); }
    bool mic_gate_open() const { return mic_gate_open_; }
    bool mic_fallback() const { return mic_ && mic_->fallback(); }
    // The playback device being recorded; empty before one was opened.
    std::string system_name() const;
    // The chosen playback device is not connected: the Windows default is recorded instead.
    bool system_fallback() const { return system_fallback_; }
    // Push-to-talk is on, the microphone is closed, and an elevated program is in front while
    // Backtrack is not elevated: Windows may hide the key from Backtrack then.
    bool push_to_talk_blocked() const { return push_to_talk_blocked_; }

    // 1, or 3 with separate tracks; fixed for this object's life.
    std::size_t track_count() const { return tracks_.size(); }
    // A track's AAC output type, with MF_MT_USER_DATA (AudioSpecificConfig) for the MP4 writer (§7).
    ComPtr<IMFMediaType> output_type(std::size_t track = kMixTrack) const { return tracks_.at(track).output_type; }
    // What players and editors show for the track ("Game + mic", "Game", "Mic").
    static const char* track_name(std::size_t track);
    AudioStats stats() const;

    // True when the audio thread could not reach the Windows audio system at all and ended.
    bool failed() const { return failed_; }

    // True while sound is actually being captured (a device is open); false while there is no
    // device, while one is being reopened and after failed().
    bool recording() const { return open_ && !failed_; }

private:
    class DeviceNotifier;

    void thread_main();
    void run_loop();
    void open_device(IMMDeviceEnumerator* enumerator);
    void open_endpoint(IMMDevice* device, bool fallback);
    void close_device();
    void keep_render_alive();
    void drop_render_stream();
    void add_latency_sample(Time100ns at, Time100ns value);
    void read_packets();
    void handle_packet(const BYTE* data, UINT32 frames, DWORD flags, UINT64 qpc_position, Time100ns arrival);
    void encode_pcm(const BYTE* data, UINT32 frames);  // data null: silence
    void fill_gap(UINT32 frames);  // silence for a gap in the timeline
    void keep_mic_flowing();
    void drain_resampler();
    void mix(IMFSample* pcm);  // the microphone and the volumes, in place, before encode_aac
    // Separate tracks: the system and microphone parts mix() left in mix_system_ / mix_voice_, as
    // samples with the mixed block's time; both null without separate tracks.
    std::array<ComPtr<IMFSample>, 2> part_samples(IMFSample* mixed);
    bool push_to_talk_open();
    bool foreground_elevated();
    void encode_aac(std::size_t track, IMFSample* pcm);
    void drain_aac(std::size_t track);
    void flush_aac();  // every track's encoder, before the timeline jumps
    Time100ns timeline() const;

    // One AAC encoder per track. Each lives as long as this object (see Threads above).
    struct AacTrack {
        ComPtr<IMFTransform> encoder;
        ComPtr<IMFMediaType> output_type;
        DWORD output_size = 0;
        Time100ns offset = 0;  // correction applied to its stamps (stays 0 unless a flush needed it)
        bool check_after_flush = false;
    };

    PacketCallback on_packet_;
    const Time100ns max_gap_fill_;
    std::vector<AacTrack> tracks_;

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
    const std::wstring wanted_device_;  // empty: the Windows default
    std::wstring device_id_;  // the device open now (or last)
    std::atomic<bool> system_fallback_{false};
    mutable std::mutex name_mutex_;
    std::string system_name_;
    bool warned_missing_ = false;
    std::wstring unusable_device_;  // the chosen device: connected, but opening it failed
    ULONGLONG unusable_until_ = 0;  // ... so it is not tried again before this (GetTickCount64)
    std::atomic<bool> open_{false};  // written by the audio thread, read by recording()
    ULONGLONG opened_at_ms_ = 0;
    bool anchored_ = false;
    Time100ns anchor_ = 0;  // recording-clock time of the first sample ever
    Time100ns segment_base_ = 0;  // timeline offset where the current device format began
    std::uint64_t segment_frames_ = 0;  // frames fed since then, at device_rate_
    Time100ns clock_offset_ = 0;
    // Two seconds of packets: long enough that at least one was read right after it was mixed.
    WindowedMinimum device_latency_{2 * kTicksPerSecond};

    struct BatchPacket {
        std::vector<BYTE> bytes;  // reused between polls; empty for a silent packet
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 qpc_position = 0;
        std::uint64_t frames_after = 0;
    };
    std::vector<BatchPacket> batch_;  // grows to the most packets one poll has found, then stays
    log::RateLimit error_log_{std::chrono::seconds(30)};
    log::RateLimit reopen_log_{std::chrono::seconds(30)};
    Time100ns aac_expected_after_flush_ = 0;  // where the timeline was at the last flush
    bool tracks_out_of_step_ = false;  // a block reached some tracks' encoders but not all

    UniqueHandle stop_event_;
    UniqueHandle wake_event_;
    std::atomic<bool> device_changed_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    std::thread thread_;

    // Mix. The options and the meters are atomics shared with any thread; the rest is the audio
    // thread's. mic_fifo_ is declared before mic_ so it outlives the microphone thread.
    const bool system_sound_;
    std::atomic<int> system_volume_;
    std::atomic<int> mic_volume_;
    std::atomic<bool> mic_muted_{false};
    std::atomic<bool> push_to_talk_;
    std::atomic<std::uint64_t> push_to_talk_key_;  // modifiers << 32 | virtual key
    std::unique_ptr<AudioFifo> mic_fifo_;
    std::unique_ptr<MicCapture> mic_;
    GainRamp system_gain_;
    GainRamp mic_gain_;
    PeakMeter system_meter_;
    PeakMeter mic_meter_;
    std::vector<std::int16_t> mix_pcm_;  // one block; grows to the largest block, then stays
    std::vector<float> mix_mic_;
    std::vector<std::int16_t> mix_system_;  // separate tracks: the block's two parts (else empty)
    std::vector<std::int16_t> mix_voice_;
    Time100ns push_to_talk_released_at_ = 0;
    bool push_to_talk_was_held_ = false;
    const bool self_elevated_;
    DWORD foreground_pid_ = 0;
    bool foreground_elevated_ = false;
    std::atomic<bool> push_to_talk_blocked_{false};
    bool gap_fill_ = false;  // inside fill_gap: the mic FIFO is read with pull_gap
    bool gap_ran_dry_ = false;
    bool logged_quiet_fill_ = false;
    std::atomic<float> system_level_{0.0f};
    std::atomic<float> mic_level_{0.0f};
    std::atomic<bool> mic_gate_open_{false};

    struct Counters {
        std::atomic<std::uint64_t> packets_out{0}, silence_inserted_ms{0}, trimmed_ms{0}, device_opens{0},
            silent_packets{0};
    } counters_;
};

}  // namespace bt
