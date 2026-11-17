#include "linux/Player.h"

#include "linux/Log.h"
#include "linux/Media.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <pulse/error.h>
#include <pulse/simple.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace bt::linux_app {

namespace {

constexpr int kSoundRate = 48000;
constexpr int kSoundChannels = 2;
// The decoder stays this far ahead: a few pictures, half a second of sound. A file that keeps
// its sound in big blocks (other recorders) may need more pictures before the next sound arrives,
// up to the hard limit.
constexpr std::size_t kSoftFrames = 6;
constexpr std::size_t kHardFrames = 24;
constexpr double kSoundAhead = 0.5;
constexpr double kSoundMost = 3.0;  // a file with far more sound than pictures: still bounded
// Sound this far off the clock is dropped (late) or waited for (early).
constexpr double kSoundSlack = 0.03;
// What the sound server keeps queued: short, so a pause or a seek is heard at once.
constexpr double kSoundBuffer = 0.06;

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The largest size with the video's shape that fits the view.
void fit(int video_w, int video_h, int view_w, int view_h, int& out_w, int& out_h) {
    if (view_w <= 0 || view_h <= 0) {
        out_w = video_w;
        out_h = video_h;
        return;
    }
    const double scale =
        std::min(static_cast<double>(view_w) / video_w, static_cast<double>(view_h) / video_h);
    out_w = std::max(1, static_cast<int>(std::lround(video_w * scale)));
    out_h = std::max(1, static_cast<int>(std::lround(video_h * scale)));
}

}  // namespace

struct Player::Decoding {
    std::string path;
    AVFormatContext* input = nullptr;
    AVStream* video = nullptr;
    AVStream* audio = nullptr;
    AVCodecContext* video_decoder = nullptr;
    AVCodecContext* audio_decoder = nullptr;
    SwsContext* sws = nullptr;
    SwrContext* swr = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* held = nullptr;  // the last picture skipped while seeking, for a seek past the last one
    bool holding = false;
    AVPacket* packet = nullptr;
    double zero = 0.0;  // the video track's first time, in seconds
    double skip_before = 0.0;  // after a seek: pictures and sound before this are not shown
    double half_frame = 0.001;
    bool shown_since_seek = false;
    bool flushed = false;
    struct ScaleKey {
        int width = 0;
        int height = 0;
        int format = -1;
        int out_width = 0;
        int out_height = 0;
        int colorspace = -1;
        int range = -1;
        bool operator==(const ScaleKey&) const = default;
    };
    ScaleKey scale_key;

    ~Decoding() {
        av_packet_free(&packet);
        av_frame_free(&held);
        av_frame_free(&frame);
        swr_free(&swr);
        sws_freeContext(sws);
        avcodec_free_context(&audio_decoder);
        avcodec_free_context(&video_decoder);
        avformat_close_input(&input);
    }

    static AVCodecContext* open_decoder(const AVStream* stream) {
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (codec == nullptr) {
            return nullptr;
        }
        AVCodecContext* context = avcodec_alloc_context3(codec);
        if (context == nullptr || avcodec_parameters_to_context(context, stream->codecpar) < 0) {
            avcodec_free_context(&context);
            return nullptr;
        }
        // The packets' time base: lets the decoder move the times past skipped AAC priming.
        context->pkt_timebase = stream->time_base;
        // A few threads so 1440p at 120 fps keeps up; not one per core: frame threads hold each
        // picture back by one frame per thread, which a seek then waits for.
        context->thread_count = 4;
        if (avcodec_open2(context, codec, nullptr) < 0) {
            avcodec_free_context(&context);
        }
        return context;
    }

