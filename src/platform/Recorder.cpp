#include "platform/Recorder.h"

#include "core/ClipPlan.h"
#include "platform/QpcClock.h"

#include <psapi.h>
#include <shlobj.h>

#include <ctime>
#include <cwctype>

namespace bt {
namespace {

// §3.1 hard byte cap: room for the clip plus slack at twice the configured bitrate. CBR stays near
// the bitrate, so the cap is only reached when keyframes go missing.
constexpr std::size_t kCapSlackSeconds = 2;
constexpr std::size_t kCapBitrateFactor = 2;
constexpr std::size_t kBitsPerKilobit = 1000;
constexpr std::size_t kBitsPerByte = 8;

// A clip starts up to one GOP before `seconds` ago; the sound must reach back that far as well.
constexpr int kAudioExtraSeconds = 2;

// §7: a clip shorter than one GOP (hotkey in the first second) is refused.
constexpr Time100ns kMinimumClip = kTicksPerSecond;

// Rebuilding the video pipeline after a fatal error: quick first retry, then at most every 10 s.
constexpr Time100ns kPipelineRetryInitial = kTicksPerSecond;
constexpr Time100ns kPipelineRetryMax = 10 * kTicksPerSecond;
// Two graphics device resets within a minute: the hardware encoder may be causing them, so the
// rebuilt pipeline keeps the software encoder instead of looping reset -> hardware -> reset.
constexpr Time100ns kDeviceResetWindow = 60 * kTicksPerSecond;
constexpr std::size_t kMaxDeviceResetsBeforeSoftware = 2;
// A software encoder that cannot start is retried this often, not on every supervisor tick.
constexpr Time100ns kEncoderRetryDelay = 5 * kTicksPerSecond;

// §9: counters to the log once a minute at debug level.
constexpr Time100ns kCounterLogInterval = 60 * kTicksPerSecond;
constexpr auto kSupervisorTick = std::chrono::milliseconds(500);

constexpr int kMaxNameAttempts = 999;
constexpr const char* kFallbackFileName = "clip_%Y-%m-%d_%H-%M-%S.mp4";
// Temporary files of a save: <clip>.mp4.tmp while writing, <clip>.mp4.fast.tmp while moving the index.
constexpr std::wstring_view kTempSuffix = L".mp4.tmp";
constexpr std::wstring_view kFastTempSuffix = L".mp4.fast.tmp";

// The cap is a ceiling against a runaway encoder, not a budget. At very low settings (1 Mbit/s at
// 144 fps) an encoder cannot stay near its target and twice the bitrate cut a 30 s clip to 12 s
// (measured), so the cap never goes below what 50 Mbit/s would need.
constexpr std::size_t kCapFloorKbps = 50000;

std::size_t byte_cap(int seconds, int kbps) {
    const std::size_t ceiling_kbps =
        std::max(static_cast<std::size_t>(kbps) * kCapBitrateFactor, kCapFloorKbps);
    return (static_cast<std::size_t>(seconds) + kCapSlackSeconds) * ceiling_kbps * kBitsPerKilobit / kBitsPerByte;
}

std::filesystem::path expand_environment(const std::string& text) {
    const std::wstring wide = to_wide(text);
    const DWORD length = ExpandEnvironmentStringsW(wide.c_str(), nullptr, 0);
    if (length == 0) {
        return wide;
    }
    std::wstring out(length, L'\0');
    ExpandEnvironmentStringsW(wide.c_str(), out.data(), length);
    out.resize(length - 1);  // the count includes the terminator
    return out;
}

std::filesystem::path videos_folder() {
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &raw))) {
        std::filesystem::path path(raw);
        CoTaskMemFree(raw);
        return path;
    }
    CoTaskMemFree(raw);
    return expand_environment("%USERPROFILE%\\Videos");
}

bool usable_folder(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return std::filesystem::is_directory(dir, ec);
}

// §5.2: created if it does not exist; when that fails, clips go to the Videos folder. A relative
// path is taken from the exe's folder, never from wherever Backtrack happened to be started (§8:
// no working directory assumption), and an unknown %VARIABLE% is not taken literally.
std::filesystem::path prepare_output_dir(const std::string& setting, const std::filesystem::path& app_dir,
                                         bool& fell_back) {
    std::filesystem::path dir = expand_environment(setting);
    // A %NAME% that is still there after expansion is a variable that does not exist. A single %
    // in a real folder name ("Clips 100%") is fine.
    const std::wstring expanded = dir.wstring();
    const std::size_t open = expanded.find(L'%');
    const std::size_t close = open == std::wstring::npos ? std::wstring::npos : expanded.find(L'%', open + 1);
    if (close != std::wstring::npos && close > open + 1) {
        const std::filesystem::path fallback = videos_folder();
        log::warn("output_dir '{}' uses an environment variable that does not exist, saving clips in {} instead",
                  setting, to_utf8(fallback.wstring()));
        usable_folder(fallback);
        fell_back = true;
        return fallback;
    }
    if (dir.is_relative()) {
        dir = app_dir / dir;
    }
    if (usable_folder(dir)) {
        return dir;
    }
    const std::filesystem::path fallback = videos_folder();
    log::warn("Cannot create output_dir '{}', saving clips in {} instead", setting, to_utf8(fallback.wstring()));
    usable_folder(fallback);
    fell_back = true;
    return fallback;
}

