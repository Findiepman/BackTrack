#pragma once

// Everything about an export that can be worked out without Media Foundation: the output size,
// the bitrates per quality, the size estimate the export dialog shows, the "Fits Discord" budget
// (and the second try when the first file came out too big), which frames a GIF keeps and how
// long each one shows. platform/Exporter does the decoding and encoding.

#include "core/Clock.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bt {

enum class ExportFormat {
    Mp4,  // H.264 + AAC
    Gif,  // animated, no sound. Big: several times an MP4 of the same piece (see ExportPlan::note)
};

enum class ExportQuality {
    High,
    Balanced,
    Small,
    FitsDiscord,  // the whole file stays under ExportOptions::byte_budget
};

// Discord's upload limit without Nitro.
inline constexpr std::uint64_t kDiscordFreeUploadBytes = 10'000'000;

struct ExportOptions {
    ExportFormat format = ExportFormat::Mp4;
    // The output's short side: 0 keeps the clip's own size; 1440, 1080, 720 or 480 scale it down
    // (never up). The short side, not the height, so a portrait clip at "720p" is 720 x 1280.
    int height = 0;
    ExportQuality quality = ExportQuality::Balanced;  // MP4 only
    std::uint64_t byte_budget = kDiscordFreeUploadBytes;  // FitsDiscord only
    bool audio = true;  // MP4 only: the clip's first sound track (the mix); GIF never has sound
    double in_seconds = 0.0;  // on the clip's timeline, like the clip page's trim handles
    double out_seconds = 0.0;  // 0: to the end
    bool hardware = true;  // false: Windows' software decoder and encoder only (Settings: encoder = software)
};

// What the plan needs to know about the clip (Exporter's read_export_source fills it in).
struct ExportSource {
    int width = 0;  // the visible picture, not the coded size (1080, not 1088)
    int height = 0;
    double fps = 0.0;  // 0 when the file does not say; taken as 60
    double duration_s = 0.0;  // the whole clip
    bool has_audio = true;
    int video_kbps = 0;  // the clip's own video bitrate; 0 when not known
};

struct ExportSize {
    int width = 0;
    int height = 0;
};

struct ExportPlan {
    std::string problem;  // empty: it can be exported. Otherwise one line for the user
    ExportFormat format = ExportFormat::Mp4;
    double start_s = 0.0;  // the range on the clip's timeline
    double end_s = 0.0;
    int width = 0;  // output picture, always even
    int height = 0;
    double fps = 0.0;  // output frame rate: the clip's own, at most 15 for a GIF, at most 60 for Fits Discord
    bool every_frame = true;  // false: frames are dropped to reach `fps`
    int video_kbps = 0;  // MP4 only
    int audio_kbps = 0;  // 0: no sound
    bool constant_bitrate = false;  // Fits Discord: a predictable size matters more than a few percent of quality
    std::uint64_t estimated_bytes = 0;
    // Something the dialog should say next to the estimate, e.g. that GIFs are big, or that
    // Fits Discord lowered the resolution. Empty when there is nothing to say.
    std::string note;
};

// The plan for these options. The dialog's "Estimated size" line is plan_export(...).estimated_bytes.
ExportPlan plan_export(const ExportOptions& options, const ExportSource& source);

inline std::uint64_t estimate_bytes(const ExportOptions& options, const ExportSource& source) {
    return plan_export(options, source).estimated_bytes;
}

// Fits Discord: the first file came out `actual_bytes` long, more than the budget (the encoder
// overshot). The second and last try aims lower by the same factor, stepping down the resolution
// when the bitrate gets too thin for it.
ExportPlan plan_budget_retry(const ExportOptions& options, const ExportSource& source, const ExportPlan& first,
                             std::uint64_t actual_bytes);

// The picture size for a short side of `short_side` (0: the source's own), keeping the aspect
// ratio, never larger than the source, both sides even (H.264 4:2:0 needs that).
ExportSize scaled_size(int source_width, int source_height, int short_side);

// Shrinks `size` so its long side is at most `max_long_side`, keeping the aspect, both sides even.
ExportSize fit_long_side(ExportSize size, int max_long_side);

// The video bitrate of a quality at this size and frame rate. FitsDiscord has no fixed rate.
int quality_video_kbps(ExportSize size, double fps, ExportQuality quality);

// The MP4 index and box headers: a few bytes per frame plus a fixed part.
std::uint64_t mp4_overhead_bytes(double seconds, double fps, bool audio);

// MP4 time scales round frame times by a tick or two: a frame that ends within half a millisecond
// after `start`, or starts that close before `end`, is the neighbour of the piece, not part of it
// (measured: a 30 fps file gave 451 frames for 15 s without this).
inline constexpr Time100ns kFrameSlack = 5'000;

// Whether a decoded frame belongs to the export: the frame on screen at `start` is the first,
// then every frame that starts before `end`.
inline bool frame_in_range(Time100ns pts, Time100ns duration, Time100ns start, Time100ns end) {
    return pts + kFrameSlack < end && pts + duration > start + kFrameSlack;
}

// Drops frames to reach a lower frame rate: keeps the first frame of every 1/fps slot. Fed the
// frames' times since the start of the export, in order.
class FrameDecimator {
public:
    explicit FrameDecimator(double fps) : fps_(fps) {}
    bool keep(Time100ns since_start);

private:
    double fps_;
    std::int64_t last_slot_ = -1;
};

// How long a GIF frame shows, in hundredths of a second, from its start and the next frame's
// start on the export's timeline. Rounded on the running time, not per frame, so 15 fps comes
// out as 7, 6, 7, 7, 6, ... and a long GIF keeps time. At least 2: browsers show 0 and 1 as 10.
std::uint16_t gif_delay_cs(Time100ns start, Time100ns next);

// Where a piece of decoded sound goes on the export's sound track, in sample frames. `written`
// frames are out already; the piece starts at `piece_start` on the export's timeline (negative:
// before the start) and is `piece_frames` long; the track ends at `end_frames`. A gap is filled
// with silence first so picture and sound keep their offset; an overlap is skipped. Differences
// up to `tolerance` frames are timestamp rounding, not gaps.
struct PcmStep {
    std::int64_t silence = 0;  // frames of silence to write before the piece
    std::int64_t skip = 0;  // frames at the start of the piece to leave out
    std::int64_t take = 0;  // frames of the piece to write after that
};
PcmStep place_pcm(std::int64_t written, std::int64_t piece_start, std::int64_t piece_frames, std::int64_t end_frames,
                  std::int64_t tolerance);

// "<stem> (export).mp4" for n == 1, then "<stem> (export 2).mp4", ...
std::wstring export_file_name(std::wstring_view stem, int n, ExportFormat format);

enum class TrackKind { Video, Audio, Other };

// The kind of every track ('trak' box) in an MP4's moov box (the whole box, header included), in
// the order the file stores them. Empty when the box is not understood.
std::vector<TrackKind> mp4_track_kinds(std::span<const std::uint8_t> moov);

// Which of the source reader's streams is the file's first track of `kind` (for sound: the mix of
// game sound and microphone). Media Foundation does not number its streams in track order: it
// lists an MP4's tracks last to first (measured on Backtrack's own clips and on files written by
// ffmpeg). The order is recognised by comparing the kinds, so a reader that keeps the file's order
// works too. Empty when the two lists do not match either way, or when they match both ways
// (e.g. sound, video, sound) and the track's position is therefore ambiguous.
std::optional<std::size_t> reader_stream_of_first_track(std::span<const TrackKind> file_order,
                                                        std::span<const TrackKind> reader_order, TrackKind kind);

}  // namespace bt
