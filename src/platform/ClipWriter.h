#pragma once

// Writes one clip to an MP4 file (§7). Compressed packets are passed through: nothing is
// re-encoded. Runs on the Recorder's writer thread; COM must be initialised there.

#include "core/ClipBuffer.h"
#include "core/Mp4.h"
#include "platform/Com.h"

#include <mfidl.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bt {

// One sound track of a clip. Separate tracks: the mix first (the one players play), then the
// system sound, then the microphone; otherwise just the mix.
struct ClipAudioTrack {
    ComPtr<IMFMediaType> type;  // the AAC encoder's output type
    std::vector<Packet> packets;
    std::string name;  // what players and editors call the track; empty: Windows' own ("SoundHandler")
};

struct ClipWriteRequest {
    std::filesystem::path path;  // final name; the file is written as <path>.tmp first
    ComPtr<IMFMediaType> video_type;  // the encoder's current output type (carries the sequence header)
    std::vector<Packet> video;
    std::vector<ClipAudioTrack> audio;  // empty for a clip without sound; a track without packets is left out
};

struct ClipWriteResult {
    bool ok = false;
    std::string error;  // one line, for the user
    double seconds = 0.0;  // real duration of the written clip
    std::uint64_t bytes = 0;
    std::size_t video_frames = 0;
    std::size_t audio_frames = 0;  // of the first sound track
    std::size_t audio_tracks = 0;  // sound tracks written
};

ClipWriteResult write_clip(const ClipWriteRequest& request);

// The sound tracks of an MP4 file in file order (mp4::audio_tracks; empty when it cannot be read).
std::vector<mp4::AudioTrackInfo> read_audio_tracks(const std::filesystem::path& path);

}  // namespace bt