std::wstring lower(std::wstring text) {
    for (wchar_t& c : text) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return text;
}

// A clip that was being written when the process was killed (console closed during a long save)
// leaves <clip>.mp4.tmp or <clip>.mp4.fast.tmp behind. Only one Backtrack runs at a time, so any
// found at startup are dead. Case-insensitive: the filename setting may say CLIP.MP4. `prefix`
// limits it further in a shared folder (Videos), where other programs keep their own .tmp files.
void remove_stale_temp_files(const std::filesystem::path& dir, const std::wstring& prefix) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::wstring name = entry.path().filename().wstring();
        const std::wstring lower_name = lower(name);
        const bool ours = lower_name.ends_with(kTempSuffix) || lower_name.ends_with(kFastTempSuffix);
        if (entry.is_regular_file(ec) && ours && lower_name.starts_with(lower(prefix))) {
            std::error_code remove_error;
            if (std::filesystem::remove(entry.path(), remove_error)) {
                log::info("Removed {} left over from a save that was interrupted", to_utf8(name));
            }
        }
    }
}

constexpr double kBytesPerMegabyte = 1024.0 * 1024.0;

double megabytes(std::uint64_t bytes) {
    return static_cast<double>(bytes) / kBytesPerMegabyte;
}

}  // namespace

Recorder::Recorder(const Settings& settings, const std::filesystem::path& app_dir, SavedCallback on_saved)
    : settings_(settings),
      on_saved_(std::move(on_saved)),
      output_dir_(prepare_output_dir(settings.output_dir, app_dir, output_dir_fallback_)),
      encoder_preference_(settings.encoder),
      video_buffer_(seconds_to_100ns(settings.clip_seconds), byte_cap(settings.clip_seconds, settings.video_bitrate_kbps)) {
    log::info("Clips are saved to {}", to_utf8(output_dir_.wstring()));
    remove_stale_temp_files(output_dir_, L"");
    // The Videos folder is where a clip goes when the clips folder was gone. Other programs use it
    // too, so only files starting like Backtrack's own names are touched there.
    const std::wstring name_prefix = to_wide(settings_.filename.substr(0, settings_.filename.find('%')));
    if (videos_folder() != output_dir_ && !name_prefix.empty()) {
        remove_stale_temp_files(videos_folder(), name_prefix);
    }
    if (!settings_.audio_enabled && !settings_.mic_enabled) {
        log::info("Sound recording is off ([audio] enabled = false, mic = false)");
        return;
    }
    try {
        // A gap in the sound is filled with silence as far back as the audio buffer reaches, so a
        // clip never contains a hole (one second more than the buffer, for rounding).
        const Time100ns max_gap_fill = seconds_to_100ns(settings_.clip_seconds + kAudioExtraSeconds + 1);
        AudioMixOptions mix;
        mix.system_sound = settings_.audio_enabled;
        mix.system_volume = settings_.system_volume;
        mix.mic = settings_.mic_enabled;
        mix.mic_device = to_wide(settings_.mic_device);
        mix.mic_volume = settings_.mic_volume;
        mix.noise_suppression = settings_.noise_suppression;
        mix.push_to_talk = settings_.push_to_talk;
        mix.system_device = to_wide(settings_.system_device);
        mix.separate_tracks = settings_.separate_tracks;
        const HotkeyParseResult key = parse_hotkey(settings_.push_to_talk_key, HotkeyUse::Held);
        if (key.hotkey) {
            mix.push_to_talk_key = *key.hotkey;
        } else if (settings_.push_to_talk) {
            // Recording the microphone all the time would be the wrong surprise for push-to-talk.
            log::warn("The push-to-talk key '{}' cannot be used ({}); the microphone stays off until it is fixed",
                      settings_.push_to_talk_key, key.error);
        }
        audio_ = std::make_unique<AudioCapture>(
            settings_.audio_bitrate_kbps, max_gap_fill, mix,
            [this](std::size_t track, Packet&& packet) { on_audio_packet(track, std::move(packet)); });
        // Every track holds the same seconds: separate tracks take about three times the memory
        // of the one mixed track (still a few MB against the video's tens).
        for (std::size_t track = 0; track < audio_->track_count(); ++track) {
            audio_buffers_.push_back(std::make_unique<ClipBuffer>(
                seconds_to_100ns(settings.clip_seconds + kAudioExtraSeconds),
                byte_cap(settings.clip_seconds + kAudioExtraSeconds, settings.audio_bitrate_kbps)));
            audio_types_.push_back(audio_->output_type(track));
        }
    } catch (const std::exception& e) {
        log::error("Sound recording is not available ({}); clips are saved without sound", e.what());
        audio_.reset();
        audio_buffers_.clear();
        audio_types_.clear();
    }
}

