#pragma once

#include "core/ClipBuffer.h"
#include "core/Config.h"
#include "linux/AudioRecorder.h"
#include "linux/FrameSource.h"
#include "linux/VideoEncoder.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace bt::linux_app {

struct SaveResult {
    bool ok = false;
    std::string path;  // the saved clip
    std::string error;
    double seconds = 0.0;  // the clip's length
};

struct RecorderStatus {
    bool running = false;
    std::string capture;  // "Display 1 (X11, 1920x1080)"
    std::string encoder;  // "h264_vaapi (graphics card)"
    std::string audio;  // "system sound + microphone"
    std::string error;  // why recording stopped, or a warning (sound missing)
};

// The Linux replay buffer: a video thread that grabs the screen at the frame rate and encodes it,
// the sound recorder, and the two core::ClipBuffers that keep the last clip_seconds. save() cuts
// a clip out of the buffers and writes it, while recording goes on.
class Recorder {
public:
    Recorder();
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // `source` is already started. Returns at once; the first frames arrive on the video thread.
    void start(const Settings& settings, std::unique_ptr<FrameSource> source);
    void stop();

    // Blocks while the clip is written (call it off the GUI thread). Saves are one at a time.
    SaveResult save(const std::string& folder, const std::string& filename_pattern, int seconds);

    RecorderStatus status() const;

private:
    void video_loop();
    void set_error(std::string message);

    Settings settings_;
    std::unique_ptr<FrameSource> source_;
    std::unique_ptr<ClipBuffer> video_;
    std::unique_ptr<ClipBuffer> audio_;
    AudioRecorder audio_recorder_;
    bool audio_ok_ = false;
    std::thread video_thread_;
    std::atomic<bool> running_{false};
    std::mutex save_mutex_;  // the buffers are not replaced while a save copies out of them
    std::mutex write_mutex_;  // one save at a time

    mutable std::mutex mutex_;  // guards the fields below
    CodecParameters video_parameters_;
    std::uint64_t video_generation_ = 0;  // +1 each time the encoder (re)opens and the buffer is cleared
    std::string encoder_name_;
    std::string error_;
    bool video_running_ = false;
};

}  // namespace bt::linux_app