    bool open() {
        if (avformat_open_input(&input, path.c_str(), nullptr, nullptr) < 0 ||
            avformat_find_stream_info(input, nullptr) < 0) {
            return false;
        }
        const int video_index = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (video_index < 0) {
            return false;
        }
        video = input->streams[video_index];
        video_decoder = open_decoder(video);
        if (video_decoder == nullptr) {
            return false;
        }
        const int audio_index = av_find_best_stream(input, AVMEDIA_TYPE_AUDIO, -1, video_index, nullptr, 0);
        if (audio_index >= 0) {
            audio = input->streams[audio_index];
            audio_decoder = open_decoder(audio);
            if (audio_decoder == nullptr) {
                audio = nullptr;  // the picture still plays
            }
        }
        for (unsigned i = 0; i < input->nb_streams; ++i) {
            input->streams[i]->discard =
                (input->streams[i] == video || input->streams[i] == audio) ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
        }
        zero = video->start_time != AV_NOPTS_VALUE ? static_cast<double>(video->start_time) * av_q2d(video->time_base)
                                                   : 0.0;
        const AVRational rate = av_guess_frame_rate(input, video, nullptr);
        if (rate.num > 0 && rate.den > 0) {
            half_frame = 0.5 / av_q2d(rate);
        }
        frame = av_frame_alloc();
        held = av_frame_alloc();
        packet = av_packet_alloc();
        return frame != nullptr && held != nullptr && packet != nullptr;
    }

    double duration() const {
        if (video->duration > 0 && video->duration != AV_NOPTS_VALUE) {
            return static_cast<double>(video->duration) * av_q2d(video->time_base);
        }
        if (input->duration > 0) {
            // Already counted from the file's start, so not from `zero` again.
            return static_cast<double>(input->duration) / AV_TIME_BASE;
        }
        return 0.0;
    }

    double seconds(const AVFrame* f, const AVStream* stream) const {
        const std::int64_t at = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        return at == AV_NOPTS_VALUE ? 0.0 : static_cast<double>(at) * av_q2d(stream->time_base) - zero;
    }

    void seek(double target) {
        const std::int64_t at = static_cast<std::int64_t>(std::max(0.0, target + zero) / av_q2d(video->time_base));
        if (av_seek_frame(input, video->index, at, AVSEEK_FLAG_BACKWARD) < 0) {
            av_seek_frame(input, video->index, 0, AVSEEK_FLAG_BACKWARD);
        }
        avcodec_flush_buffers(video_decoder);
        if (audio_decoder != nullptr) {
            avcodec_flush_buffers(audio_decoder);
        }
        swr_free(&swr);
        skip_before = target;
        shown_since_seek = false;
        holding = false;
        flushed = false;
        av_frame_unref(held);
    }

    QImage picture(const AVFrame* f, int view_w, int view_h) {
        int out_w = 0;
        int out_h = 0;
        fit(f->width, f->height, view_w, view_h, out_w, out_h);
        const ScaleKey key{f->width, f->height, f->format, out_w, out_h, static_cast<int>(f->colorspace),
                            static_cast<int>(f->color_range)};
        if (sws == nullptr || !(key == scale_key)) {
            // A new size or colour: a new context, given the frame's colours (a fresh one would
            // fall back to swscale's BT.601 defaults).
            sws_freeContext(sws);
            sws = sws_getContext(f->width, f->height, static_cast<AVPixelFormat>(f->format), out_w, out_h, AV_PIX_FMT_BGRA,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (sws == nullptr) {
                return {};
            }
            use_frame_colors(sws, f);
            scale_key = key;
        }
        QImage image(out_w, out_h, QImage::Format_RGB32);
        if (image.isNull()) {
            return {};
        }
        std::uint8_t* planes[4] = {image.bits(), nullptr, nullptr, nullptr};
        const int strides[4] = {static_cast<int>(image.bytesPerLine()), 0, 0, 0};
        sws_scale(sws, f->data, f->linesize, 0, f->height, planes, strides);
        return image;
    }

    // Stereo 16-bit at kSoundRate; the samples before skip_before are cut off.
    bool sound(const AVFrame* f, SoundChunk& chunk) {
        if (swr == nullptr) {
            AVChannelLayout stereo{};
            av_channel_layout_default(&stereo, kSoundChannels);
            if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, kSoundRate, &f->ch_layout,
                                    static_cast<AVSampleFormat>(f->format), f->sample_rate, 0, nullptr) < 0 ||
                swr_init(swr) < 0) {
                swr_free(&swr);
                return false;
            }
        }
        const int room = swr_get_out_samples(swr, f->nb_samples);
        if (room <= 0) {
            return false;
        }
        chunk.samples.resize(static_cast<std::size_t>(room) * kSoundChannels);
        std::uint8_t* out[1] = {reinterpret_cast<std::uint8_t*>(chunk.samples.data())};
        const int made = swr_convert(swr, out, room, const_cast<const std::uint8_t**>(f->extended_data), f->nb_samples);
        if (made <= 0) {
            return false;
        }
        chunk.samples.resize(static_cast<std::size_t>(made) * kSoundChannels);
        chunk.pts = seconds(f, audio);
        const double cut = skip_before - chunk.pts;
        if (cut > 0) {
            const auto skip = static_cast<std::size_t>(cut * kSoundRate) * kSoundChannels;
            if (skip >= chunk.samples.size()) {
                return false;
            }
            chunk.samples.erase(chunk.samples.begin(), chunk.samples.begin() + static_cast<std::ptrdiff_t>(skip));
            chunk.pts = skip_before;
        }
        return true;
    }
};

