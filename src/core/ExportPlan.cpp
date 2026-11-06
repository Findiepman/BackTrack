#include "core/ExportPlan.h"

#include "core/Mp4.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <vector>

namespace bt {
namespace {

constexpr double kDefaultFps = 60.0;
constexpr double kMaxFps = 1000.0;

// The bitrate table is one curve: High at 1080p60 is 16 Mbit/s, and it grows with the pixel
// count to the power 0.75 (bigger pictures compress better per pixel) and with the frame rate to
// the power 0.6 (consecutive frames are more alike at a higher rate). That gives about 8.7 Mbit/s
// at 720p60, 25 at 1440p60, 45 at 4K60 and 10.5 at 1080p30.
constexpr double kReferencePixels = 1920.0 * 1080.0;
constexpr double kReferenceFps = 60.0;
constexpr double kHighReferenceKbps = 16000.0;
constexpr double kPixelExponent = 0.75;
constexpr double kFpsExponent = 0.6;
constexpr double kBalancedShare = 0.5;  // 8 Mbit/s at 1080p60
constexpr double kSmallShare = 0.22;  // 3.5 Mbit/s at 1080p60
constexpr int kMinVideoKbps = 150;
constexpr int kMaxVideoKbps = 100'000;

// Microsoft's AAC encoder only takes 96, 128, 160 and 192 kbit/s.
constexpr int kHighAudioKbps = 192;
constexpr int kBalancedAudioKbps = 160;
constexpr int kSmallAudioKbps = 128;
constexpr int kDiscordAudioKbps = 128;
constexpr int kTightAudioKbps = 96;
constexpr double kMaxAudioShareOfBudget = 0.25;  // above this, Fits Discord takes the lower audio rate

// Fits Discord aims at 92 % of the budget: hardware encoders overshoot a constant bitrate by a few
// percent over a short piece, and the second try costs the user a whole extra encode.
constexpr double kBudgetShare = 0.92;
// The second try aims this much under what the first one's overshoot predicts.
constexpr double kRetryMargin = 0.97;
// A size is good enough for Fits Discord while the bitrate is at least this share of Small there;
// below it the picture turns to blocks, and a smaller picture at a richer bitrate looks better.
constexpr double kDiscordMinShareOfSmall = 0.75;
constexpr int kDiscordMaxFps = 60;
constexpr int kDiscordLowFps = 30;
constexpr int kDiscordSmallestSide = 360;  // only Fits Discord goes below the dialog's 480p
constexpr int kDiscordMinVideoKbps = 120;  // below this even 360p30 is mush: refuse instead
constexpr int kDiscordRungs[] = {1440, 1080, 720, 480, kDiscordSmallestSide};

constexpr double kGifMaxFps = 15.0;
// 640 px on the long side: a GIF costs about a byte per pixel per frame before compression, and
// at 640 x 360 a few seconds already make several MB. Smaller resolutions picked in the dialog
// make it smaller still.
constexpr int kGifMaxLongSide = 640;
// Compressed bytes per pixel per frame with WIC's GIF encoder (a palette per frame, error
// diffusion): 0.71 to 0.74 measured on Backtrack clips of a game, landscape and portrait. Error
// diffusion is what makes GIFs this big (the dither noise does not compress), and without it
// gradients band badly.
constexpr double kGifBytesPerPixelFrame = 0.72;

constexpr std::uint64_t kMp4FixedOverhead = 32 * 1024;
constexpr double kMp4BytesPerVideoFrame = 16.0;  // sample size, time, sync and composition entries
constexpr double kMp4BytesPerAudioFrame = 8.0;
constexpr double kAacFramesPerSecond = 48'000.0 / 1024.0;

constexpr double kBytesPerKilobit = 1000.0 / 8.0;

int round_even(double value) {
    return std::max(2, static_cast<int>(std::llround(value / 2.0)) * 2);
}

int floor_even(int value) {
    return std::max(2, value - value % 2);
}

double source_fps(const ExportSource& source) {
    if (!std::isfinite(source.fps) || source.fps <= 0.0) {
        return kDefaultFps;
    }
    return std::clamp(source.fps, 1.0, kMaxFps);
}

int audio_kbps_for(ExportQuality quality) {
    switch (quality) {
        case ExportQuality::High: return kHighAudioKbps;
        case ExportQuality::Balanced: return kBalancedAudioKbps;
        case ExportQuality::Small: return kSmallAudioKbps;
        case ExportQuality::FitsDiscord: return kDiscordAudioKbps;
    }
    return kBalancedAudioKbps;
}

std::uint64_t payload_bytes(int video_kbps, int audio_kbps, double seconds) {
    return static_cast<std::uint64_t>(std::llround((video_kbps + audio_kbps) * kBytesPerKilobit * seconds));
}

// "10 MB", "8.4 MB": how the dialog names the budget.
std::string megabytes(std::uint64_t bytes) {
    const double mb = static_cast<double>(bytes) / 1'000'000.0;
    if (std::abs(mb - std::round(mb)) < 0.05) {
        return std::format("{:.0f} MB", mb);
    }
    return std::format("{:.1f} MB", mb);
}

std::string minutes_seconds(double seconds) {
    const auto whole = static_cast<long long>(std::max(0.0, std::floor(seconds)));
    return std::format("{}:{:02}", whole / 60, whole % 60);
}

int short_side(ExportSize size) {
    return std::min(size.width, size.height);
}

// Fits Discord for a budget: the largest size whose bitrate still looks right, the full frame
// rate (at most 60) while it fits, 30 fps at the smallest size when even that does not.
void plan_discord(ExportPlan& plan, const ExportOptions& options, const ExportSource& source, double budget,
                  double seconds) {
    const double fps = source_fps(source);
    const bool audio = options.audio && source.has_audio;
    int audio_kbps = 0;
    if (audio) {
        const double share = kDiscordAudioKbps * kBytesPerKilobit * seconds / budget;
        audio_kbps = share > kMaxAudioShareOfBudget ? kTightAudioKbps : kDiscordAudioKbps;
    }
    double out_fps = std::min(fps, static_cast<double>(kDiscordMaxFps));
    const double usable = budget * kBudgetShare - static_cast<double>(mp4_overhead_bytes(seconds, out_fps, audio));
    const double video = usable / kBytesPerKilobit / seconds - audio_kbps;

    const ExportSize wanted = scaled_size(source.width, source.height, options.height);
    std::vector<ExportSize> ladder{wanted};
    for (const int rung : kDiscordRungs) {
        const ExportSize size = scaled_size(source.width, source.height, rung);
        if (short_side(size) < short_side(ladder.back())) {
            ladder.push_back(size);
        }
    }
    const auto fits = [&](ExportSize size, double rate) {
        return video >= quality_video_kbps(size, rate, ExportQuality::Small) * kDiscordMinShareOfSmall;
    };
    std::optional<ExportSize> chosen;
    bool thin = false;
    for (const ExportSize& size : ladder) {
        if (fits(size, out_fps)) {
            chosen = size;
            break;
        }
    }
    if (!chosen) {
        chosen = ladder.back();
        out_fps = std::min(out_fps, static_cast<double>(kDiscordLowFps));
        thin = !fits(*chosen, out_fps);
        if (thin && video < kDiscordMinVideoKbps) {
            const double per_second = (kDiscordMinVideoKbps + audio_kbps) * kBytesPerKilobit;
            const double longest =
                (budget * kBudgetShare - static_cast<double>(mp4_overhead_bytes(seconds, out_fps, audio))) / per_second;
            plan.problem = std::format("{} s is too long to fit in {}. At most about {} fits{}", std::llround(seconds),
                                       megabytes(options.byte_budget), minutes_seconds(std::max(0.0, longest)),
                                       audio ? "; turning the sound off gives a little more" : "");
            return;
        }
    }
    plan.width = chosen->width;
    plan.height = chosen->height;
    plan.fps = out_fps;
    plan.every_frame = out_fps >= fps - 0.01;
    // Capped before the conversion: a piece of a few milliseconds gives a bitrate beyond int.
    int video_kbps = static_cast<int>(
        std::min(video, static_cast<double>(quality_video_kbps(*chosen, out_fps, ExportQuality::High))));
    if (source.video_kbps > 0) {
        video_kbps = std::min(video_kbps, std::max(source.video_kbps, kMinVideoKbps));
    }
    plan.video_kbps = std::max(video_kbps, 1);
    plan.audio_kbps = audio_kbps;
    plan.constant_bitrate = true;
    plan.estimated_bytes = payload_bytes(plan.video_kbps, audio_kbps, seconds) + mp4_overhead_bytes(seconds, out_fps, audio);
    const bool smaller = short_side(*chosen) < short_side(wanted);
    const bool slower = out_fps < std::min(fps, static_cast<double>(kDiscordMaxFps)) - 0.01;
    if (smaller || slower) {
        plan.note = std::format("Lowered to {} x {}{} to fit in {}", plan.width, plan.height,
                                slower ? std::format(" at {:.0f} fps", out_fps) : std::string(),
                                megabytes(options.byte_budget));
    }
    if (thin) {
        plan.note += ". At this length the picture will be blocky; a shorter piece looks better";
    }
}

}  // namespace

ExportSize scaled_size(int source_width, int source_height, int short_side_wanted) {
    if (source_width <= 0 || source_height <= 0) {
        return {};
    }
    const int source_short = std::min(source_width, source_height);
    if (short_side_wanted <= 0 || short_side_wanted >= source_short) {
        return {floor_even(source_width), floor_even(source_height)};
    }
    const double scale = static_cast<double>(short_side_wanted) / source_short;
    if (source_width >= source_height) {
        return {round_even(source_width * scale), floor_even(short_side_wanted)};
    }
    return {floor_even(short_side_wanted), round_even(source_height * scale)};
}

ExportSize fit_long_side(ExportSize size, int max_long_side) {
    const int long_side = std::max(size.width, size.height);
    if (long_side <= max_long_side || long_side <= 0) {
        return size;
    }
    const double scale = static_cast<double>(max_long_side) / long_side;
    return {std::min(round_even(size.width * scale), max_long_side), std::min(round_even(size.height * scale), max_long_side)};
}

int quality_video_kbps(ExportSize size, double fps, ExportQuality quality) {
    double share = 1.0;
    switch (quality) {
        case ExportQuality::High: share = 1.0; break;
        case ExportQuality::Balanced: share = kBalancedShare; break;
        case ExportQuality::Small: share = kSmallShare; break;
        case ExportQuality::FitsDiscord: return 0;
    }
    if (size.width <= 0 || size.height <= 0) {
        return 0;
    }
    const double rate = std::isfinite(fps) && fps > 0.0 ? std::min(fps, kMaxFps) : kDefaultFps;
    const double pixels = static_cast<double>(size.width) * static_cast<double>(size.height);
    const double kbps = kHighReferenceKbps * share * std::pow(pixels / kReferencePixels, kPixelExponent) *
                        std::pow(rate / kReferenceFps, kFpsExponent);
    const auto rounded = static_cast<int>(std::llround(std::min(kbps, static_cast<double>(kMaxVideoKbps)) / 10.0) * 10);
    return std::clamp(rounded, kMinVideoKbps, kMaxVideoKbps);
}

std::uint64_t mp4_overhead_bytes(double seconds, double fps, bool audio) {
    const double s = std::max(0.0, seconds);
    double bytes = static_cast<double>(kMp4FixedOverhead) + s * fps * kMp4BytesPerVideoFrame;
    if (audio) {
        bytes += s * kAacFramesPerSecond * kMp4BytesPerAudioFrame;
    }
    return static_cast<std::uint64_t>(std::llround(bytes));
}

ExportPlan plan_export(const ExportOptions& options, const ExportSource& source) {
    ExportPlan plan;
    plan.format = options.format;
    const double duration = source.duration_s;
    if (!std::isfinite(duration) || duration <= 0.0) {
        plan.problem = "the clip's length is not known";
        return plan;
    }
    if (source.width <= 0 || source.height <= 0) {
        plan.problem = "the clip has no picture";
        return plan;
    }
    const double in = std::isfinite(options.in_seconds) ? std::max(0.0, options.in_seconds) : 0.0;
    const double out = std::isfinite(options.out_seconds) && options.out_seconds > 0.0
                           ? std::min(options.out_seconds, duration)
                           : duration;
    if (in >= duration) {
        plan.problem = "the selection starts after the end of the clip";
        return plan;
    }
    if (out <= in) {
        plan.problem = "the selection is empty";
        return plan;
    }
    plan.start_s = in;
    plan.end_s = out;
    const double seconds = out - in;
    const double fps = source_fps(source);

    if (options.format == ExportFormat::Gif) {
        const ExportSize size = fit_long_side(scaled_size(source.width, source.height, options.height), kGifMaxLongSide);
        plan.width = size.width;
        plan.height = size.height;
        plan.fps = std::min(fps, kGifMaxFps);
        plan.every_frame = fps <= kGifMaxFps + 0.01;
        const double frames = std::max(1.0, std::ceil(seconds * plan.fps));
        plan.estimated_bytes = static_cast<std::uint64_t>(
            std::llround(static_cast<double>(size.width) * size.height * frames * kGifBytesPerPixelFrame));
        plan.note = std::format("GIFs are big and have no sound: about {} for {:.0f} s. An MP4 is far smaller",
                                megabytes(plan.estimated_bytes), std::max(1.0, std::round(seconds)));
        return plan;
    }

    if (options.quality == ExportQuality::FitsDiscord) {
        if (options.byte_budget == 0) {
            plan.problem = "no size limit was given";
            return plan;
        }
        plan_discord(plan, options, source, static_cast<double>(options.byte_budget), seconds);
        return plan;
    }

    const ExportSize size = scaled_size(source.width, source.height, options.height);
    plan.width = size.width;
    plan.height = size.height;
    plan.fps = fps;
    plan.every_frame = true;
    int video_kbps = quality_video_kbps(size, fps, options.quality);
    if (source.video_kbps > 0) {
        // More bits than the clip itself has buy nothing: the detail is not there.
        video_kbps = std::min(video_kbps, std::max(source.video_kbps, kMinVideoKbps));
    }
    plan.video_kbps = video_kbps;
    const bool audio = options.audio && source.has_audio;
    plan.audio_kbps = audio ? audio_kbps_for(options.quality) : 0;
    plan.estimated_bytes = payload_bytes(plan.video_kbps, plan.audio_kbps, seconds) + mp4_overhead_bytes(seconds, fps, audio);
    return plan;
}

ExportPlan plan_budget_retry(const ExportOptions& options, const ExportSource& source, const ExportPlan& first,
                             std::uint64_t actual_bytes) {
    ExportPlan plan = first;
    plan.note.clear();
    const double expected = static_cast<double>(first.estimated_bytes > 0 ? first.estimated_bytes : options.byte_budget);
    const double overshoot = std::max(1.0, static_cast<double>(actual_bytes) / std::max(1.0, expected));
    const double seconds = first.end_s - first.start_s;
    if (options.byte_budget == 0 || seconds <= 0.0) {
        plan.problem = "no size limit was given";
        return plan;
    }
    // The same planning against a smaller budget: if the encoder made 6 % more than asked, the
    // next try asks for 6 % less (and a little more), and a size that gets too thin steps down.
    const double budget = static_cast<double>(options.byte_budget) / overshoot * kRetryMargin;
    ExportOptions narrowed = options;
    if (first.width > 0 && first.height > 0) {
        narrowed.height = short_side(ExportSize{first.width, first.height});  // never larger than the first try
    }
    plan.problem.clear();
    plan_discord(plan, narrowed, source, budget, seconds);
    if (plan.problem.empty()) {
        plan.video_kbps = std::min(plan.video_kbps, std::max(1, first.video_kbps - 1));
        plan.estimated_bytes = payload_bytes(plan.video_kbps, plan.audio_kbps, seconds) +
                               mp4_overhead_bytes(seconds, plan.fps, plan.audio_kbps > 0);
        const bool smaller = short_side(ExportSize{plan.width, plan.height}) <
                             short_side(scaled_size(source.width, source.height, options.height));
        if (smaller && plan.note.empty()) {
            plan.note = std::format("Lowered to {} x {} to fit in {}", plan.width, plan.height,
                                    megabytes(options.byte_budget));
        }
    }
    return plan;
}

bool FrameDecimator::keep(Time100ns since_start) {
    if (!(fps_ > 0.0)) {
        return true;
    }
    // A tenth of a millisecond of slack: 60 fps timestamps are rounded to 100 ns, and a frame that
    // lands a tick before its slot must not be dropped.
    constexpr double kSlackTicks = 1000.0;
    const auto slot = static_cast<std::int64_t>(
        std::floor((static_cast<double>(since_start) + kSlackTicks) * fps_ / static_cast<double>(kTicksPerSecond)));
    if (slot <= last_slot_) {
        return false;
    }
    last_slot_ = slot;
    return true;
}

std::uint16_t gif_delay_cs(Time100ns start, Time100ns next) {
    constexpr Time100ns kTicksPerCentisecond = kTicksPerSecond / 100;
    const auto centiseconds = [](Time100ns t) {
        return (std::max<Time100ns>(t, 0) + kTicksPerCentisecond / 2) / kTicksPerCentisecond;
    };
    const Time100ns delay = centiseconds(next) - centiseconds(start);
    return static_cast<std::uint16_t>(std::clamp<Time100ns>(delay, 2, 65535));
}

PcmStep place_pcm(std::int64_t written, std::int64_t piece_start, std::int64_t piece_frames, std::int64_t end_frames,
                  std::int64_t tolerance) {
    PcmStep step;
    if (piece_frames <= 0 || written >= end_frames) {
        return step;
    }
    std::int64_t start = piece_start;
    if (std::abs(start - written) <= tolerance) {
        start = written;
    }
    if (start > written) {
        step.silence = std::min(start - written, end_frames - written);
    } else if (start < written) {
        step.skip = std::min(written - start, piece_frames);
    }
    const std::int64_t room = end_frames - written - step.silence;
    step.take = std::clamp<std::int64_t>(piece_frames - step.skip, 0, std::max<std::int64_t>(room, 0));
    return step;
}

std::wstring export_file_name(std::wstring_view stem, int n, ExportFormat format) {
    std::wstring name(stem);
    name += n <= 1 ? std::wstring(L" (export)") : L" (export " + std::to_wstring(n) + L")";
    name += format == ExportFormat::Gif ? L".gif" : L".mp4";
    return name;
}

std::vector<TrackKind> mp4_track_kinds(std::span<const std::uint8_t> moov) {
    constexpr std::size_t kHeader = 8;
    constexpr std::size_t kLargeHeader = 16;
    constexpr std::size_t kHandlerTypeAt = 8;  // after version/flags and pre_defined
    bool ok = true;
    // Calls visit(type, body, end) for each box in [begin, end).
    const auto for_each_box = [&](std::size_t begin, std::size_t end, const auto& visit) {
        for (std::size_t at = begin; ok && at < end;) {
            const auto header = mp4::read_box_header(moov.first(end), at, end);
            if (!header || end - at < kHeader) {
                ok = false;
                return;
            }
            const bool large = moov[at] == 0 && moov[at + 1] == 0 && moov[at + 2] == 0 && moov[at + 3] == 1;
            const std::size_t header_size = large ? kLargeHeader : kHeader;
            if (header->size < header_size || header->size > end - at) {
                ok = false;
                return;
            }
            visit(header->type, at + header_size, at + static_cast<std::size_t>(header->size));
            at += static_cast<std::size_t>(header->size);
        }
    };
    std::vector<TrackKind> kinds;
    const auto top = mp4::read_box_header(moov, 0, moov.size());
    if (moov.size() < kHeader || !top || top->type != "moov" || top->size != moov.size()) {
        return kinds;
    }
    const std::size_t body = moov[3] == 1 && moov[0] == 0 && moov[1] == 0 && moov[2] == 0 ? kLargeHeader : kHeader;
    for_each_box(body, moov.size(), [&](const std::string& type, std::size_t begin, std::size_t end) {
        if (type != "trak") return;
        TrackKind kind = TrackKind::Other;
        for_each_box(begin, end, [&](const std::string& trak_child, std::size_t mdia_begin, std::size_t mdia_end) {
            if (trak_child != "mdia") return;
            for_each_box(mdia_begin, mdia_end, [&](const std::string& mdia_child, std::size_t at, std::size_t box_end) {
                if (mdia_child != "hdlr" || box_end - at < kHandlerTypeAt + 4) return;
                const std::string_view handler(reinterpret_cast<const char*>(moov.data() + at + kHandlerTypeAt), 4);
                kind = handler == "vide" ? TrackKind::Video : handler == "soun" ? TrackKind::Audio : TrackKind::Other;
            });
        });
        kinds.push_back(kind);
    });
    if (!ok) {
        kinds.clear();
    }
    return kinds;
}

std::optional<std::size_t> reader_stream_of_first_track(std::span<const TrackKind> file_order,
                                                        std::span<const TrackKind> reader_order, TrackKind kind) {
    const std::size_t n = file_order.size();
    if (n == 0 || reader_order.size() != n) {
        return std::nullopt;
    }
    const auto first = std::find(file_order.begin(), file_order.end(), kind);
    if (first == file_order.end()) {
        return std::nullopt;
    }
    const auto track = static_cast<std::size_t>(first - file_order.begin());
    const bool forward = std::equal(file_order.begin(), file_order.end(), reader_order.begin());
    const bool reversed = std::equal(file_order.rbegin(), file_order.rend(), reader_order.begin());
    if (forward && reversed) {
        // A mirrored layout: a kind that occurs once sits in the middle, where both orders agree.
        if (std::count(file_order.begin(), file_order.end(), kind) != 1) {
            return std::nullopt;
        }
        return track;
    }
    if (forward) {
        return track;
    }
    if (reversed) {
        return n - 1 - track;
    }
    return std::nullopt;
}

}  // namespace bt
