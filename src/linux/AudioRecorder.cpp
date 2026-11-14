#include "linux/AudioRecorder.h"

#include "core/AudioFifo.h"
#include "core/AudioMix.h"
#include "core/NoiseSuppressor.h"
#include "core/WindowedMinimum.h"
#include "linux/Log.h"
#include "linux/MonotonicClock.h"

#include <pulse/error.h>
#include <pulse/simple.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

#include <cstdlib>
#include <format>
#include <vector>

namespace bt::linux_app {

namespace {

constexpr int kRate = 48000;
constexpr std::size_t kBlockFrames = 480;  // 10 ms
constexpr std::size_t kAacFrames = 1024;
// The monitor's timestamps are re-anchored when they drift further than this from the clock.
constexpr Time100ns kMaxDrift = 100 * kTicksPerMillisecond;

std::string pulse_error(int code) {
    return pa_strerror(code);
}

pa_simple* open_stream(const char* device, const char* description, pa_sample_format_t format, std::uint8_t channels,
                       std::size_t bytes_per_block, std::string& error) {
    pa_sample_spec spec{};
    spec.format = format;
    spec.rate = kRate;
    spec.channels = channels;
    pa_buffer_attr attr{};
    attr.maxlength = static_cast<std::uint32_t>(-1);
    attr.tlength = static_cast<std::uint32_t>(-1);
    attr.prebuf = static_cast<std::uint32_t>(-1);
    attr.minreq = static_cast<std::uint32_t>(-1);
    attr.fragsize = static_cast<std::uint32_t>(bytes_per_block);  // deliver every 10 ms, not in big lumps
    int code = 0;
    pa_simple* stream = pa_simple_new(nullptr, "Backtrack", PA_STREAM_RECORD, device, description, &spec, nullptr, &attr, &code);
    if (stream == nullptr) {
        error = pulse_error(code);
    }
    return stream;
}

}  // namespace

struct AudioRecorder::Streams {
    pa_simple* system = nullptr;
    pa_simple* mic = nullptr;
    AVCodecContext* aac = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;