Player::Player(const std::string& path) : decoding_(std::make_unique<Decoding>()) {
    decoding_->path = path;
    anchor_wall_ = now_ns();
    decode_thread_ = std::thread([this] { decode_loop(); });
    sound_thread_ = std::thread([this] { sound_loop(); });
}

Player::~Player() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    decode_wake_.notify_all();
    sound_wake_.notify_all();
    decode_thread_.join();
    sound_thread_.join();
}

std::string Player::error() const {
    std::lock_guard lock(mutex_);
    return error_;
}

bool Player::ready() const {
    std::lock_guard lock(mutex_);
    return opened_;
}

double Player::duration() const {
    std::lock_guard lock(mutex_);
    return duration_;
}

int Player::video_width() const {
    std::lock_guard lock(mutex_);
    return video_width_;
}

int Player::video_height() const {
    std::lock_guard lock(mutex_);
    return video_height_;
}

double Player::clock_locked() const {
    if (!playing_) {
        return anchor_media_;
    }
    const double run = static_cast<double>(now_ns() - anchor_wall_) / 1e9 * rate_;
    return std::min(anchor_media_ + run, std::max(duration_, anchor_media_));
}

void Player::anchor_locked(double media_time) {
    anchor_media_ = media_time;
    anchor_wall_ = now_ns();
}

double Player::sound_queued_locked() const {
    double seconds = 0.0;
    for (const SoundChunk& chunk : sound_) {
        seconds += static_cast<double>(chunk.samples.size()) / (kSoundRate * kSoundChannels);
    }
    return seconds;
}

void Player::play() {
    bool restart = false;
    {
        std::lock_guard lock(mutex_);
        if (!opened_ || playing_) {
            return;
        }
        restart = duration_ > 0 && anchor_media_ >= duration_ - 0.01;
    }
    if (restart) {
        seek(0.0);  // played to the end: from the start again
    }
    {
        std::lock_guard lock(mutex_);
        anchor_locked(anchor_media_);
        playing_ = true;
    }
    sound_wake_.notify_all();
}

void Player::pause() {
    {
        std::lock_guard lock(mutex_);
        if (!playing_) {
            return;
        }
        anchor_locked(clock_locked());
        playing_ = false;
    }
    sound_wake_.notify_all();
}

bool Player::playing() const {
    std::lock_guard lock(mutex_);
    return playing_;
}

bool Player::ended() const {
    std::lock_guard lock(mutex_);
    return opened_ && duration_ > 0 && clock_locked() >= duration_ - 0.001;
}