Recorder::~Recorder() {
    stop();
}

void Recorder::start() {
    std::lock_guard lock(pipeline_mutex_);
    if (started_) {
        return;
    }
    started_ = true;
    rebuild_pipeline_ = true;
    next_pipeline_attempt_ = 0;
    pipeline_retry_delay_ = kPipelineRetryInitial;
    next_counter_log_ = qpc_now() + kCounterLogInterval;
    stop_supervisor_ = false;
    stop_writer_ = false;
    supervisor_ = std::thread(&Recorder::supervisor_main, this);
    writer_ = std::thread(&Recorder::writer_main, this);
    if (audio_) {
        audio_->start(paused_total_);
    }
}

void Recorder::stop() {
    if (supervisor_.joinable()) {
        {
            std::lock_guard lock(wake_mutex_);
            stop_supervisor_ = true;
        }
        wake_.notify_all();
        supervisor_.join();
        log_counters();  // the last minute's counters (supervisor gone: capture_ cannot change now)
    }
    {
        std::lock_guard lock(pipeline_mutex_);
        if (paused_) {
            // A later start() continues the recording clock where it is now.
            paused_total_ += qpc_now() - pause_started_;
            paused_ = false;
        }
        if (capture_) {
            capture_->stop();
        }
        std::unique_ptr<VideoEncoder> encoder;
        {
            std::lock_guard encoder_lock(encoder_mutex_);
            encoder = std::move(encoder_);
        }
        encoder.reset();  // before the capture: its input textures come from the capture's allocator
        capture_.reset();
        if (audio_) {
            audio_->stop();
        }
        pipeline_up_ = false;
        started_ = false;
    }
    if (writer_.joinable()) {
        {
            std::lock_guard lock(writer_mutex_);
            stop_writer_ = true;
        }
        writer_wake_.notify_all();
        writer_.join();  // §6: every queued clip is written before we return
    }
}

void Recorder::pause() {
    std::lock_guard lock(pipeline_mutex_);
    if (!started_ || paused_) {
        return;
    }
    paused_ = true;
    if (capture_) {
        capture_->stop();
    }
    if (audio_) {
        // Also joins the microphone thread, which may be inside a slow Activate/Initialize (a
        // Bluetooth device can take a second or two). WASAPI gives no way to cancel those, and
        // pause_started_ must be taken after the sound has stopped, so this waits under
        // pipeline_mutex_; the supervisor only takes that lock briefly and waits at most as long.
        audio_->stop();
    }
    // Taken once both have stopped: no frame or sound from before the pause is stamped later.
    pause_started_ = qpc_now();
    log::info("Recording paused");
}

void Recorder::resume() {
    {
        std::lock_guard lock(pipeline_mutex_);
        if (!started_ || !paused_) {
            return;
        }
        // The recording clock resumes one frame after where it stopped, not exactly there: otherwise
        // the first frame after the pause can land within a frame of the last one before it, which
        // a player at the clip's frame rate sees as two frames at the same time. Sound and picture
        // both move by that frame, so they stay in step; the clock never goes backwards.
        const Time100ns frame_interval = kTicksPerSecond / settings_.fps;
        paused_total_ += qpc_now() - pause_started_ - frame_interval;
        paused_ = false;
        if (capture_) {
            try {
                capture_->start(paused_total_);
            } catch (const std::exception& e) {
                log::error("Cannot restart screen capture: {}", e.what());
                pipeline_up_ = false;
                rebuild_pipeline_ = true;
            }
        } else {
            rebuild_pipeline_ = true;  // it failed while paused; the supervisor builds it now
        }
        if (audio_) {
            try {
                audio_->start(paused_total_);
            } catch (const std::exception& e) {
                // A thread or event that cannot be made: the picture carries on, the status says
                // "no sound", and the next resume tries again.
                log::error("Cannot restart sound recording: {}", e.what());
            }
        }
        log::info("Recording resumed");
    }
    wake_supervisor();
}

// Owner thread only: it is the only writer of paused_, pause_started_ and paused_total_.
Time100ns Recorder::recording_now() const {
    return (paused_ ? pause_started_ : qpc_now()) - paused_total_;
}

