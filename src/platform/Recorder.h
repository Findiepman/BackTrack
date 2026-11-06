#pragma once

// The whole recording pipeline behind one object (§6): screen capture, video encoder, sound
// capture, both ring buffers and the clip writer. This is the engine the UI drives.
//
// Threads and what they own:
//   - Owner thread (the UI): constructor, start, stop, pause, resume, save_clip, status_text.
//     One thread only; these are not meant to be called concurrently.
//   - Supervisor thread: creates the ScreenCapture and the VideoEncoder, replaces the encoder when
//     the frame size changes or it breaks, rebuilds the video pipeline after a fatal capture error
//     (graphics device lost, monitor on another GPU, no monitor at startup). Logs counters once a
//     minute (§9). Creating an encoder takes ~250 ms, so it never happens on the capture thread,
//     and tearing one down can take seconds, so it never happens while pipeline_mutex_ is held.
//   - Capture thread (ScreenCapture): hands frames to the encoder under encoder_mutex_ and never
//     waits for the supervisor; frames arriving while there is no encoder are counted and dropped.
//   - Encoder threads: packets into the video buffer under stream_mutex_.
//   - Audio thread (AudioCapture): packets into the audio buffer under stream_mutex_. It mixes in
//     the microphone, which MicCapture records on a thread of its own.
//   - Writer thread: writes saved clips one after another, then calls SavedCallback (§3.8).
//
// Locks: pipeline_mutex_ may be held while taking encoder_mutex_ or stream_mutex_; nothing ever
// waits for pipeline_mutex_ while holding another lock. The capture, encoder and audio callbacks
// never take pipeline_mutex_, so it can be held while those threads are stopped.
//
// Shutdown (stop): supervisor, capture thread, encoder, audio thread, then the writer finishes
// every queued clip (§6: a clip is never left half written). MFShutdown only after that.
//
// Timeline: every timestamp is on the recording clock = QPC minus the total time spent paused, so
// a clip that spans a pause plays on without a frozen gap.

#include "core/ClipBuffer.h"
#include "core/Config.h"
#include "core/Hotkey.h"
#include "platform/AudioCapture.h"
#include "platform/MicCapture.h"
#include "platform/ClipWriter.h"
#include "platform/ScreenCapture.h"
#include "platform/VideoEncoder.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace bt {

struct SaveResult {
    bool ok = false;
    std::filesystem::path path;
    std::string message;  // one line for the user: what was saved, or why not
    double seconds = 0.0;
    Time100ns start = 0;  // the clip's first frame on the recording clock (see recording_time)
};

// The sound meters and the microphone's state for the UI. MicStatus is in platform/MicCapture.h
// (Off, Starting, Recording, NoDevice, Blocked = Windows privacy settings, Failed), and so is
// list_microphones() for the device list.
struct AudioLevels {
    float system = 0;  // peaks 0..1 after the volume
    float mic = 0;  // after the volume and the gate
    MicStatus mic_status = MicStatus::Off;
    std::string mic_name;  // the microphone being recorded; empty before one was opened
    bool mic_gate_open = false;  // not muted and (push-to-talk off or its key held)
    bool mic_fallback = false;  // the chosen microphone is not connected: the Windows default is recorded
    // Push-to-talk is on and closed while an admin (elevated) program is in front and Backtrack is
    // not elevated: Windows may hide the key from Backtrack then.
    bool ptt_blocked = false;
};

// What the Record page shows about the replay buffer.
struct BufferStatus {
    double seconds = 0.0;  // how much of the past the buffer holds now
    std::uint64_t bytes = 0;  // video and sound together
    bool hardware_encoder = false;
    bool encoder_running = false;
};

class Recorder {
public:
    using SavedCallback = std::function<void(const SaveResult& result)>;  // called on the writer thread

    // app_dir: the exe's folder; a relative output_dir is taken from there (§8).
    Recorder(const Settings& settings, const std::filesystem::path& app_dir, SavedCallback on_saved);
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    void start();
    // Stops recording and waits until every queued clip is written.
    void stop();

    // Paused: nothing is captured, the buffers keep what they have and can still be saved.
    void pause();
    void resume();
    bool paused() const { return paused_; }

    // Now on the recording clock (QPC minus the time spent paused), the clock SaveResult::start is on.
    Time100ns recording_time() const { return recording_now(); }

    // §3.8: takes the last `seconds` out of the buffers (shared bytes, microseconds) and queues
    // them for the writer thread; returns at once. False with a reason when nothing can be saved.
    // 0 or more than the buffer holds: all of it. Passed in rather than part of Settings because
    // the save length changes live, without a new pipeline (that would empty the buffer).
    bool save_clip(std::string& reason, int seconds = 0);