void Player::seek(double seconds) {
    {
        std::lock_guard lock(mutex_);
        if (!opened_ || !std::isfinite(seconds)) {
            return;
        }
        const double target = std::clamp(seconds, 0.0, duration_);
        ++generation_;
        seek_pending_ = true;
        seek_target_ = target;
        anchor_locked(target);
        want_frame_ = true;
        decoder_done_ = false;
        frames_.clear();
        sound_.clear();
    }
    decode_wake_.notify_all();
    sound_wake_.notify_all();
}

double Player::position() const {
    std::lock_guard lock(mutex_);
    return clock_locked();
}

void Player::set_volume(double volume) {
    std::lock_guard lock(mutex_);
    volume_ = std::clamp(volume, 0.0, 1.0);
}

void Player::set_muted(bool muted) {
    std::lock_guard lock(mutex_);
    muted_ = muted;
}

void Player::set_rate(double rate) {
    {
        std::lock_guard lock(mutex_);
        anchor_locked(clock_locked());
        rate_ = std::clamp(rate, 0.25, 4.0);
    }
    sound_wake_.notify_all();
}

void Player::set_view_size(int width, int height) {
    std::lock_guard lock(mutex_);
    view_width_ = width;
    view_height_ = height;
}

bool Player::take_frame(QImage& frame) {
    std::unique_lock lock(mutex_);
    std::erase_if(frames_, [this](const VideoFrame& f) { return f.generation != generation_; });
    const double clock = clock_locked();
    if (playing_ && duration_ > 0 && clock >= duration_ - 0.001 && decoder_done_ && frames_.empty()) {
        anchor_locked(duration_);
        playing_ = false;
    }
    std::size_t pick = frames_.size();
    for (std::size_t i = 0; i < frames_.size() && frames_[i].pts <= clock + 0.002; ++i) {
        pick = i;
    }
    if (pick == frames_.size()) {
        if (!want_frame_ || frames_.empty()) {
            return false;
        }
        pick = 0;  // the first picture after a seek, just after the target
    }
    frame = std::move(frames_[pick].image);
    last_shown_ = frames_[pick].pts;
    frames_.erase(frames_.begin(), frames_.begin() + static_cast<std::ptrdiff_t>(pick) + 1);
    want_frame_ = false;
    lock.unlock();
    decode_wake_.notify_all();
    return !frame.isNull();
}