bool Recorder::save_clip(std::string& reason, int seconds) {
    if (!writer_.joinable() || writer_dead_) {
        reason = writer_dead_ ? "Cannot save: the clip writer stopped (details in backtrack.log)"
                              : "Backtrack is not recording";
        return false;
    }
    const Time100ns now = recording_now();
    SaveJob job;
    job.requested = std::chrono::system_clock::now();
    try {
        // §3.8: only pointers to the shared packet bytes are copied, so the lock is held for
        // microseconds even for a 300 s clip.
        std::lock_guard lock(stream_mutex_);
        const int length = seconds > 0 && seconds < settings_.clip_seconds ? seconds : settings_.clip_seconds;
        job.video = video_buffer_.extract(now - seconds_to_100ns(length), now);
        job.video_type = video_type_;
        if (!job.video.empty()) {
            // §6: audio from the first video frame on; the writer drops the packet that starts before it.
            // Names only with separate tracks: a lone track keeps Windows' own, as before.
            for (std::size_t track = 0; track < audio_buffers_.size(); ++track) {
                job.audio.push_back(ClipAudioTrack{audio_types_[track],
                                                   audio_buffers_[track]->extract(job.video.front().pts, now),
                                                   audio_buffers_.size() > 1 ? AudioCapture::track_name(track) : ""});
            }
        }
    } catch (const std::exception& e) {
        reason = std::string("Cannot save: ") + e.what();
        return false;
    }
    if (job.video.empty() || !job.video_type) {
        reason = "Nothing to save yet: there is no video in the buffer";
        return false;
    }
    job.start = job.video.front().pts;
    const Time100ns length = job.video.back().pts + job.video.back().duration - job.video.front().pts;
    if (length < kMinimumClip) {
        reason = "Nothing to save yet: less than a second has been recorded";
        return false;
    }
    ++saves_pending_;
    {
        std::lock_guard lock(writer_mutex_);
        jobs_.push_back(std::move(job));
    }
    writer_wake_.notify_one();
    return true;
}

BufferStatus Recorder::buffer_status() {
    BufferStatus status;
    const ClipBufferStats video = video_buffer_.stats();
    status.bytes = video.bytes;
    for (const auto& buffer : audio_buffers_) {
        status.bytes += buffer->stats().bytes;
    }
    if (video.packets > 0 && video.newest_pts > video.oldest_pts) {
        status.seconds = static_cast<double>(video.newest_pts - video.oldest_pts) / 1e7;
    }
    std::lock_guard lock(encoder_mutex_);
    status.encoder_running = encoder_ != nullptr;
    status.hardware_encoder = encoder_ != nullptr && encoder_->hardware();
    return status;
}

std::string Recorder::status_text() const {
    if (supervisor_dead_ || writer_dead_) {
        return "Not working: internal error, see backtrack.log";
    }
    if (saves_pending_ > 0) {
        return "Saving clip...";
    }
    if (paused_) {
        return "Paused";
    }
    if (!pipeline_up_ || !have_frames_) {
        // Also at the lock screen or with the monitor asleep at startup: no frame has come yet.
        return "Waiting for the screen, retrying";
    }
    if (capture_lost_) {
        return "Capture lost, retrying";
    }
    if (encoder_unavailable_) {
        return "Video encoder failed, retrying";
    }
    std::string encoder;
    {
        std::lock_guard lock(encoder_mutex_);
        if (!encoder_) {
            return "Starting the video encoder";
        }
        encoder = encoder_->hardware() ? "hardware" : "software";
    }
    const bool sound = audio_ && audio_->recording();
    const MicStatus mic = audio_ ? audio_->mic_status() : MicStatus::Off;
    const char* mic_note = mic == MicStatus::Blocked    ? ", microphone blocked by Windows"
                           : mic == MicStatus::NoDevice ? ", no microphone"
                           : mic == MicStatus::Failed   ? ", microphone not working"
                           : mic == MicStatus::OnlyBluetooth ? ", only a Bluetooth headset mic"
                                                        : "";
    return std::format("Recording {} s, {} fps, {}{}{}", settings_.clip_seconds, settings_.fps, encoder,
                       sound ? "" : ", no sound", mic_note);
}

AudioLevels Recorder::audio_levels() const {
    AudioLevels levels;
    if (audio_) {
        levels.system = audio_->system_level();
        levels.mic = audio_->mic_level();
        levels.mic_status = audio_->mic_status();
        levels.mic_name = audio_->mic_name();
        levels.mic_gate_open = audio_->mic_gate_open();
        levels.mic_fallback = audio_->mic_fallback();
        levels.system_name = audio_->system_name();
        levels.system_fallback = audio_->system_fallback();
        levels.ptt_blocked = audio_->push_to_talk_blocked();
    }
    return levels;
}

