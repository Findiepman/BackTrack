#pragma once

// "Repair" for clips whose microphone is on a second sound track (Medal does that): Discord and
// most players play only the first track, so the voice seems missing. A repaired clip has
//   - the video copied packet for packet: not re-encoded, every timestamp as it was, so a
//     variable frame rate stays exactly as recorded;
//   - as its first sound track a new AAC mix of all the source's sound tracks, named "Game + mic",
//     the one players play;
//   - after it the original sound tracks, unchanged and with their names, marked as alternatives
//     so players do not play them on top of the mix.
// The index is at the front (fast start) and the source's own notes (its udta box) are kept.
//
// The pure parts (which clips need it, the mixing, the limiter) are in core/RepairPlan.

#include "core/RepairPlan.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace bt {

// Quick, nothing is decoded: reads the clip's index (track list, names, formats). Empty when the
// file cannot be read or is not an MP4.
std::optional<RepairCheck> check_for_repair(const std::filesystem::path& clip);

struct RepairResult {
    bool ok = false;
    bool cancelled = false;
    // One plain sentence for the user, for success ("Repaired: the microphone is now in the first
    // sound track.") and failure ("Could not repair the clip: the disk is full.").
    std::string message;
};

// Writes the repaired copy of `source` to `destination`, which must not exist yet. Blocking: call
// it on a worker thread with COM (MTA) initialised and MFStartup done. `progress` (0 to 1, may be
// empty) is called on that thread a few times per second. Never throws. On failure or cancel no
// destination and no temporary file is left behind; the source is only read.
RepairResult repair_clip(const std::filesystem::path& source, const std::filesystem::path& destination,
                         const std::function<void(double)>& progress, const std::atomic<bool>& cancel);

// Where the original goes when the repaired clip takes its place: <clips>\Before repair\<name>, or
// "<stem> (2).mp4" and so on when that name is taken. The folder is not created here. Empty when
// no free name is found.
std::filesystem::path backup_path(const std::filesystem::path& clips_folder, const std::filesystem::path& file_name);

}  // namespace bt
