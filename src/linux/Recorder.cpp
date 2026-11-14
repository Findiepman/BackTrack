#include "linux/Recorder.h"

#include "core/ClipPlan.h"
#include "linux/ClipFile.h"
#include "linux/Log.h"
#include "linux/MonotonicClock.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <format>
#include <system_error>

namespace bt::linux_app {

namespace {

// Encoder failures in a row before recording stops instead of trying again.
constexpr int kMaxEncoderFailures = 3;

void sleep_until(Time100ns target) {
    const Time100ns wait = target - now_100ns();
    if (wait > 0) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(wait * 100));
    }
}

}  // namespace

Recorder::Recorder() = default;

Recorder::~Recorder() {
    stop();
}

void Recorder::start(const Settings& settings, std::unique_ptr<FrameSource> source) {
    stop();
    // A save on its worker thread reads the buffers: never replace them under it.
    std::lock_guard save_lock(save_mutex_);
    settings_ = settings;
    source_ = std::move(source);
    const std::int64_t seconds = settings.clip_seconds + 2;
    // Twice the bitrate as a safety net for a burst; the keep time does the real work (§3.1).
    const std::size_t video_cap = static_cast<std::size_t>(settings.video_bitrate_kbps) * 1000 / 8 * seconds * 2 + (64u << 20);
    video_ = std::make_unique<ClipBuffer>(seconds_to_100ns(settings.clip_seconds), video_cap);
    // Sound keeps a little longer: the video reaches back to the keyframe before the clip's start
    // (up to a second), and the clip should have sound from its first frame.
    audio_ = std::make_unique<ClipBuffer>(seconds_to_100ns(seconds), (std::size_t{1} << 20) * seconds);
    {
        std::lock_guard lock(mutex_);
        error_.clear();
        encoder_name_.clear();
        video_parameters_.reset();
        video_running_ = true;
    }

    AudioConfig audio;
    audio.system = settings.audio_enabled;
    audio.system_volume = settings.system_volume;
    audio.mic = settings.mic_enabled;
    audio.mic_volume = settings.mic_volume;
    audio.noise_suppression = settings.noise_suppression;
    audio.bitrate_kbps = settings.audio_bitrate_kbps;
    std::string audio_error;
    audio_ok_ = audio_recorder_.start(audio, *audio_, audio_error);
    if (!audio_ok_ && (settings.audio_enabled || settings.mic_enabled)) {
        log_warning("Recording without sound: " + audio_error);
        set_error("No sound: " + audio_error);
    }

    running_ = true;
    video_thread_ = std::thread([this] { video_loop(); });
}

void Recorder::stop() {
    std::lock_guard save_lock(save_mutex_);
    running_ = false;
    if (video_thread_.joinable()) {
        video_thread_.join();
    }
    audio_recorder_.stop();
    audio_ok_ = false;
    if (source_) {
        source_->stop();
        source_.reset();
    }
    std::lock_guard lock(mutex_);
    video_running_ = false;
}

void Recorder::set_error(std::string message) {
    std::lock_guard lock(mutex_);
    error_ = std::move(message);
}

void Recorder::video_loop() {
    VideoEncoder encoder;
    const int fps = settings_.fps;
    const Time100ns start = now_100ns();
    std::int64_t tick = 0;
    int failures = 0;
    std::vector<Packet> packets;
    std::string error;

    while (running_) {
        const Time100ns due = start + frame_offset(tick, fps);
        sleep_until(due);
        if (!running_) {
            break;
        }
        FrameView frame;
        const GrabResult grabbed = source_->grab(frame);
        if (grabbed == GrabResult::Failed) {
            set_error(source_->error());
            break;
        }
        if (grabbed == GrabResult::Frame) {
            const VideoEncoderConfig& current = encoder.config();
            if (!encoder.is_open() || frame.width != current.source_width || frame.height != current.source_height ||
                frame.layout != current.layout) {
                VideoEncoderConfig config;
                config.source_width = frame.width;
                config.source_height = frame.height;
                config.layout = frame.layout;
                config.fps = fps;
                config.bitrate_kbps = settings_.video_bitrate_kbps;
                config.resolution = settings_.resolution;
                config.preference = settings_.encoder;
                config.quality = settings_.quality;
                if (!encoder.open(config, error)) {
                    set_error(error);
                    break;
                }
                // Old packets carry the old SPS/PPS: they cannot share a file with the new ones.
                video_->clear();
                std::lock_guard lock(mutex_);
                video_parameters_ = encoder.parameters();
                ++video_generation_;
                encoder_name_ = std::format("{} ({}, {}x{})", encoder.name(), encoder.hardware() ? "graphics card" : "processor",
                                            encoder.width(), encoder.height());
            }
            packets.clear();
            if (encoder.encode(frame, due, packets, error)) {
                failures = 0;
                for (Packet& packet : packets) {
                    video_->push(std::move(packet));
                }
            } else {
                log_warning("Video encoder: " + error);
                encoder.close();  // reopened with the next frame
                if (++failures >= kMaxEncoderFailures) {
                    set_error("The video encoder keeps failing: " + error);
                    break;
                }
            }
        }
        ++tick;
        // Fell behind (a slow encoder, the machine was suspended): skip the missed frames instead
        // of encoding a burst, so the video stays in step with the clock.
        const Time100ns now = now_100ns();
        if (now > start + frame_offset(tick + 1, fps)) {
            tick = (now - start) * fps / kTicksPerSecond + 1;
        }
    }
    std::lock_guard lock(mutex_);
    video_running_ = false;
    if (!error_.empty()) {
        log_error("Recording stopped: " + error_);
    }
}