void Player::decode_loop() {
    Decoding& d = *decoding_;
    try {
        if (!d.open()) {
            log_warning("Cannot play " + d.path);
            std::lock_guard lock(mutex_);
            error_ = "This clip could not be opened for playing.";
            return;
        }
        {
            std::lock_guard lock(mutex_);
            duration_ = d.duration();
            video_width_ = d.video->codecpar->width;
            video_height_ = d.video->codecpar->height;
            has_sound_ = d.audio != nullptr;
            opened_ = true;
        }
        sound_wake_.notify_all();

        while (true) {
            std::uint64_t generation = 0;
            int view_w = 0;
            int view_h = 0;
            {
                std::unique_lock lock(mutex_);
                decode_wake_.wait(lock, [this] {
                    if (stop_ || seek_pending_) {
                        return true;
                    }
                    if (decoder_done_) {
                        return false;
                    }
                    const double queued = has_sound_ ? sound_queued_locked() : 0.0;
                    const bool need_picture = frames_.size() < kSoftFrames && queued < kSoundMost;
                    const bool need_sound = has_sound_ && queued < kSoundAhead && frames_.size() < kHardFrames;
                    return need_picture || need_sound;
                });
                if (stop_) {
                    return;
                }
                if (seek_pending_) {
                    seek_pending_ = false;
                    const double target = seek_target_;
                    lock.unlock();
                    d.seek(target);
                    continue;
                }
                generation = generation_;
                view_w = view_width_;
                view_h = view_height_;
            }

            std::vector<VideoFrame> pictures;
            std::vector<SoundChunk> chunks;
            bool at_end = false;
            if (!d.flushed) {
                const int read = av_read_frame(d.input, d.packet);
                if (read < 0) {
                    // The end: what the decoders still hold comes out below.
                    avcodec_send_packet(d.video_decoder, nullptr);
                    if (d.audio_decoder != nullptr) {
                        avcodec_send_packet(d.audio_decoder, nullptr);
                    }
                    d.flushed = true;
                } else {
                    if (d.packet->stream_index == d.video->index) {
                        avcodec_send_packet(d.video_decoder, d.packet);
                    } else if (d.audio != nullptr && d.packet->stream_index == d.audio->index) {
                        avcodec_send_packet(d.audio_decoder, d.packet);
                    }
                    av_packet_unref(d.packet);
                }
            }
            while (avcodec_receive_frame(d.video_decoder, d.frame) == 0) {
                const double pts = d.seconds(d.frame, d.video);
                if (pts + d.half_frame < d.skip_before) {
                    av_frame_unref(d.held);
                    av_frame_move_ref(d.held, d.frame);  // kept in case nothing comes after it
                    d.holding = true;
                    continue;
                }
                pictures.push_back(VideoFrame{pts, generation, d.picture(d.frame, view_w, view_h)});
                d.shown_since_seek = true;
                av_frame_unref(d.frame);
            }
            if (d.audio_decoder != nullptr) {
                while (avcodec_receive_frame(d.audio_decoder, d.frame) == 0) {
                    SoundChunk chunk;
                    chunk.generation = generation;
                    if (d.sound(d.frame, chunk)) {
                        chunks.push_back(std::move(chunk));
                    }
                    av_frame_unref(d.frame);
                }
            }
            if (d.flushed) {
                at_end = true;
                if (!d.shown_since_seek && d.holding) {
                    // A seek to the very end: the last picture there is.
                    pictures.push_back(VideoFrame{d.seconds(d.held, d.video), generation, d.picture(d.held, view_w, view_h)});
                    d.shown_since_seek = true;
                }
            }

            {
                std::lock_guard lock(mutex_);
                if (generation == generation_ && !seek_pending_) {
                    for (VideoFrame& picture : pictures) {
                        if (!picture.image.isNull()) {
                            frames_.push_back(std::move(picture));
                        }
                    }
                    for (SoundChunk& chunk : chunks) {
                        sound_.push_back(std::move(chunk));
                    }
                    if (at_end) {
                        decoder_done_ = true;
                    }
                }
            }
            if (!chunks.empty()) {
                sound_wake_.notify_all();
            }
        }
    } catch (const std::exception& e) {
        log_error(std::string("Clip player stopped: ") + e.what());
        std::lock_guard lock(mutex_);
        error_ = "The clip stopped playing because of an error.";
    }
}