    ~Streams() {
        if (system != nullptr) pa_simple_free(system);
        if (mic != nullptr) pa_simple_free(mic);
        avcodec_free_context(&aac);
        av_frame_free(&frame);
        av_packet_free(&packet);
    }
};

AudioRecorder::AudioRecorder() = default;

AudioRecorder::~AudioRecorder() {
    stop();
}

bool AudioRecorder::start(const AudioConfig& config, ClipBuffer& out, std::string& error) {
    stop();
    config_ = config;
    out_ = &out;
    {
        std::lock_guard lock(mutex_);
        error_.clear();
        parameters_.reset();
        mic_ok_ = false;
    }
    if (!config.system && !config.mic) {
        error = "Sound is switched off";
        return false;
    }
    auto streams = std::make_unique<Streams>();

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    streams->aac = codec != nullptr ? avcodec_alloc_context3(codec) : nullptr;
    if (streams->aac == nullptr) {
        error = "No AAC encoder in this FFmpeg";
        return false;
    }
    AVCodecContext* aac = streams->aac;
    aac->sample_fmt = AV_SAMPLE_FMT_FLTP;
    aac->sample_rate = kRate;
    av_channel_layout_default(&aac->ch_layout, 2);
    aac->bit_rate = static_cast<std::int64_t>(config.bitrate_kbps) * 1000;
    aac->time_base = AVRational{1, kRate};
    aac->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;  // AudioSpecificConfig in extradata, for the MP4 header
    if (const int result = avcodec_open2(aac, codec, nullptr); result < 0) {
        error = "The AAC encoder does not open";
        return false;
    }
    streams->frame = av_frame_alloc();
    streams->packet = av_packet_alloc();
    if (streams->frame == nullptr || streams->packet == nullptr) {
        error = "Out of memory";
        return false;
    }
    streams->frame->format = AV_SAMPLE_FMT_FLTP;
    streams->frame->sample_rate = kRate;
    streams->frame->nb_samples = static_cast<int>(kAacFrames);
    av_channel_layout_copy(&streams->frame->ch_layout, &aac->ch_layout);
    if (av_frame_get_buffer(streams->frame, 0) < 0) {
        error = "Out of memory";
        return false;
    }

    std::string why;
    // @DEFAULT_MONITOR@ follows the default output, also when it changes while recording.
    streams->system = open_stream("@DEFAULT_MONITOR@", "Game sound", PA_SAMPLE_S16LE, 2, kBlockFrames * 4, why);
    if (streams->system == nullptr) {
        error = "No sound server (" + why + "). On Arch: install pipewire-pulse";
        return false;
    }
    bool mic_ok = false;
    if (config.mic) {
        streams->mic = open_stream("@DEFAULT_SOURCE@", "Microphone", PA_SAMPLE_FLOAT32LE, 1, kBlockFrames * 4, why);
        mic_ok = streams->mic != nullptr;
        if (!mic_ok) {
            log_warning("No microphone: " + why);
        }
    }
    {
        std::lock_guard lock(mutex_);
        parameters_ = copy_parameters(aac);
        mic_ok_ = mic_ok;
    }
    streams_ = std::move(streams);
    running_ = true;
    if (mic_ok) {
        mic_fifo_ = std::make_unique<AudioFifo>(std::size_t{1} << 16);
        mic_running_ = true;
        mic_thread_ = std::thread([this] { mic_loop(); });
    }
    system_thread_ = std::thread([this] { system_loop(); });
    log_info("Sound: " + description());
    return true;
}

void AudioRecorder::stop() {
    running_ = false;
    mic_running_ = false;
    if (system_thread_.joinable()) system_thread_.join();
    if (mic_thread_.joinable()) mic_thread_.join();
    streams_.reset();
    mic_fifo_.reset();
}

void AudioRecorder::mic_loop() {
    NoiseSuppressor suppressor(config_.noise_suppression);
    std::vector<float> block(kBlockFrames);
    while (mic_running_) {
        int code = 0;
        if (pa_simple_read(streams_->mic, block.data(), block.size() * sizeof(float), &code) < 0) {
            log_warning("Microphone stopped: " + pulse_error(code));
            break;  // the mix pulls zeros from now on
        }
        suppressor.process(block);
        mic_fifo_->push(block);
    }
}

void AudioRecorder::system_loop() {
    Streams& s = *streams_;
    const bool mic = mic_fifo_ != nullptr;
    GainRamp system_gain(config_.system ? static_cast<float>(config_.system_volume) / 100.0f : 0.0f, kBlockFrames);
    GainRamp mic_gain(static_cast<float>(config_.mic_volume) / 100.0f, kBlockFrames);
    std::vector<std::int16_t> block(kBlockFrames * 2);
    std::vector<float> mic_block(mic ? kBlockFrames : 0);
    std::vector<std::int16_t> pending;  // mixed stereo not yet handed to the AAC encoder
    pending.reserve((kAacFrames + kBlockFrames) * 2);

    std::int64_t frames_read = 0;
    std::int64_t samples_encoded = 0;
    std::int64_t anchor_frame = 0;
    Time100ns anchor_time = 0;
    bool anchored = false;
    WindowedMinimum drift(2 * kTicksPerSecond);
    const auto frame_time = [&](std::int64_t frame) {
        return anchor_time + (frame - anchor_frame) * kTicksPerSecond / kRate;
    };

    while (running_) {
        int code = 0;
        if (pa_simple_read(s.system, block.data(), block.size() * sizeof(std::int16_t), &code) < 0) {
            set_error("Sound stopped: " + pulse_error(code));
            return;
        }
        // When the block's first frame was recorded: now, minus what still waits in the server,
        // minus the block itself. Reads can be late, never early, so the minimum over two seconds
        // is the real offset (the same idea as the Windows recorder).
        const Time100ns now = now_100ns();
        pa_usec_t latency = pa_simple_get_latency(s.system, &code);
        if (latency == static_cast<pa_usec_t>(-1)) {
            latency = 0;  // unknown this time; the windowed minimum smooths it out
        }
        const Time100ns block_start = now - static_cast<Time100ns>(latency) * 10 -
                                      static_cast<Time100ns>(kBlockFrames) * kTicksPerSecond / kRate;
        if (!anchored) {
            anchor_frame = frames_read;
            anchor_time = block_start;
            anchored = true;
        } else {
            drift.add(now, block_start - frame_time(frames_read));
            const Time100ns offset = drift.value().value_or(0);
            if (std::abs(offset) > kMaxDrift) {
                log_info(std::format("Sound clock re-anchored by {} ms", offset / kTicksPerMillisecond));
                anchor_time = frame_time(frames_read) + offset;
                anchor_frame = frames_read;
                drift.reset();
            }
        }
        frames_read += static_cast<std::int64_t>(kBlockFrames);

        if (mic) {
            mic_fifo_->pull(mic_block);
        }
        mix_block(block, mic_block, system_gain, mic_gain);
        pending.insert(pending.end(), block.begin(), block.end());

        while (pending.size() >= kAacFrames * 2) {
            if (av_frame_make_writable(s.frame) < 0) {
                set_error("Sound encoder: out of memory");
                return;
            }
            auto* left = reinterpret_cast<float*>(s.frame->data[0]);
            auto* right = reinterpret_cast<float*>(s.frame->data[1]);
            for (std::size_t i = 0; i < kAacFrames; ++i) {
                left[i] = static_cast<float>(pending[2 * i]) / 32768.0f;
                right[i] = static_cast<float>(pending[2 * i + 1]) / 32768.0f;
            }
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(kAacFrames * 2));
            s.frame->pts = samples_encoded;
            samples_encoded += static_cast<std::int64_t>(kAacFrames);
            if (avcodec_send_frame(s.aac, s.frame) < 0) {
                set_error("The sound encoder failed");
                return;
            }
            while (avcodec_receive_packet(s.aac, s.packet) == 0) {
                Packet packet;
                packet.pts = frame_time(s.packet->pts);
                packet.duration = s.packet->duration * kTicksPerSecond / kRate;
                packet.keyframe = true;  // every AAC frame decodes on its own
                packet.data = make_packet_bytes(std::vector<std::uint8_t>(s.packet->data, s.packet->data + s.packet->size));
                out_->push(std::move(packet));
                av_packet_unref(s.packet);
            }
        }
    }
}

void AudioRecorder::set_error(std::string message) {
    log_warning(message);
    std::lock_guard lock(mutex_);
    error_ = std::move(message);
}

CodecParameters AudioRecorder::parameters() const {
    std::lock_guard lock(mutex_);
    return parameters_;
}

std::string AudioRecorder::error() const {
    std::lock_guard lock(mutex_);
    return error_;
}

std::string AudioRecorder::description() const {
    std::lock_guard lock(mutex_);
    if (!parameters_) {
        return "no sound";
    }
    if (config_.system && mic_ok_) return "system sound + microphone";
    if (mic_ok_) return "microphone";
    return config_.system ? "system sound" : "no sound";
}

}  // namespace bt::linux_app