void Recorder::set_volumes(int system_percent, int mic_percent) {
    if (audio_) {
        audio_->set_volumes(system_percent, mic_percent);
    }
}

void Recorder::set_noise_suppression(bool on) {
    if (audio_) {
        audio_->set_noise_suppression(on);
    }
}

void Recorder::set_mic_muted(bool muted) {
    if (audio_) {
        audio_->set_mic_muted(muted);
    }
}

bool Recorder::mic_muted() const {
    return audio_ && audio_->mic_muted();
}

void Recorder::set_push_to_talk(bool on, Hotkey key) {
    if (audio_) {
        audio_->set_push_to_talk(on, key);
    }
}

void Recorder::simulate_capture_loss() {
    std::lock_guard lock(pipeline_mutex_);
    if (capture_) {
        capture_->simulate_access_lost();
    }
}

void Recorder::wake_supervisor() {
    {
        std::lock_guard lock(wake_mutex_);
        wake_pending_ = true;
    }
    wake_.notify_one();
}

void Recorder::supervisor_main() {
    try {
        ComApartment com(COINIT_MULTITHREADED);
        while (true) {
            {
                std::unique_lock lock(wake_mutex_);
                wake_.wait_for(lock, kSupervisorTick, [this] { return stop_supervisor_ || wake_pending_; });
                if (stop_supervisor_) {
                    break;
                }
                wake_pending_ = false;
            }
            if (rebuild_pipeline_) {
                rebuild_pipeline();
            }
            if (!rebuild_pipeline_) {
                ensure_encoder();
            }
            if (qpc_now() >= next_counter_log_) {
                log_counters();
                next_counter_log_ = qpc_now() + kCounterLogInterval;
            }
        }
    } catch (const std::exception& e) {
        log::error("Recording supervisor stopped: {}", e.what());
        supervisor_dead_ = true;  // §9: the status says so instead of pretending to record
    } catch (...) {
        log::error("Recording supervisor stopped: unknown exception");
        supervisor_dead_ = true;
    }
}

void Recorder::rebuild_pipeline() {
    const Time100ns now = qpc_now();
    std::unique_ptr<ScreenCapture> old_capture;
    std::unique_ptr<VideoEncoder> old_encoder;
    {
        std::lock_guard lock(pipeline_mutex_);
        if (paused_ || !started_ || now < next_pipeline_attempt_) {
            return;
        }
        pipeline_up_ = false;
        if (capture_) {
            capture_->stop();  // after this nobody calls on_frame
        }
        {
            std::lock_guard encoder_lock(encoder_mutex_);
            old_encoder = std::move(encoder_);
        }
        old_capture = std::move(capture_);
    }
    // Torn down and built without pipeline_mutex_: an encoder can take seconds to drain and a
    // capture a moment to start, and the owner thread (status, pause, save) must not wait for it.
    // Every graphics device reset is counted here, whichever thread noticed it first (the capture
    // reports Failed, the encoder fails, or ensure_encoder sees the removed device).
    if (old_capture && FAILED(old_capture->device()->GetDeviceRemovedReason())) {
        device_resets_.push_back(now);
    }
    old_encoder.reset();  // before its capture: the encoder's input textures come from it
    old_capture.reset();
    {
        // A new graphics device means a new encoder with its own SPS/PPS: old packets cannot be
        // joined to new ones in one MP4. (A plain capture loss never gets here, §3.7.)
        std::lock_guard stream_lock(stream_mutex_);
        ++video_generation_;
        video_buffer_.clear();
        video_type_.Reset();
    }

    std::unique_ptr<ScreenCapture> capture;
    try {
        const CaptureOptions options{settings_.monitor, settings_.fps, settings_.resolution, settings_.capture_cursor};
        capture = std::make_unique<ScreenCapture>(
            options, [this](IMFSample* sample, Time100ns pts) { on_frame(sample, pts); },
            [this](CaptureEvent event, VideoSize) { on_capture_event(event); });
    } catch (const std::exception& e) {
        log::warn("Cannot start screen capture ({}), retrying in {} s", e.what(),
                  pipeline_retry_delay_ / kTicksPerSecond);
        next_pipeline_attempt_ = now + pipeline_retry_delay_;
        pipeline_retry_delay_ = std::min(pipeline_retry_delay_ * 2, kPipelineRetryMax);
        return;
    }

    std::lock_guard lock(pipeline_mutex_);
    capture_ = std::move(capture);
    capture_lost_ = false;
    have_frames_ = false;
    // A new graphics device gets the configured encoder again: a hardware encoder that broke on
    // the old device (driver reset) is not to blame for the next one. Unless the resets keep
    // coming: then the hardware encoder may be the cause, and the software one stays.
    const Time100ns since_reset_window = qpc_now() - kDeviceResetWindow;
    std::erase_if(device_resets_, [since_reset_window](Time100ns at) { return at < since_reset_window; });
    if (device_resets_.size() < kMaxDeviceResetsBeforeSoftware) {
        encoder_preference_ = settings_.encoder;
    } else if (encoder_preference_ != EncoderPreference::Software) {
        log::warn("The graphics device was reset {} times within a minute; staying on the software encoder",
                  device_resets_.size());
        encoder_preference_ = EncoderPreference::Software;
    }
    // The flags are set before the thread starts: it may report Failed on its very first tick,
    // and that report must win over "the pipeline is up".
    rebuild_pipeline_ = false;
    rebuild_encoder_ = true;
    pipeline_up_ = true;
    pipeline_retry_delay_ = kPipelineRetryInitial;
    if (paused_) {
        return;  // resume() starts it
    }
    try {
        capture_->start(paused_total_);
    } catch (const std::exception& e) {
        log::error("Cannot start screen capture: {}", e.what());
        pipeline_up_ = false;
        rebuild_pipeline_ = true;
        next_pipeline_attempt_ = now + pipeline_retry_delay_;
    }
}

