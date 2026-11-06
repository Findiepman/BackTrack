#include "core/RepairPlan.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace bt {
namespace {

// The bitrates Microsoft's AAC encoder takes, in bytes per second (96, 128, 160, 192 kbit/s).
constexpr std::uint32_t kAacRates[] = {12'000, 16'000, 20'000, 24'000};
constexpr std::uint32_t kDefaultMixRate = 20'000;  // 160 kbit/s
// A source that declares a hair under a step still counts as that step: Medal's 192 kbit/s
// tracks declare anything from 191998 to 192014 bit/s (measured).
constexpr double kBitrateSlack = 0.99;
// An AAC track's edit list skips its encoder's first samples (1024 to 2112 frames, under 50 ms);
// skipping more is a trim, which the copy of the track could not keep.
constexpr std::int64_t kMaxSkippedStart = 100 * 10'000;  // 100 ms in 100 ns units
// A track that starts later than this is not a clip's microphone; the silence before it would be
// made in memory at once (10 min of 48 kHz stereo is 220 MB of floats).
constexpr std::int64_t kMaxLateStart = 10ll * 60 * 10'000'000;
// "To the end": place_pcm never stops a track early.
constexpr std::int64_t kNoEnd = std::numeric_limits<std::int64_t>::max() / 4;

bool same_text_ignoring_case(std::string_view a, std::string_view b) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) { return lower(x) == lower(y); });
}

// Media Foundation's name for a track nobody named says nothing to the user.
bool has_name(const std::string& name) {
    return !name.empty() && name != "SoundHandler";
}

std::string quoted(const std::string& name) {
    return has_name(name) ? std::format(" (\"{}\")", name) : std::string();
}

// MP3 in an MP4 has an "mp4a" sample entry too; its esds says what it really is.
bool is_aac(const mp4::TrackInfo& track) {
    constexpr std::uint8_t kMpeg4Audio = 0x40;
    constexpr std::uint8_t kMpeg2AacFirst = 0x66;
    constexpr std::uint8_t kMpeg2AacLast = 0x68;
    return track.codec == "mp4a" && (track.object_type == 0 || track.object_type == kMpeg4Audio ||
                                     (track.object_type >= kMpeg2AacFirst && track.object_type <= kMpeg2AacLast));
}

}  // namespace

RepairCheck decide_repair(std::span<const mp4::TrackInfo> tracks) {
    RepairCheck check;
    std::vector<const mp4::TrackInfo*> audio;
    const mp4::TrackInfo* video = nullptr;
    for (const mp4::TrackInfo& track : tracks) {
        if (track.handler == "soun") {
            audio.push_back(&track);
        } else if (track.handler == "vide" && video == nullptr) {
            video = &track;
        }
    }
    check.audio_tracks = static_cast<int>(audio.size());
    if (video == nullptr) {
        check.reason = "The file has no video.";
        return check;
    }
    if (audio.empty()) {
        check.reason = "The clip has no sound.";
        return check;
    }
    if (audio.size() == 1) {
        check.reason = "The clip has one sound track, which every player plays.";
        return check;
    }
    if (same_text_ignoring_case(audio.front()->name, kMixTrackName)) {
        check.reason = "The first sound track already has the game and the microphone together.";
        return check;
    }
    const auto with_sound = std::count_if(audio.begin() + 1, audio.end(), [](const mp4::TrackInfo* t) { return t->samples > 0; });
    if (with_sound == 0) {
        check.reason = "The other sound tracks are empty, so there is nothing to add.";
        return check;
    }
    if (video->codec != "avc1" && video->codec != "avc3") {
        check.reason = "The video is not H.264, which the repair cannot copy.";
        return check;
    }
    if (video->reordered) {
        check.reason = "The video uses B-frames, which the repair cannot copy.";
        return check;
    }
    // The frames are copied byte for byte, and the MP4 sink always declares 4-byte NAL lengths.
    if (video->nal_length_size != 4) {
        check.reason = "The video is stored in a way the repair cannot copy.";
        return check;
    }
    if (std::any_of(audio.begin(), audio.end(), [](const mp4::TrackInfo* t) { return !is_aac(*t); })) {
        check.reason = "A sound track is not AAC, which the repair cannot mix.";
        return check;
    }
    // Media Foundation reads the packets without their edit lists, so the repair places them by
    // the edit itself: a track that starts late, or skips its encoder's first samples. A video that
    // is shifted, or anything cut and joined, it cannot copy as it is.
    const auto shifted = [](const mp4::TrackInfo* t) {
        return t->samples > 0 &&
               (!t->edits_understood || t->edit_shift < -kMaxSkippedStart || t->edit_shift > kMaxLateStart);
    };
    if (!video->edits_understood || video->edit_shift != 0 || std::any_of(audio.begin(), audio.end(), shifted)) {
        check.reason = "The clip is cut with an edit list, which the repair cannot copy.";
        return check;
    }
    check.needed = true;
    if (audio.size() == 2) {
        check.reason = std::format("Discord and most players only play the first sound track{}, so the second{} is not heard.",
                                   quoted(audio[0]->name), quoted(audio[1]->name));
    } else {
        std::string names;
        for (std::size_t i = 1; i < audio.size(); ++i) {
            if (has_name(audio[i]->name)) {
                names += names.empty() ? "" : ", ";
                names += "\"" + audio[i]->name + "\"";
            }
        }
        check.reason = std::format("Discord and most players only play the first sound track{}, so the other {}{} are not heard.",
                                   quoted(audio[0]->name), audio.size() - 1, names.empty() ? "" : " (" + names + ")");
    }
    return check;
}

