#pragma once

// Everything about repairing a clip that can be worked out without Media Foundation. "Repair" is
// for clips recorded by other programs (Medal, measured on 217 of a user's 226 GTA clips) that put
// the microphone on a second sound track: Discord and most players play only the first, so the
// voice seems missing. The repaired clip gets a new first track with all of them mixed, followed
// by the original tracks unchanged; the video is copied packet for packet. platform/Repair does
// the reading, decoding and writing.

#include "core/ExportPlan.h"
#include "core/Mp4.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bt {

// The name of the mixed first track, the same one Backtrack's own clips with separate tracks use
// (platform/AudioCapture): a clip whose first track has it is mixed already.
inline constexpr char kMixTrackName[] = "Game + mic";

struct RepairCheck {
    bool needed = false;  // the clip has sound only some players play, and the repair can fix it
    int audio_tracks = 0;
    std::string reason;  // one plain sentence for the user, whether needed or not
};

// Decides from the moov's tracks (in file order, mp4::tracks) whether a clip needs repairing.
// Needed: two or more sound tracks, the first not already a mix, and nothing the repair cannot
// copy (H.264 video without B-frames with 4-byte NAL lengths, AAC sound, edit lists that only shift
// a sound track).
RepairCheck decide_repair(std::span<const mp4::TrackInfo> tracks);

// The mixed track's AAC bitrate in bytes per second (what MF_MT_AUDIO_AVG_BYTES_PER_SECOND takes):
// 160 kbit/s, or the best source track's when that is higher. Microsoft's AAC encoder only takes
// 96, 128, 160 and 192 kbit/s, so a source in between gets the step at or below it.
std::uint32_t mix_aac_bytes_per_second(std::span<const std::uint32_t> source_bytes_per_second);

// The mixed track's sample rate: the first track's when Microsoft's AAC encoder takes it (44.1 or
// 48 kHz), otherwise 48 kHz. Other tracks are resampled to it.
std::uint32_t mix_sample_rate(std::uint32_t first_track_rate);

// The ways the source reader's streams can map onto the file's tracks: each entry gives, for every
// track in file order, the reader stream that delivers it. Media Foundation lists an MP4's tracks
// last to first (measured, see reader_stream_of_first_track), so that order comes first when the
// kinds fit it; in file order second. Usually only one fits; with two (a mirrored layout, or only
// sound tracks) the caller tells them apart by the sample descriptions. Empty when none fits.
std::vector<std::vector<std::size_t>> reader_track_orders(std::span<const TrackKind> file_order,
                                                          std::span<const TrackKind> reader_order);

// The name a clip gets in the backup folder: its own for n == 1, then "<stem> (2)<ext>", ...
std::wstring backup_file_name(std::wstring_view file_name, int n);

// Lines up the decoded sound of several tracks on one timeline (frame 0 = time 0) and sums them.
// Each piece is placed by its timestamp: a timestamp a little off from where the track's sound
// already ends (Medal stamps its 1024-frame AAC packets 1000 or 1100 frames apart, measured, so a
// track strays up to 2 ms from back to back) is taken as back to back; further off, the gap is
// filled with silence or the overlap is cut, as place_pcm does for an export. Medal's sound has
// real gaps of 0.7 to 4 s where the game hung (measured); they stay gaps, in step with the picture.
class TrackMixer {
public:
    TrackMixer(std::size_t tracks, std::uint32_t channels, std::int64_t tolerance_frames);

    // Interleaved samples (full scale = 1) of `track` that start at frame `start` by their timestamp.
    // Sound before frame 0 is left out.
    void add(std::size_t track, std::int64_t start, std::span<const float> samples);
    // No more sound from this track: from where it ends it counts as silence.
    void finish(std::size_t track);

    std::int64_t written(std::size_t track) const { return tracks_[track].written; }
    bool finished(std::size_t track) const { return tracks_[track].finished; }
    // Frames from mixed() on that every track still running has reached; once all have finished,
    // up to where the longest one ends.
    std::int64_t ready() const;
    std::int64_t mixed() const { return mixed_; }
    // Appends the sum of frames [mixed(), ready()) to `out` (interleaved) and lets them go.
    void mix(std::vector<float>& out);

private:
    struct Track {
        std::vector<float> samples;  // frames from mixed_ on, starting at `offset` in the vector
        std::size_t offset = 0;
        std::int64_t written = 0;  // where the track's sound ends on the timeline
        bool finished = false;
    };
    std::vector<Track> tracks_;
    std::uint32_t channels_;
    std::int64_t tolerance_;
    std::int64_t mixed_ = 0;
};

// Keeps the sum of the tracks under full scale without the harsh crackle of clipping. A peak
// limiter: it looks `lookahead` frames ahead and turns the gain down smoothly before a peak that
// would pass the ceiling, then back up over `release` frames. Sound that stays under the ceiling
// passes with a gain of exactly 1, so the game sound keeps its level. A soft knee above the ceiling
// catches anything left (the first frames, rounding), so no sample ever reaches full scale.
class PeakLimiter {
public:
    PeakLimiter(std::uint32_t channels, std::uint32_t lookahead_frames, std::uint32_t release_frames, float ceiling);

    // Takes interleaved frames (full scale = 1) and appends the finished ones to `out` as 16-bit
    // samples. The output trails the input by the look-ahead; flush() hands out the rest.
    void process(std::span<const float> in, std::vector<std::int16_t>& out);
    void flush(std::vector<std::int16_t>& out);

    std::uint64_t limited_frames() const { return limited_frames_; }  // frames turned down, for the log
    float lowest_gain() const { return lowest_gain_; }

private:
    void emit(std::vector<std::int16_t>& out);

    std::uint32_t channels_;
    std::uint32_t lookahead_;
    float release_step_;
    float ceiling_;
    std::deque<float> pending_;  // input frames not yet handed out
    std::uint64_t in_frames_ = 0;
    std::uint64_t out_frames_ = 0;
    std::deque<std::pair<std::uint64_t, float>> needed_;  // (frame, gain it needs); gains increasing
    float gain_ = 1.0f;  // after the release, before smoothing
    std::vector<float> recent_;  // the last lookahead + 1 gains, averaged into the one applied
    std::size_t recent_at_ = 0;
    double recent_sum_ = 0.0;
    std::size_t recent_below_one_ = 0;
    std::uint64_t limited_frames_ = 0;
    float lowest_gain_ = 1.0f;
};

}  // namespace bt