    // "Recording 30 s, 60 fps, hardware", "Capture lost, retrying", "Saving clip...", ...
    std::string status_text() const;
    bool saving() const { return saves_pending_ > 0; }
    BufferStatus buffer_status();
    const std::filesystem::path& output_dir() const { return output_dir_; }
    // The configured clips folder could not be used and clips go to the Videos folder this run.
    bool output_dir_is_fallback() const { return output_dir_fallback_; }

    // Sound. All live (no restart), from any thread. The initial values come from Settings;
    // changing mic_enabled or mic_device needs a new Recorder. The mute is not saved anywhere.
    AudioLevels audio_levels() const;
    void set_volumes(int system_percent, int mic_percent);
    void set_noise_suppression(bool on);
    void set_mic_muted(bool muted);
    bool mic_muted() const;
    void set_push_to_talk(bool on, Hotkey key);

    // Test hook: behave as if Windows took the screen away (UAC prompt).
    void simulate_capture_loss();

private:
    struct SaveJob {
        std::chrono::system_clock::time_point requested;
        Time100ns start = 0;
        ComPtr<IMFMediaType> video_type;
        ComPtr<IMFMediaType> audio_type;
        std::vector<Packet> video;
        std::vector<Packet> audio;
    };

    // Owner thread / supervisor.
    Time100ns recording_now() const;
    void supervisor_main();
    void rebuild_pipeline();
    void ensure_encoder();
    void log_counters();
    void wake_supervisor();

    // Capture thread.
    void on_frame(IMFSample* sample, Time100ns pts);
    void on_capture_event(CaptureEvent event);

    // Encoder and audio threads.
    void on_video_packet(std::uint64_t generation, Packet&& packet, IMFMediaType* type);
    void on_audio_packet(Packet&& packet);

    // Writer thread.
    void writer_main();
    std::filesystem::path next_clip_path(std::chrono::system_clock::time_point when);

    const Settings settings_;
    SavedCallback on_saved_;
    bool output_dir_fallback_ = false;  // before output_dir_: set while that is initialised
    std::filesystem::path output_dir_;

    // Pipeline. Changed under pipeline_mutex_. capture_ is also read without it by the capture
    // thread (on_frame) and the supervisor: only the supervisor replaces it, after stopping the
    // capture thread, and stop() only after the supervisor has ended.
    mutable std::mutex pipeline_mutex_;
    std::unique_ptr<ScreenCapture> capture_;
    std::unique_ptr<AudioCapture> audio_;  // set in the constructor, never replaced
    bool started_ = false;
    std::atomic<bool> paused_{false};
    Time100ns pause_started_ = 0;  // QPC; written by the owner thread only
    Time100ns paused_total_ = 0;  // written by the owner thread under pipeline_mutex_
    Time100ns next_pipeline_attempt_ = 0;  // supervisor only
    Time100ns pipeline_retry_delay_ = 0;  // supervisor only

    // Encoder: capture thread and supervisor, under encoder_mutex_.
    mutable std::mutex encoder_mutex_;
    std::unique_ptr<VideoEncoder> encoder_;
    EncoderPreference encoder_preference_;  // supervisor only
    Time100ns next_encoder_attempt_ = 0;  // supervisor only
    std::vector<Time100ns> device_resets_;  // supervisor only: recent graphics device resets
    std::atomic<bool> encoder_unavailable_{false};
    log::RateLimit frame_error_log_{std::chrono::seconds(10)};  // capture thread only

    // Streams: encoder and audio threads, owner (save), supervisor, under stream_mutex_.
    std::mutex stream_mutex_;
    ClipBuffer video_buffer_;
    ClipBuffer audio_buffer_;
    ComPtr<IMFMediaType> video_type_;
    ComPtr<IMFMediaType> audio_type_;
    std::uint64_t video_generation_ = 0;  // packets from an older encoder are ignored

    // Supervisor.
    std::thread supervisor_;
    std::mutex wake_mutex_;
    std::condition_variable wake_;
    bool stop_supervisor_ = false;
    bool wake_pending_ = false;
    std::atomic<bool> rebuild_pipeline_{true};
    std::atomic<bool> rebuild_encoder_{false};
    std::atomic<bool> capture_lost_{false};
    std::atomic<bool> have_frames_{false};  // the current capture has delivered its first frame
    std::atomic<bool> pipeline_up_{false};
    std::atomic<bool> supervisor_dead_{false};
    Time100ns next_counter_log_ = 0;

    // Writer.
    std::thread writer_;
    std::mutex writer_mutex_;
    std::condition_variable writer_wake_;
    std::deque<SaveJob> jobs_;
    bool stop_writer_ = false;
    std::atomic<int> saves_pending_{0};
    std::atomic<bool> writer_dead_{false};

    std::atomic<std::uint64_t> frames_without_encoder_{0};
};

}  // namespace bt
