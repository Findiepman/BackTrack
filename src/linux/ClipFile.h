#pragma once

#include "core/ClipBuffer.h"
#include "core/ClipPlan.h"
#include "linux/VideoEncoder.h"

#include <string>
#include <vector>

namespace bt::linux_app {

// Writes one clip as MP4 with FFmpeg's muxer, in the order and at the times core/ClipPlan worked
// out (the same plan as the Windows writer), with the index at the front (faststart) so it plays
// while it uploads. Written under a hidden temporary name and renamed when complete, so a
// half-written file never shows up as a clip. `audio` may be null (no sound track).
bool write_clip(const std::string& path, const CodecParameters& video, const CodecParameters& audio,
                const std::vector<Packet>& video_packets, const std::vector<Packet>& audio_packets, const ClipPlan& plan,
                std::string& error);

// Gives a finished file written under a temporary name its real name, never replacing a file that
// is already there (a clip saved in the same second, a trim made twice). The temporary file is
// removed either way. False with `error` set when it could not be renamed.
bool publish_file(const std::string& temp_path, const std::string& final_path, std::string& error);

}  // namespace bt::linux_app