std::uint32_t mix_aac_bytes_per_second(std::span<const std::uint32_t> source_bytes_per_second) {
    std::uint32_t best = 0;
    for (const std::uint32_t rate : source_bytes_per_second) {
        best = std::max(best, rate);
    }
    std::uint32_t chosen = kDefaultMixRate;
    for (const std::uint32_t rate : kAacRates) {
        if (rate > chosen && rate * kBitrateSlack <= best) {
            chosen = rate;
        }
    }
    return chosen;
}

std::uint32_t mix_sample_rate(std::uint32_t first_track_rate) {
    return first_track_rate == 44'100 || first_track_rate == 48'000 ? first_track_rate : 48'000;
}

std::vector<std::vector<std::size_t>> reader_track_orders(std::span<const TrackKind> file_order,
                                                          std::span<const TrackKind> reader_order) {
    std::vector<std::vector<std::size_t>> orders;
    const std::size_t n = file_order.size();
    if (n == 0 || reader_order.size() != n) {
        return orders;
    }
    if (std::equal(file_order.rbegin(), file_order.rend(), reader_order.begin())) {
        std::vector<std::size_t>& streams = orders.emplace_back(n);
        for (std::size_t i = 0; i < n; ++i) {
            streams[i] = n - 1 - i;
        }
    }
    if (n > 1 && std::equal(file_order.begin(), file_order.end(), reader_order.begin())) {
        std::vector<std::size_t>& streams = orders.emplace_back(n);
        for (std::size_t i = 0; i < n; ++i) {
            streams[i] = i;
        }
    }
    return orders;
}

std::wstring backup_file_name(std::wstring_view file_name, int n) {
    if (n <= 1) {
        return std::wstring(file_name);
    }
    const std::size_t dot = file_name.rfind(L'.');
    const bool has_extension = dot != std::wstring_view::npos && dot > 0;
    const std::wstring_view stem = has_extension ? file_name.substr(0, dot) : file_name;
    const std::wstring_view extension = has_extension ? file_name.substr(dot) : std::wstring_view();
    return std::wstring(stem) + L" (" + std::to_wstring(n) + L")" + std::wstring(extension);
}

TrackMixer::TrackMixer(std::size_t tracks, std::uint32_t channels, std::int64_t tolerance_frames)
    : tracks_(tracks), channels_(std::max<std::uint32_t>(channels, 1)), tolerance_(std::max<std::int64_t>(tolerance_frames, 0)) {}

void TrackMixer::add(std::size_t track, std::int64_t start, std::span<const float> samples) {
    Track& t = tracks_[track];
    const auto frames = static_cast<std::int64_t>(samples.size() / channels_);
    if (t.finished || frames == 0) {
        return;
    }
    // The first piece is placed exactly: a track that starts late starts late, by any amount.
    const PcmStep step = place_pcm(t.written, start, frames, kNoEnd, t.written == 0 && t.samples.empty() ? 0 : tolerance_);
    t.samples.insert(t.samples.end(), static_cast<std::size_t>(step.silence) * channels_, 0.0f);
    const auto first = samples.begin() + static_cast<std::ptrdiff_t>(step.skip * channels_);
    t.samples.insert(t.samples.end(), first, first + static_cast<std::ptrdiff_t>(step.take * channels_));
    t.written += step.silence + step.take;
}

void TrackMixer::finish(std::size_t track) {
    tracks_[track].finished = true;
}

std::int64_t TrackMixer::ready() const {
    bool running = false;
    std::int64_t slowest = std::numeric_limits<std::int64_t>::max();
    std::int64_t longest = mixed_;
    for (const Track& t : tracks_) {
        if (!t.finished) {
            running = true;
            slowest = std::min(slowest, t.written);
        }
        longest = std::max(longest, t.written);
    }
    return running ? std::max(mixed_, slowest) : longest;
}