void Player::sound_loop() {
    pa_simple* out = nullptr;
    bool wrote = false;  // something may still be queued in the sound server
    bool failed = false;
    double latency = 0.0;  // seconds the sound server holds before it is heard
    std::uint64_t written_generation = 0;
    std::vector<std::int16_t> samples;
    try {
        while (true) {
            std::unique_lock lock(mutex_);
            // Woken by play, pause, a speed change, a seek, new sound and stop: no polling.
            sound_wake_.wait(lock, [&] {
                return stop_ || (playing_ && !sound_.empty()) ||
                       (wrote && (!playing_ || rate_ != 1.0 || written_generation != generation_));
            });
            if (stop_) {
                break;
            }
            if (playing_ && rate_ != 1.0 && !sound_.empty()) {
                // Another speed plays without sound: let it go, or the decoder (which keeps a few
                // seconds of sound at most) would wait for room forever and the picture freeze.
                sound_.clear();
                lock.unlock();
                decode_wake_.notify_all();
                lock.lock();
            }
            if (!playing_ || rate_ != 1.0 || (wrote && written_generation != generation_)) {
                // Paused, another speed or a seek: what the server still holds must not be heard.
                if (wrote) {
                    lock.unlock();
                    int error = 0;
                    pa_simple_flush(out, &error);
                    wrote = false;
                    latency = 0.0;
                }
                continue;
            }
            if (sound_.empty()) {
                continue;
            }
            if (sound_.front().generation != generation_) {
                sound_.pop_front();
                continue;
            }
            if (out == nullptr) {
                if (failed) {
                    sound_.clear();
                    has_sound_ = false;  // the decoder no longer waits for sound
                    lock.unlock();
                    decode_wake_.notify_all();
                    continue;
                }
                lock.unlock();
                pa_sample_spec spec{};
                spec.format = PA_SAMPLE_S16NE;
                spec.rate = kSoundRate;
                spec.channels = kSoundChannels;
                pa_buffer_attr buffer{};
                buffer.maxlength = static_cast<std::uint32_t>(-1);
                buffer.tlength = static_cast<std::uint32_t>(kSoundBuffer * kSoundRate) * kSoundChannels * 2;
                buffer.prebuf = static_cast<std::uint32_t>(-1);
                buffer.minreq = static_cast<std::uint32_t>(-1);
                buffer.fragsize = static_cast<std::uint32_t>(-1);
                int error = 0;
                out = pa_simple_new(nullptr, "Backtrack", PA_STREAM_PLAYBACK, nullptr, "Clip", &spec, nullptr, &buffer,
                                    &error);
                if (out == nullptr) {
                    log_warning(std::string("No sound for the clip player: ") + pa_strerror(error));
                    failed = true;
                }
                continue;
            }

            SoundChunk& chunk = sound_.front();
            const double length = static_cast<double>(chunk.samples.size()) / (kSoundRate * kSoundChannels);
            const double heard_at = clock_locked() + latency;  // where sound written now is heard
            if (chunk.pts + length < heard_at - kSoundSlack) {
                sound_.pop_front();  // late (the decoder fell behind, or just after a seek)
                lock.unlock();
                decode_wake_.notify_all();
                continue;
            }
            if (chunk.pts > heard_at + kSoundSlack) {
                // Early: the picture has not got there yet.
                const double wait = std::min(chunk.pts - heard_at, 0.02);
                sound_wake_.wait_for(lock, std::chrono::microseconds(static_cast<std::int64_t>(wait * 1e6)));
                continue;
            }
            samples = std::move(chunk.samples);
            if (chunk.pts < heard_at - kSoundSlack) {
                const auto skip = std::min(samples.size(), static_cast<std::size_t>((heard_at - chunk.pts) * kSoundRate) *
                                                               kSoundChannels);
                samples.erase(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(skip));
            }
            const double gain = muted_ ? 0.0 : volume_;
            written_generation = chunk.generation;
            sound_.pop_front();
            lock.unlock();
            decode_wake_.notify_all();

            if (gain < 1.0) {
                for (std::int16_t& sample : samples) {
                    sample = static_cast<std::int16_t>(std::lround(sample * gain));
                }
            }
            int error = 0;
            if (!samples.empty() && pa_simple_write(out, samples.data(), samples.size() * sizeof(std::int16_t), &error) < 0) {
                log_warning(std::string("Clip sound stopped: ") + pa_strerror(error));
                pa_simple_free(out);
                out = nullptr;
                failed = true;
                wrote = false;
                continue;
            }
            wrote = true;
            const pa_usec_t queued = pa_simple_get_latency(out, &error);
            latency = queued == static_cast<pa_usec_t>(-1) ? kSoundBuffer : static_cast<double>(queued) / 1e6;
        }
    } catch (const std::exception& e) {
        log_error(std::string("Clip sound stopped: ") + e.what());
    }
    if (out != nullptr) {
        pa_simple_free(out);
    }
}

}  // namespace bt::linux_app