void Recorder::ensure_encoder() {
    VideoSize size;
    IMFDXGIDeviceManager* manager = nullptr;
    LUID adapter{};
    HRESULT device_state = S_OK;
    {
        std::lock_guard lock(pipeline_mutex_);
        if (!capture_ || paused_ || !pipeline_up_) {
            return;
        }
        size = capture_->frame_size();
        manager = capture_->device_manager();
        adapter = capture_->monitor().adapter_luid;
        device_state = capture_->device()->GetDeviceRemovedReason();
    }
    if (FAILED(device_state)) {
        // The graphics device is gone (driver reset or update). The encoder usually notices
        // before the capture does; the cure is a new device, not a downgrade to software.
        log::info("The graphics device was reset ({}), rebuilding the video pipeline", hresult_text(device_state));
        rebuild_pipeline_ = true;  // counted in rebuild_pipeline
        return;
    }
    rebuild_encoder_ = false;
    if (size.width == 0 || qpc_now() < next_encoder_attempt_) {
        return;  // no frame yet, or waiting before retrying a failed encoder
    }

    std::unique_ptr<VideoEncoder> old;
    {
        std::lock_guard lock(encoder_mutex_);
        if (encoder_ && encoder_->size() == size && !encoder_->failed()) {
            return;
        }
        if (encoder_ && encoder_->failed()) {
            if (encoder_->hardware()) {
                // A hardware encoder that breaks mid-session (driver reset, NVENC session limit)
                // is replaced by the software one so recording continues.
                log::error("{} stopped working, switching to the software encoder", encoder_->name());
                encoder_preference_ = EncoderPreference::Software;
            } else {
                log::error("The software video encoder stopped working, starting it again");
            }
        }
        old = std::move(encoder_);  // the capture thread drops frames until the new one is in
    }
    old.reset();  // drains outside the lock, so the capture thread never waits for it

    std::uint64_t generation = 0;
    {
        std::lock_guard lock(stream_mutex_);
        generation = ++video_generation_;
        video_buffer_.clear();  // an MP4 cannot change resolution or encoder in the middle
        video_type_.Reset();
    }
    try {
        const VideoEncoderSettings encoder_settings{size, settings_.fps, settings_.video_bitrate_kbps,
                                                    encoder_preference_, adapter, settings_.quality};
        auto encoder = std::make_unique<VideoEncoder>(
            encoder_settings, manager,
            [this, generation](Packet&& packet, IMFMediaType* type) { on_video_packet(generation, std::move(packet), type); });
        std::lock_guard lock(encoder_mutex_);
        encoder_ = std::move(encoder);
        encoder_unavailable_ = false;
    } catch (const std::exception& e) {
        encoder_unavailable_ = true;
        if (encoder_preference_ != EncoderPreference::Software) {
            log::error("Cannot start the video encoder ({}), trying the software encoder", e.what());
            encoder_preference_ = EncoderPreference::Software;
            wake_supervisor();
        } else {
            log::error("Cannot start the video encoder ({}), retrying in {} s", e.what(),
                       kEncoderRetryDelay / kTicksPerSecond);
            next_encoder_attempt_ = qpc_now() + kEncoderRetryDelay;
        }
    }
}