void TrackMixer::mix(std::vector<float>& out) {
    const std::int64_t frames = ready() - mixed_;
    if (frames <= 0) {
        return;
    }
    const std::size_t begin = out.size();
    out.resize(begin + static_cast<std::size_t>(frames) * channels_, 0.0f);
    for (Track& t : tracks_) {
        const std::int64_t available = std::clamp<std::int64_t>(t.written - mixed_, 0, frames);
        const std::size_t count = static_cast<std::size_t>(available) * channels_;
        for (std::size_t i = 0; i < count; ++i) {
            out[begin + i] += t.samples[t.offset + i];
        }
        t.offset += count;
        // Let go of what is mixed now and then, not on every call: the front of a vector is costly
        // to erase, and a track that ran ahead (a long gap filled with silence) can hold seconds.
        if (t.offset >= t.samples.size() / 2) {
            t.samples.erase(t.samples.begin(), t.samples.begin() + static_cast<std::ptrdiff_t>(t.offset));
            t.offset = 0;
        }
    }
    mixed_ += frames;
}

PeakLimiter::PeakLimiter(std::uint32_t channels, std::uint32_t lookahead_frames, std::uint32_t release_frames,
                         float ceiling)
    : channels_(std::max<std::uint32_t>(channels, 1)), lookahead_(lookahead_frames),
      release_step_(1.0f / static_cast<float>(std::max<std::uint32_t>(release_frames, 1))),
      ceiling_(std::clamp(ceiling, 0.1f, 0.999f)), recent_(static_cast<std::size_t>(lookahead_frames) + 1, 1.0f),
      recent_sum_(static_cast<double>(recent_.size())) {}

void PeakLimiter::process(std::span<const float> in, std::vector<std::int16_t>& out) {
    const std::size_t frames = in.size() / channels_;
    for (std::size_t f = 0; f < frames; ++f) {
        float peak = 0.0f;
        for (std::uint32_t c = 0; c < channels_; ++c) {
            float x = in[f * channels_ + c];
            if (!std::isfinite(x)) {
                x = 0.0f;  // a damaged packet must not turn into a full-scale burst
            }
            peak = std::max(peak, std::abs(x));
            pending_.push_back(x);
        }
        const float need = peak > ceiling_ ? ceiling_ / peak : 1.0f;
        while (!needed_.empty() && needed_.back().second >= need) {
            needed_.pop_back();
        }
        needed_.emplace_back(in_frames_, need);
        ++in_frames_;
        if (in_frames_ - out_frames_ > lookahead_) {
            emit(out);
        }
    }
}

void PeakLimiter::flush(std::vector<std::int16_t>& out) {
    while (out_frames_ < in_frames_) {
        emit(out);
    }
}

void PeakLimiter::emit(std::vector<std::int16_t>& out) {
    // The lowest gain any frame from this one to the end of the look-ahead needs. The newest frame
    // is never dropped from the queue, so it is not empty.
    while (needed_.front().first < out_frames_) {
        needed_.pop_front();
    }
    gain_ = std::min({needed_.front().second, gain_ + release_step_, 1.0f});
    // The applied gain is the average of the last lookahead + 1 gains: it slides down over the
    // look-ahead instead of jumping (a jump in gain is a click), and still reaches what a peak
    // needs by the time the peak is played, since every gain in that window already counts it.
    const float old = recent_[recent_at_];
    if (old < 1.0f) {
        --recent_below_one_;
    }
    if (gain_ < 1.0f) {
        ++recent_below_one_;
    }
    recent_sum_ += static_cast<double>(gain_) - static_cast<double>(old);
    recent_[recent_at_] = gain_;
    recent_at_ = (recent_at_ + 1) % recent_.size();
    // Exactly 1 whenever no gain in the window is below it, so the running sum's rounding never
    // touches sound that needs no limiting.
    const float gain =
        recent_below_one_ == 0 ? 1.0f : std::min(1.0f, static_cast<float>(recent_sum_ / static_cast<double>(recent_.size())));
    if (gain < 1.0f) {
        ++limited_frames_;
        lowest_gain_ = std::min(lowest_gain_, gain);
    }
    for (std::uint32_t c = 0; c < channels_; ++c) {
        float y = pending_.front() * gain;
        pending_.pop_front();
        const float level = std::abs(y);
        if (level > ceiling_) {
            const float over = (level - ceiling_) / (1.0f - ceiling_);
            y = std::copysign(ceiling_ + (1.0f - ceiling_) * std::tanh(over), y);
        }
        const long value = std::lround(y * 32768.0f);
        out.push_back(static_cast<std::int16_t>(std::clamp<long>(value, -32768, 32767)));
    }
    ++out_frames_;
}

}  // namespace bt
