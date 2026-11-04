#pragma once

// Writes one clip to an MP4 file (§7). Compressed packets are passed through: nothing is
// re-encoded. Runs on the Recorder's writer thread; COM must be initialised there.

#include "core/ClipBuffer.h"
#include "platform/Com.h"

#include <mfidl.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bt {

struct ClipWriteRequest {
    std::filesystem::path path;  // final name; the file is written as <path>.tmp first
    ComPtr<IMFMediaType> video_type;  // the encoder's current output type (carries the sequence header)
    ComPtr<IMFMediaType> audio_type;  // the AAC encoder's output type; null for a clip without sound
    std::vector<Packet> video;
    std::vector<Packet> audio;
};

struct ClipWriteResult {
    bool ok = false;
    std::string error;  // one line, for the user
    double seconds = 0.0;  // real duration of the written clip
    std::uint64_t bytes = 0;
    std::size_t video_frames = 0;
    std::size_t audio_frames = 0;
};

ClipWriteResult write_clip(const ClipWriteRequest& request);

}  // namespace bt