void Recorder::on_frame(IMFSample* sample, Time100ns pts) {
    std::lock_guard lock(encoder_mutex_);
    if (!encoder_ || encoder_->failed() || !(encoder_->size() == capture_->frame_size())) {
        ++frames_without_encoder_;
        if (!rebuild_encoder_.exchange(true)) {
            wake_supervisor();
        }
        return;
    }
    try {
        if (encoder_->needs_system_memory()) {
            LONGLONG duration = 0;
            BT_CHECK_HR(sample->GetSampleDuration(&duration));
            encoder_->encode_system_memory(capture_->read_nv12(sample), pts, duration);
        } else {
            encoder_->encode(sample);
        }
    } catch (const std::exception& e) {
        ++frames_without_encoder_;
        if (frame_error_log_.allow()) {
            log::error("Handing a frame to the encoder failed: {}", e.what());
        }
    }
}

void Recorder::on_capture_event(CaptureEvent event) {
    switch (event) {
        case CaptureEvent::FormatChanged:
            have_frames_ = true;
            rebuild_encoder_ = true;
            wake_supervisor();
            break;
        case CaptureEvent::Lost:
            capture_lost_ = true;
            break;
        case CaptureEvent::Recovered:
            capture_lost_ = false;
            break;
        case CaptureEvent::Failed:
            capture_lost_ = false;
            pipeline_up_ = false;
            rebuild_pipeline_ = true;
            wake_supervisor();
            break;
    }
}

void Recorder::on_video_packet(std::uint64_t generation, Packet&& packet, IMFMediaType* type) {
    std::lock_guard lock(stream_mutex_);
    if (generation != video_generation_) {
        return;  // from an encoder that has been replaced
    }
    if (type != nullptr && type != video_type_.Get()) {
        if (video_type_) {
            // The encoder renegotiated its format: earlier packets belong to the old sequence header.
            log::info("Video format changed, the clip buffer starts over");
            video_buffer_.clear();
        }
        video_type_ = type;
    }
    video_buffer_.push(std::move(packet));
}

void Recorder::on_audio_packet(std::size_t track, Packet&& packet) {
    std::lock_guard lock(stream_mutex_);
    if (track < audio_buffers_.size()) {
        audio_buffers_[track]->push(std::move(packet));
    }
}

void Recorder::log_counters() {
    if (!log::enabled(LogLevel::Debug)) {
        return;
    }
    if (capture_) {  // supervisor thread: the only one that replaces capture_
        const CaptureStats c = capture_->stats();
        log::debug("Capture: {} ticks, {} new frames, {} repeated, {} dropped (encoder behind), {} conversion failures, "
                   "{} late ticks skipped, {} capture losses, {} delivered, {} without an encoder",
                   c.ticks, c.new_frames, c.repeated_frames, c.allocator_empty, c.convert_failed, c.late_ticks_skipped,
                   c.access_lost, c.delivered, frames_without_encoder_.load());
    }
    {
        std::lock_guard lock(encoder_mutex_);
        if (encoder_) {
            const VideoEncoderStats e = encoder_->stats();
            log::debug("Encoder: {} frames in, {} dropped (queue full), {} input errors, {} packets out, {} keyframes, "
                       "{:.1f} MB",
                       e.frames_in, e.frames_dropped, e.input_errors, e.packets_out, e.keyframes, megabytes(e.bytes_out));
        }
    }
    if (audio_) {
        const AudioStats a = audio_->stats();
        log::debug("Sound: {} packets, {} ms silence filled in, {} ms trimmed, {} silent packets, device opened {} times",
                   a.packets_out, a.silence_inserted_ms, a.trimmed_ms, a.silent_packets, a.device_opens);
        if (a.mic) {
            constexpr double kFramesPerMs = 48.0;
            log::debug("Microphone: {} packets, opened {} times; mix: headroom {:.1f} ms, {} dropouts ({:.0f} ms), "
                       "{:.0f} ms dropped at once, {:.1f} ms caught up by resampling, {:.0f} ms overflowed",
                       a.mic_capture.packets, a.mic_capture.device_opens, a.mic_fifo.headroom_frames / kFramesPerMs,
                       a.mic_fifo.underflows, static_cast<double>(a.mic_fifo.underflow_frames) / kFramesPerMs,
                       static_cast<double>(a.mic_fifo.dropped_frames) / kFramesPerMs,
                       static_cast<double>(a.mic_fifo.corrected_frames) / kFramesPerMs,
                       static_cast<double>(a.mic_fifo.overflow_frames) / kFramesPerMs);
        }
    }
    ClipBufferStats v;
    ClipBufferStats a;  // the sound tracks together; the seconds are the first one's
    {
        std::lock_guard lock(stream_mutex_);
        v = video_buffer_.stats();
        for (std::size_t track = 0; track < audio_buffers_.size(); ++track) {
            const ClipBufferStats t = audio_buffers_[track]->stats();
            if (track == 0) {
                a = t;
            } else {
                a.bytes += t.bytes;
                a.dropped_out_of_order += t.dropped_out_of_order;
                a.dropped_waiting_for_keyframe += t.dropped_waiting_for_keyframe;
                a.dropped_by_byte_cap += t.dropped_by_byte_cap;
            }
        }
    }
    log::debug("Buffers: video {:.1f} s {:.1f} MB, sound {:.1f} s {:.1f} MB; dropped {} out of order, {} waiting for "
               "a keyframe, {} by the byte cap",
               static_cast<double>(v.newest_pts - v.oldest_pts) / kTicksPerSecond, megabytes(v.bytes),
               static_cast<double>(a.newest_pts - a.oldest_pts) / kTicksPerSecond, megabytes(a.bytes),
               v.dropped_out_of_order + a.dropped_out_of_order,
               v.dropped_waiting_for_keyframe + a.dropped_waiting_for_keyframe,
               v.dropped_by_byte_cap + a.dropped_by_byte_cap);
    PROCESS_MEMORY_COUNTERS memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) {
        log::debug("Memory: working set {:.1f} MB, private {:.1f} MB", megabytes(memory.WorkingSetSize),
                   megabytes(memory.PagefileUsage));
    }
}