SaveResult Recorder::save(const std::string& folder, const std::string& pattern, int seconds) {
    std::lock_guard write_lock(write_mutex_);  // one clip at a time
    // The buffers only while copying out of them (pointers, milliseconds): stop(), start() and
    // quitting never wait for the file to be written.
    std::unique_lock buffers_lock(save_mutex_);
    SaveResult result;
    if (!video_) {
        result.error = "Backtrack is not recording";
        return result;
    }
    const Time100ns end = now_100ns();
    const Time100ns begin = end - seconds_to_100ns(seconds);

    CodecParameters video_parameters;
    std::uint64_t generation = 0;
    std::vector<Packet> video;
    // The encoder may reopen between taking its parameters and the packets: then try once more.
    for (int attempt = 0; attempt < 2; ++attempt) {
        {
            std::lock_guard lock(mutex_);
            video_parameters = video_parameters_;
            generation = video_generation_;
        }
        video = video_->extract(begin, end);
        std::lock_guard lock(mutex_);
        if (generation == video_generation_) {
            break;
        }
        video.clear();
    }
    if (video.empty() || !video_parameters) {
        result.error = "Nothing recorded yet";
        return result;
    }
    std::vector<Packet> audio;
    const CodecParameters audio_parameters = audio_ok_ ? audio_recorder_.parameters() : nullptr;
    if (audio_parameters) {
        audio = audio_->extract(video.front().pts, end);
    }
    buffers_lock.unlock();
    const ClipPlan plan = plan_clip(video, audio);
    if (plan.samples.empty()) {
        result.error = "Nothing recorded yet";
        return result;
    }

    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) {
        result.error = "Cannot make the clips folder " + folder + ": " + ec.message();
        return result;
    }
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    const std::string name = expand_clip_name(pattern, local);
    if (name.empty()) {
        result.error = "The file name setting gives an empty name";
        return result;
    }
    std::filesystem::path path = std::filesystem::path(folder) / name;
    for (int number = 2; std::filesystem::exists(path, ec) && number < 1000; ++number) {
        path = std::filesystem::path(folder) / numbered_clip_name(name, number);
    }

    if (!write_clip(path.string(), video_parameters, audio_parameters, video, audio, plan, result.error)) {
        log_error("Saving failed: " + result.error);
        return result;
    }
    result.ok = true;
    result.path = path.string();
    result.seconds = static_cast<double>(plan.duration) / kTicksPerSecond;
    log_info(std::format("Saved {} ({:.1f} s, {} video and {} sound packets)", result.path, result.seconds,
                         plan.video_samples, plan.audio_samples));
    return result;
}

RecorderStatus Recorder::status() const {
    RecorderStatus status;
    std::lock_guard lock(mutex_);
    status.running = video_running_;
    status.capture = source_ ? source_->description() : std::string();
    status.encoder = encoder_name_;
    status.audio = audio_ok_ ? audio_recorder_.description() : "no sound";
    status.error = error_;
    if (status.error.empty() && audio_ok_) {
        status.error = audio_recorder_.error();
    }
    return status;
}

}  // namespace bt::linux_app
