#pragma once

// The clip page's "Export...": re-encodes a piece of a clip to an MP4 (H.264 + AAC) or an animated
// GIF, at a chosen size and quality. Unlike the lossless trim (platform/Trim) the cut is exact to
// the frame, and it costs a decode and an encode: a few seconds for a 30 s 1080p60 clip on the
// graphics card, several times that with Windows' software encoder.
//
// The graphics card decodes, scales and encodes when it can (Media Foundation's DXVA decoder and
// video processor, and the card's H.264 encoder through the sink writer); when any of that fails
// the export starts over in software, so it always works, only slower. Fits Discord encodes in
// software on purpose: its size must be exact, and the AMD encoder overshot its bitrate (measured).
//
// GIFs are big and have no sound: a 640 x 360 GIF of 6 s of a game is about 15 MB, four times a
// 720p MP4 of the same piece (measured; see ExportPlan::note for what the dialog should say).
//
// The sizes, bitrates and the "Fits Discord" budget math are in core/ExportPlan.

#include "core/ExportPlan.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace bt {

struct ExportResult {
    bool ok = false;
    bool cancelled = false;
    std::filesystem::path path;  // the finished file (the destination); empty when not ok
    std::uint64_t bytes = 0;
    // One plain-English line for the user, for success ("Saved a 1280 x 720 MP4 of 30 s: 8.9 MB.")
    // and failure ("Could not export: the disk is full.").
    std::string message;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    double seconds = 0.0;  // length of the exported piece
    bool hardware = false;  // MP4: the graphics card's encoder made it; GIF: the graphics card decoded it
};

// Fraction done, 0 to 1, called on the exporting thread a few times per second. A Fits Discord
// export that came out too big encodes a second time; the fraction then starts again from 0.
using ExportProgress = std::function<void(double fraction)>;

// What the export dialog needs for plan_export / estimate_bytes: picture size, frame rate, length,
// sound, bitrate. Blocking but quick (reads the file's header, decodes nothing). Same threading as
// export_clip. Empty when the file cannot be opened or has no video.
std::optional<ExportSource> read_export_source(const std::filesystem::path& clip);

// Blocking: exports `source` to `destination` (which must not exist yet). Call it on a worker
// thread with COM initialised (MTA) and MFStartup done, like trim_clip. Never throws.
//
// The file is written as <destination>.tmp and renamed when it is complete, so a failed or
// cancelled export never leaves a file with the final name; the temporary files are deleted.
// Setting `cancel` stops it within a fraction of a second (result.cancelled).
ExportResult export_clip(const std::filesystem::path& source, const std::filesystem::path& destination,
                         const ExportOptions& options, const ExportProgress& progress, const std::atomic<bool>& cancel);

// A destination in `folder` that is not taken yet: "<stem> (export).mp4", "<stem> (export 2).mp4",
// ... (".gif" for a GIF). Also skips names whose temporary file exists (an export still running).
// Empty when the folder has no free name left (9999 exports of one clip).
std::filesystem::path free_export_path(const std::filesystem::path& folder, const std::wstring& stem,
                                       ExportFormat format);

}  // namespace bt