void Recorder::writer_main() {
    try {
        ComApartment com(COINIT_MULTITHREADED);
        while (true) {
            SaveJob job;
            {
                std::unique_lock lock(writer_mutex_);
                writer_wake_.wait(lock, [this] { return stop_writer_ || !jobs_.empty(); });
                if (jobs_.empty()) {
                    break;  // stop requested and nothing left to write
                }
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            SaveResult result;
            result.start = job.start;
            try {
                ClipWriteRequest request{next_clip_path(job.requested), job.video_type, std::move(job.video),
                                         std::move(job.audio)};
                result.path = request.path;
                const ClipWriteResult written = write_clip(request);
                result.ok = written.ok;
                result.seconds = written.seconds;
                result.message = written.ok
                    ? std::format("Saved {} ({:.1f} s{})", to_utf8(result.path.filename().wstring()), written.seconds,
                                  written.audio_frames > 0 ? "" : ", no sound")
                    : "Saving the clip failed: " + written.error + " (details in backtrack.log)";
            } catch (const std::exception& e) {
                result.ok = false;
                result.message = std::string("Saving the clip failed: ") + e.what() + " (details in backtrack.log)";
            }
            if (!result.ok) {
                log::error("{}", result.message);
            }
            --saves_pending_;
            try {
                on_saved_(result);
            } catch (const std::exception& e) {
                log::error("Clip saved callback failed: {}", e.what());
            }
        }
    } catch (const std::exception& e) {
        log::error("Clip writer stopped: {}", e.what());
        writer_dead_ = true;
    } catch (...) {
        log::error("Clip writer stopped: unknown exception");
        writer_dead_ = true;
    }
    if (writer_dead_) {
        // §9: nothing waits for clips that will never be written, and the status says why.
        std::lock_guard lock(writer_mutex_);
        saves_pending_ -= static_cast<int>(jobs_.size());
        jobs_.clear();
    }
}

std::filesystem::path Recorder::next_clip_path(std::chrono::system_clock::time_point when) {
    // §5.2: the folder may have gone since startup (deleted, USB drive pulled): recreate it, or
    // fall back to the Videos folder for this clip.
    std::filesystem::path dir = output_dir_;
    if (!usable_folder(dir)) {
        dir = videos_folder();
        log::warn("{} is not available, saving this clip in {}", to_utf8(output_dir_.wstring()), to_utf8(dir.wstring()));
        if (!usable_folder(dir)) {
            throw std::runtime_error("neither the clips folder nor the Videos folder can be written to");
        }
    }
    const std::time_t time = std::chrono::system_clock::to_time_t(when);
    std::tm local{};
    localtime_s(&local, &time);
    std::string name = expand_clip_name(settings_.filename, local);
    if (name.empty()) {
        name = expand_clip_name(kFallbackFileName, local);
    }
    std::error_code ec;
    std::filesystem::path candidate = dir / to_wide(name);
    // Two saves in the same second get clip.mp4 and clip_2.mp4, never one overwriting the other.
    for (int n = 2; std::filesystem::exists(candidate, ec); ++n) {
        if (n > kMaxNameAttempts) {
            throw std::runtime_error(std::format("{} and {} numbered copies already exist; put a time placeholder "
                                                 "such as %H-%M-%S in the filename setting",
                                                 name, kMaxNameAttempts));
        }
        candidate = dir / to_wide(numbered_clip_name(name, n));
    }
    return candidate;
}

}  // namespace bt
