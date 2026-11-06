#include "platform/Repair.h"

#include "core/H264.h"
#include "core/Mp4.h"
#include "platform/ClipWriter.h"
#include "platform/Com.h"
#include "platform/Log.h"
#include "platform/Text.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <span>
#include <vector>

namespace bt {
namespace {

// ReadSample can hand out gaps and stream ticks without a sample; a file that only does that is
// broken, and the loop must still end.
constexpr int kMaxEmptyReads = 1000;
constexpr std::uint64_t kMaxMoovBytes = 64ull * 1024 * 1024;
constexpr std::size_t kLargestBoxHeader = 16;
constexpr UINT32 kAacPayloadRaw = 0;
constexpr UINT32 kAacLcLevel = 0x29;  // AAC Low Complexity, the only profile Microsoft's encoder makes
constexpr UINT32 kPcmBits = 16;  // what Microsoft's AAC encoder takes
constexpr UINT32 kFloatBits = 32;
// A timestamp up to 20 ms off from where a track's sound ends is taken as back to back: Medal's
// stray up to 2 ms (measured), and its real gaps are 0.7 s and longer. Less than one AAC frame
// (21 ms), so a single lost packet still becomes silence and the sound stays in step.
constexpr std::int64_t kToleranceMs = 20;
// The limiter (core/RepairPlan): -0.5 dBFS, a 2 ms look-ahead, back to full level over 100 ms.
constexpr float kCeiling = 0.944f;
constexpr std::int64_t kLookaheadMs = 2;
constexpr std::int64_t kReleaseMs = 100;
constexpr auto kProgressInterval = std::chrono::milliseconds(100);
constexpr int kMaxFreeNames = 9999;
// The repair writes the file twice (as the sink writer makes it, then with the index moved to the
// front) before the first copy is deleted, so it needs twice the clip's size, and some room.
constexpr std::uint64_t kSpareBytes = 64ull * 1024 * 1024;

constexpr wchar_t kTempSuffix[] = L".tmp";
constexpr wchar_t kFastSuffix[] = L".fast.tmp";

// A problem the user can understand: shown as it is. Anything else (a failed HRESULT) is logged
// in full and reported with a general line.
class RepairError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Cancelled {};

std::filesystem::path with_suffix(const std::filesystem::path& path, const wchar_t* suffix) {
    std::filesystem::path out = path;
    out += suffix;
    return out;
}

// Deletes the repair's temporary files when it ends, however it ends. Declared before the Media
// Foundation objects that hold the files open, so those are released first.
class TempFiles {
public:
    explicit TempFiles(std::vector<std::filesystem::path> paths) : paths_(std::move(paths)) {}
    ~TempFiles() {
        for (const std::filesystem::path& path : paths_) {
            DeleteFileW(path.c_str());
        }
    }
    TempFiles(const TempFiles&) = delete;
    TempFiles& operator=(const TempFiles&) = delete;

private:
    std::vector<std::filesystem::path> paths_;
};

bool disk_full(HRESULT hr) {
    return hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL) || hr == HRESULT_FROM_WIN32(ERROR_HANDLE_DISK_FULL);
}

// Timestamps come from the file's own tables. A damaged one far past the clip would make the mix
// fill hours of silence in memory, or overflow the frame arithmetic (time * sample rate).
void check_time(Time100ns time, Time100ns latest) {
    if (time < -kTicksPerSecond || time > latest) {
        throw RepairError("a packet in the clip has a timestamp outside it");
    }
}

// The latest believable timestamp: a minute past the clip's stated length (never more than a day).
Time100ns latest_time(Time100ns duration) {
    constexpr Time100ns kDay = 24ll * 3600 * kTicksPerSecond;
    return duration > 0 ? std::min(kDay, duration + 60 * kTicksPerSecond) : kDay;
}

void check_cancel(const std::atomic<bool>& cancel) {
    if (cancel.load(std::memory_order_relaxed)) {
        throw Cancelled{};
    }
}

// The moov box (the index) of an MP4 file; empty when the file is not one or cannot be read.
std::optional<std::vector<std::uint8_t>> read_moov(const std::filesystem::path& file) {
    std::error_code ec;
    const std::uint64_t file_size = std::filesystem::file_size(file, ec);
    if (ec) {
        return std::nullopt;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    for (std::uint64_t at = 0; at < file_size;) {
        std::uint8_t header[kLargestBoxHeader] = {};
        const auto available = static_cast<std::size_t>(std::min<std::uint64_t>(sizeof(header), file_size - at));
        in.seekg(static_cast<std::streamoff>(at));
        in.read(reinterpret_cast<char*>(header), static_cast<std::streamsize>(available));
        const auto box = mp4::read_box_header(std::span<const std::uint8_t>(header, available), 0, file_size - at);
        if (!in || !box || box->size > file_size - at) {
            return std::nullopt;
        }
        if (box->type == "moov") {
            if (box->size > kMaxMoovBytes) {
                return std::nullopt;
            }
            std::vector<std::uint8_t> moov(static_cast<std::size_t>(box->size));
            in.seekg(static_cast<std::streamoff>(at));
            in.read(reinterpret_cast<char*>(moov.data()), static_cast<std::streamsize>(moov.size()));
            if (!in) {
                return std::nullopt;
            }
            return moov;
        }
        at += box->size;
    }
    return std::nullopt;
}

// The attributes the MP4 sink needs, copied from the source's native types (the same lists as
// Trim's). Not the whole type: the source adds its own MP4 sample description
// (MF_MT_MPEG4_SAMPLE_DESCRIPTION), which describes the old file, not the one being written.
const GUID kVideoKeys[] = {
    MF_MT_MAJOR_TYPE, MF_MT_SUBTYPE, MF_MT_FRAME_SIZE, MF_MT_FRAME_RATE, MF_MT_PIXEL_ASPECT_RATIO,
    MF_MT_INTERLACE_MODE, MF_MT_AVG_BITRATE, MF_MT_MPEG2_PROFILE, MF_MT_MPEG2_LEVEL, MF_MT_MPEG_SEQUENCE_HEADER,
    MF_MT_VIDEO_PRIMARIES, MF_MT_TRANSFER_FUNCTION, MF_MT_YUV_MATRIX, MF_MT_VIDEO_NOMINAL_RANGE,
};
const GUID kAudioKeys[] = {
    MF_MT_MAJOR_TYPE, MF_MT_SUBTYPE, MF_MT_AUDIO_BITS_PER_SAMPLE, MF_MT_AUDIO_SAMPLES_PER_SECOND,
    MF_MT_AUDIO_NUM_CHANNELS, MF_MT_AUDIO_AVG_BYTES_PER_SECOND, MF_MT_AUDIO_BLOCK_ALIGNMENT, MF_MT_AUDIO_CHANNEL_MASK,
    MF_MT_AAC_PAYLOAD_TYPE, MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, MF_MT_USER_DATA,
};

ComPtr<IMFMediaType> copy_keys(IMFMediaType* native, std::span<const GUID> keys) {
    ComPtr<IMFMediaType> type;
    BT_CHECK_HR(MFCreateMediaType(&type));
    for (const GUID& key : keys) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(native->GetItem(key, &value))) {
            const HRESULT hr = type->SetItem(key, value);
            PropVariantClear(&value);
            BT_CHECK_HR(hr);
        }
    }
    return type;
}

std::vector<std::uint8_t> blob(IMFAttributes* attributes, const GUID& key) {
    UINT32 size = 0;
    std::vector<std::uint8_t> bytes;
    if (SUCCEEDED(attributes->GetBlobSize(key, &size)) && size > 0) {
        bytes.resize(size);
        if (FAILED(attributes->GetBlob(key, bytes.data(), size, nullptr))) {
            bytes.clear();
        }
    }
    return bytes;
}

ComPtr<IMFSourceReader> open_reader(const std::filesystem::path& file) {
    ComPtr<IMFSourceReader> reader;
    const HRESULT hr = MFCreateSourceReaderFromURL(file.c_str(), nullptr, &reader);
    if (FAILED(hr)) {
        log::warn("Repair: cannot open {}: {}", to_utf8(file.filename().wstring()), hresult_text(hr));
        throw RepairError("the clip could not be opened");
    }
    return reader;
}

// One sound track of the source that goes into the repaired clip.
struct SourceAudio {
    DWORD stream = 0;  // the reader's stream
    std::string name;
    ComPtr<IMFMediaType> type;  // for the copy
    UINT32 rate = 0;
    UINT32 channels = 0;
    Time100ns shift = 0;  // the edit list's, which Media Foundation leaves out (mp4::TrackInfo::edit_shift)
};

struct SourceLayout {
    DWORD video = 0;
    std::size_t video_track = 0;  // in file order, for its sample tables
    ComPtr<IMFMediaType> video_type;
    std::vector<SourceAudio> audio;  // in file order, empty tracks left out
    Time100ns duration = 0;
};

// Which reader stream is which track of the file. The kinds narrow it down to one or two orders
// (core/RepairPlan); where a stream's sample description is known, it must be the track's own.
SourceLayout find_layout(IMFSourceReader* reader, const std::vector<mp4::TrackInfo>& tracks) {
    std::vector<TrackKind> reader_kinds;
    std::vector<ComPtr<IMFMediaType>> natives;
    for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> native;
        const HRESULT hr = reader->GetNativeMediaType(index, 0, &native);
        if (hr == MF_E_INVALIDSTREAMNUMBER) {
            break;
        }
        BT_CHECK_HR(hr);
        GUID major{};
        BT_CHECK_HR(native->GetGUID(MF_MT_MAJOR_TYPE, &major));
        reader_kinds.push_back(major == MFMediaType_Video   ? TrackKind::Video
                               : major == MFMediaType_Audio ? TrackKind::Audio
                                                            : TrackKind::Other);
        natives.push_back(native);
    }
    std::vector<TrackKind> file_kinds;
    for (const mp4::TrackInfo& track : tracks) {
        file_kinds.push_back(track.handler == "vide"   ? TrackKind::Video
                             : track.handler == "soun" ? TrackKind::Audio
                                                       : TrackKind::Other);
    }
    std::optional<std::vector<std::size_t>> order;
    for (const std::vector<std::size_t>& candidate : reader_track_orders(file_kinds, reader_kinds)) {
        bool fits = true;
        for (std::size_t t = 0; t < tracks.size() && fits; ++t) {
            const std::vector<std::uint8_t> described = blob(natives[candidate[t]].Get(), MF_MT_MPEG4_SAMPLE_DESCRIPTION);
            fits = described.empty() || tracks[t].sample_description.empty() || described == tracks[t].sample_description;
        }
        if (fits) {
            order = candidate;
            break;
        }
    }
    if (!order) {
        log::warn("Repair: the reader's {} streams do not match the file's {} tracks", reader_kinds.size(), tracks.size());
        throw RepairError("the tracks of the clip could not be told apart");
    }

    SourceLayout layout;
    bool have_video = false;
    for (std::size_t t = 0; t < tracks.size(); ++t) {
        const auto stream = static_cast<DWORD>((*order)[t]);
        IMFMediaType* native = natives[stream].Get();
        GUID subtype{};
        BT_CHECK_HR(native->GetGUID(MF_MT_SUBTYPE, &subtype));
        if (file_kinds[t] == TrackKind::Video && !have_video) {
            if (subtype != MFVideoFormat_H264) {
                throw RepairError("the video is not H.264");
            }
            have_video = true;
            layout.video = stream;
            layout.video_track = t;
            layout.video_type = copy_keys(native, kVideoKeys);
            // The MP4 sink makes the video's timescale 1000 times the frame rate it is told (measured:
            // 19472 for a Medal clip's 19.47 fps), which moved every timestamp of a variable frame
            // rate clip by up to 51 us. A "frame rate" of the source's timescale / 1000 gives its own
            // timescale back (90000 for Medal), and every timestamp is written as it was (measured).
            // MP4 stores no frame rate of its own, so nothing else changes.
            if (tracks[t].timescale >= 1000) {
                BT_CHECK_HR(MFSetAttributeRatio(layout.video_type.Get(), MF_MT_FRAME_RATE, tracks[t].timescale, 1000));
            }
        } else if (file_kinds[t] == TrackKind::Audio && tracks[t].samples > 0) {
            if (subtype != MFAudioFormat_AAC) {
                throw RepairError("a sound track is not AAC");
            }
            SourceAudio audio;
            audio.stream = stream;
            audio.name = tracks[t].name;
            audio.type = copy_keys(native, kAudioKeys);
            audio.rate = MFGetAttributeUINT32(native, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
            audio.channels = MFGetAttributeUINT32(native, MF_MT_AUDIO_NUM_CHANNELS, 0);
            audio.shift = tracks[t].edit_shift;
            layout.audio.push_back(std::move(audio));
        }
    }
    if (!have_video) {
        throw RepairError("the clip has no video");
    }
    if (layout.audio.empty()) {
        throw RepairError("the clip has no sound");
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION,
                                                   &value))) {
        ULONGLONG duration = 0;
        if (SUCCEEDED(PropVariantToUInt64(value, &duration))) {
            layout.duration = static_cast<Time100ns>(duration);
        }
        PropVariantClear(&value);
    }
    return layout;
}

ComPtr<IMFMediaType> pcm_type(const GUID& subtype, UINT32 bits, UINT32 rate, UINT32 channels) {
    ComPtr<IMFMediaType> type;
    BT_CHECK_HR(MFCreateMediaType(&type));
    BT_CHECK_HR(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    BT_CHECK_HR(type->SetGUID(MF_MT_SUBTYPE, subtype));
    BT_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bits));
    BT_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate));
    BT_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels));
    BT_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * bits / 8));
    BT_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * channels * bits / 8));
    BT_CHECK_HR(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    return type;
}

// Decodes one sound track to float PCM at the mix's rate and channel count. Its own reader with
// only that stream selected, so each track is read at its own pace and none queues up behind
// another. When the track's rate differs, the source reader puts Media Foundation's resampler in.
class TrackDecoder {
public:
    TrackDecoder(const std::filesystem::path& file, DWORD stream, Time100ns shift, Time100ns latest, UINT32 rate,
                 UINT32 channels)
        : reader_(open_reader(file)), stream_(stream), shift_(shift), latest_(latest), rate_(rate), channels_(channels) {
        BT_CHECK_HR(reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
        BT_CHECK_HR(reader_->SetStreamSelection(stream_, TRUE));
        ComPtr<IMFMediaType> type = pcm_type(MFAudioFormat_Float, kFloatBits, rate, channels);
        const HRESULT hr = reader_->SetCurrentMediaType(stream_, nullptr, type.Get());
        if (FAILED(hr)) {
            log::warn("Repair: stream {} cannot be decoded to {} Hz {} channel float: {}", stream_, rate, channels,
                      hresult_text(hr));
            throw RepairError("the clip's sound cannot be decoded");
        }
    }

    // Reads the next piece into `samples` (interleaved) and its first frame on the mix's timeline
    // into `start`. False at the end of the track.
    bool read(std::vector<float>& samples, std::int64_t& start) {
        for (int empty = 0; empty <= kMaxEmptyReads; ++empty) {
            DWORD index = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            BT_CHECK_HR(reader_->ReadSample(stream_, 0, &index, &flags, &timestamp, &sample));
            if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
                throw std::runtime_error("the source reader reported a stream error while decoding");
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                return false;
            }
            LONGLONG pts = 0;
            if (!sample || FAILED(sample->GetSampleTime(&pts))) {
                continue;
            }
            check_time(pts, latest_);
            ComPtr<IMFMediaBuffer> buffer;
            BT_CHECK_HR(sample->ConvertToContiguousBuffer(&buffer));
            DWORD length = 0;
            BT_CHECK_HR(buffer->GetCurrentLength(&length));
            // Sized before the buffer is locked: a throw while it is locked would leave it locked.
            samples.resize(length / sizeof(float) / channels_ * channels_);
            BYTE* data = nullptr;
            BT_CHECK_HR(buffer->Lock(&data, nullptr, &length));
            const std::size_t count = std::min<std::size_t>(samples.size(), length / sizeof(float));
            if (count > 0) {
                std::memcpy(samples.data(), data, count * sizeof(float));
            }
            BT_CHECK_HR(buffer->Unlock());
            samples.resize(count / channels_ * channels_);
            // pts * rate stays far inside int64 for any clip (10^13 * 48000 < 9 * 10^18).
            const std::int64_t scaled = (pts + shift_) * static_cast<std::int64_t>(rate_);
            start = (scaled >= 0 ? scaled + kTicksPerSecond / 2 : scaled - kTicksPerSecond / 2) / kTicksPerSecond;
            return true;
        }
        throw std::runtime_error("the source reader returns no decoded sound");
    }

private:
    ComPtr<IMFSourceReader> reader_;
    DWORD stream_;
    Time100ns shift_;
    Time100ns latest_;
    UINT32 rate_;
    UINT32 channels_;
};

// The video's own bytes, read from the file with the moov's sample tables, to keep its parameter
// sets (SPS/PPS) right. Media Foundation's reader hands H.264 out as Annex B with the avcC's SPS/PPS
// put in front of each keyframe that has none, and the MP4 sink takes SPS/PPS out of every frame
// and stores only the ones of the type's sequence header (the avcC). 9 of 38 Medal clips tried
// carry their own SPS/PPS in the first keyframe, different from their avcC (measured: the avcC's
// SPS lacks the colour and timing fields); decoders use those from that keyframe on. So the
// repaired clip's avcC gets the first keyframe's own SPS/PPS, which leaves every frame decoding
// exactly as before (measured: the same decoded pictures), and a clip whose later keyframes bring
// other ones is refused, since the sink would drop them. (The sink's
// MF_MPEG4SINK_SPSPPS_PASSTHROUGH, which would keep them in the frames, fails on length-prefixed
// frames with E_UNEXPECTED and writes Annex B ones unreadable; measured.)
class VideoBytes {
public:
    VideoBytes(const std::filesystem::path& file, std::vector<mp4::SampleLocation> locations)
        : in_(file, std::ios::binary), locations_(std::move(locations)) {
        if (!in_) {
            throw RepairError("the clip could not be read");
        }
    }

    std::size_t size() const { return locations_.size(); }

    // The parameter sets of frame `index` as the file stores it (in the form of
    // MF_MT_MPEG_SEQUENCE_HEADER), empty when it has none. With the reader's frame `decoded`, also
    // checks that it is that frame (the same last bytes), so the index and the reader agree.
    std::vector<std::uint8_t> parameter_sets(std::size_t index, IMFSample* decoded) {
        const mp4::SampleLocation& at = locations_[index];
        // No video frame is this big (a 4K keyframe is a few MB); a damaged index must not make
        // this allocate gigabytes.
        constexpr std::uint32_t kMaxFrameBytes = 64u * 1024 * 1024;
        if (at.size > kMaxFrameBytes) {
            throw RepairError("the video frames do not match the clip's index");
        }
        bytes_.resize(at.size);
        in_.seekg(static_cast<std::streamoff>(at.offset));
        in_.read(reinterpret_cast<char*>(bytes_.data()), static_cast<std::streamsize>(bytes_.size()));
        const auto units = in_ ? h264::split_length_prefixed(bytes_) : std::nullopt;
        if (!units || units->empty()) {
            in_.clear();
            throw RepairError("the video frames do not match the clip's index");
        }
        if (decoded != nullptr) {
            const std::span<const std::uint8_t> last = units->back().payload;
            const std::size_t tail = std::min<std::size_t>(last.size(), kTailBytes);
            ComPtr<IMFMediaBuffer> buffer;
            BT_CHECK_HR(decoded->ConvertToContiguousBuffer(&buffer));
            BYTE* data = nullptr;
            DWORD length = 0;
            BT_CHECK_HR(buffer->Lock(&data, nullptr, &length));
            const bool same =
                length >= tail && std::equal(last.end() - static_cast<std::ptrdiff_t>(tail), last.end(), data + (length - tail));
            BT_CHECK_HR(buffer->Unlock());
            if (!same) {
                throw RepairError("the video frames do not match the clip's index");
            }
        }
        return h264::parameter_set_bytes(*units);
    }

private:
    static constexpr std::size_t kTailBytes = 16;
    std::ifstream in_;
    std::vector<mp4::SampleLocation> locations_;
    std::vector<std::uint8_t> bytes_;
};

// Calls the caller's progress callback at most every 100 ms, always with a rising fraction.
class ProgressReporter {
public:
    ProgressReporter(const std::function<void(double)>& progress, Time100ns length)
        : progress_(progress), length_(length) {}
    void at(Time100ns time) {
        if (!progress_ || length_ <= 0) return;
        // Copying the clip is 90 %; moving the index to the front, a second copy of the file
        // that takes about a tenth as long (no decoding), is the rest.
        constexpr double kCopyShare = 0.9;
        const double fraction = std::clamp(static_cast<double>(time) / static_cast<double>(length_), 0.0, 1.0) * kCopyShare;
        const auto now = std::chrono::steady_clock::now();
        if (fraction > last_ && now - last_call_ >= kProgressInterval) {
            last_ = fraction;
            last_call_ = now;
            progress_(fraction);
        }
    }
    void done() {
        if (progress_) progress_(1.0);
    }

private:
    const std::function<void(double)>& progress_;
    Time100ns length_;
    double last_ = -1.0;
    std::chrono::steady_clock::time_point last_call_{};
};

// Decodes every sound track, mixes them and hands the mix to the sink writer's AAC encoder, as far
// as the copied packets have got: the sink writer interleaves the tracks, so the mix keeps up with
// them instead of waiting in memory until the end.
class MixWriter {
public:
    MixWriter(const std::filesystem::path& file, const std::vector<SourceAudio>& audio, IMFSinkWriter* writer,
              DWORD stream, Time100ns latest, UINT32 rate, UINT32 channels)
        : writer_(writer), stream_(stream), rate_(rate), channels_(channels),
          mixer_(audio.size(), channels, rate * kToleranceMs / 1000),
          limiter_(channels, static_cast<std::uint32_t>(rate * kLookaheadMs / 1000),
                   static_cast<std::uint32_t>(rate * kReleaseMs / 1000), kCeiling) {
        for (const SourceAudio& track : audio) {
            decoders_.emplace_back(file, track.stream, track.shift, latest, rate, channels);
        }
    }

    // Mixes everything up to `time` on the clip's timeline.
    void advance_to(Time100ns time, const std::atomic<bool>& cancel) {
        const std::int64_t target = time * static_cast<std::int64_t>(rate_) / kTicksPerSecond;
        while (mixer_.ready() < target && read_behind(target)) {
            check_cancel(cancel);
        }
        write_ready(false);
    }

    // Reads every track to its end and writes the rest of the mix.
    void finish(const std::atomic<bool>& cancel) {
        while (read_behind(std::numeric_limits<std::int64_t>::max())) {
            check_cancel(cancel);
        }
        write_ready(true);
    }

    const PeakLimiter& limiter() const { return limiter_; }
    std::int64_t frames_written() const { return written_; }

private:
    // Reads one piece of every running track that has not reached `target`. False when no track
    // is left to read.
    bool read_behind(std::int64_t target) {
        bool any = false;
        for (std::size_t t = 0; t < decoders_.size(); ++t) {
            if (mixer_.finished(t) || mixer_.written(t) >= target) {
                continue;
            }
            any = true;
            std::int64_t start = 0;
            if (decoders_[t].read(piece_, start)) {
                mixer_.add(t, start, piece_);
            } else {
                mixer_.finish(t);
            }
        }
        return any;
    }

    void write_ready(bool last) {
        mixed_.clear();
        mixer_.mix(mixed_);
        pcm_.clear();
        limiter_.process(mixed_, pcm_);
        if (last) {
            limiter_.flush(pcm_);
        }
        const auto frames = static_cast<std::int64_t>(pcm_.size() / channels_);
        if (frames == 0) {
            return;
        }
        const auto bytes = static_cast<DWORD>(pcm_.size() * sizeof(std::int16_t));
        ComPtr<IMFMediaBuffer> buffer;
        BT_CHECK_HR(MFCreateMemoryBuffer(bytes, &buffer));
        BYTE* out = nullptr;
        BT_CHECK_HR(buffer->Lock(&out, nullptr, nullptr));
        std::memcpy(out, pcm_.data(), bytes);
        BT_CHECK_HR(buffer->Unlock());
        BT_CHECK_HR(buffer->SetCurrentLength(bytes));
        ComPtr<IMFSample> sample;
        BT_CHECK_HR(MFCreateSample(&sample));
        BT_CHECK_HR(sample->AddBuffer(buffer.Get()));
        // Times from the frame count, never a running sum of durations, so the mix cannot drift.
        const Time100ns time = written_ * kTicksPerSecond / rate_;
        const Time100ns next = (written_ + frames) * kTicksPerSecond / rate_;
        BT_CHECK_HR(sample->SetSampleTime(time));
        BT_CHECK_HR(sample->SetSampleDuration(next - time));
        BT_CHECK_HR(writer_->WriteSample(stream_, sample.Get()));
        written_ += frames;
    }

    IMFSinkWriter* writer_;
    DWORD stream_;
    UINT32 rate_;
    UINT32 channels_;
    std::vector<TrackDecoder> decoders_;
    TrackMixer mixer_;
    PeakLimiter limiter_;
    std::vector<float> piece_;
    std::vector<float> mixed_;
    std::vector<std::int16_t> pcm_;
    std::int64_t written_ = 0;  // frames of the mix handed to the encoder
};

std::string lower_first(std::string text) {
    if (!text.empty() && text.front() >= 'A' && text.front() <= 'Z') {
        text.front() = static_cast<char>(text.front() - 'A' + 'a');
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text;
}

RepairResult repair_unguarded(const std::filesystem::path& source, const std::filesystem::path& relative_destination,
                              const std::function<void(double)>& progress, const std::atomic<bool>& cancel) {
    RepairResult result;
    const auto fail = [&](const std::string& why) {
        result.message = "Could not repair the clip: " + why + ".";
        return result;
    };
    const std::string name = to_utf8(source.filename().wstring());
    std::error_code ec;
    // A bare file name has no parent folder to check or to ask for free space.
    const std::filesystem::path destination = std::filesystem::absolute(relative_destination, ec);
    if (ec) {
        return fail("the name for the repaired clip is not valid");
    }
    // Also what keeps the source from being written over: it exists.
    if (std::filesystem::exists(destination, ec)) {
        return fail("a file with that name already exists");
    }
    if (!std::filesystem::is_directory(destination.parent_path(), ec)) {
        return fail("the folder for the repaired clip does not exist");
    }
    const std::filesystem::path temp = with_suffix(destination, kTempSuffix);
    const std::filesystem::path fast = with_suffix(destination, kFastSuffix);
    // Left by a repair that was killed (otherwise it deletes them however it ends): deleting works
    // then. A running repair holds them open without delete sharing, so they stay.
    for (const std::filesystem::path& left : {temp, fast}) {
        if (std::filesystem::exists(left, ec) && DeleteFileW(left.c_str())) {
            log::info("Repair: deleted {}, left by an earlier repair", to_utf8(left.filename().wstring()));
        }
    }
    if (std::filesystem::exists(temp, ec) || std::filesystem::exists(fast, ec)) {
        // Another repair to the same name is running; its files must not be touched.
        return fail("another repair to that name is still running");
    }
    const auto moov = read_moov(source);
    const auto tracks = moov ? mp4::tracks(*moov) : std::nullopt;
    if (!tracks) {
        log::info("Repair of {} refused: the index cannot be read", name);
        return fail("the clip could not be read");
    }
    const RepairCheck check = decide_repair(*tracks);
    if (!check.needed) {
        log::info("Repair of {} refused: {}", name, check.reason);
        result.message = "Not repaired: " + lower_first(check.reason) + ".";
        return result;
    }
    const std::uint64_t source_bytes = std::filesystem::file_size(source, ec);
    if (ec) {
        return fail("the clip could not be read");
    }
    ULARGE_INTEGER free_bytes{};
    // With a trailing backslash: the root of a network share needs one.
    if (GetDiskFreeSpaceExW((destination.parent_path() / L"").c_str(), &free_bytes, nullptr, nullptr) &&
        free_bytes.QuadPart < 2 * source_bytes + kSpareBytes) {
        log::info("Repair of {} refused: {} bytes free, {} needed", name, free_bytes.QuadPart, 2 * source_bytes + kSpareBytes);
        return fail("there is not enough free space on the disk");
    }
    // The source's own notes about the clip (Medal keeps its library entry there), kept in the copy.
    const std::vector<std::uint8_t> user_data = mp4::moov_child(*moov, "udta").value_or(std::vector<std::uint8_t>{});

    TempFiles temp_files({temp, fast});
    const auto started = std::chrono::steady_clock::now();
    try {
        ComPtr<IMFSourceReader> reader = open_reader(source);
        const SourceLayout layout = find_layout(reader.Get(), *tracks);
        const UINT32 rate = mix_sample_rate(layout.audio.front().rate);
        const bool all_mono = std::all_of(layout.audio.begin(), layout.audio.end(), [](const SourceAudio& a) { return a.channels == 1; });
        const UINT32 channels = all_mono ? 1 : 2;
        std::vector<std::uint32_t> declared;
        for (const SourceAudio& audio : layout.audio) {
            declared.push_back(MFGetAttributeUINT32(audio.type.Get(), MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 0));
        }
        const std::uint32_t mix_bytes = mix_aac_bytes_per_second(declared);

        auto locations = mp4::sample_locations(*moov, layout.video_track, source_bytes);
        if (!locations || locations->empty() || locations->size() != (*tracks)[layout.video_track].samples) {
            throw RepairError("the clip's index of the video cannot be read");
        }
        VideoBytes frames(source, std::move(*locations));
        // The SPS/PPS the frames really use (VideoBytes): the first frame's own when it has both,
        // otherwise the avcC's, which the source hands out as the sequence header.
        std::vector<std::uint8_t> video_sets = frames.parameter_sets(0, nullptr);
        const auto first_units = h264::split_annex_b(video_sets);
        const bool complete =
            std::any_of(first_units.begin(), first_units.end(), [](const h264::NalUnit& u) { return u.type == h264::kNalSps; }) &&
            std::any_of(first_units.begin(), first_units.end(), [](const h264::NalUnit& u) { return u.type == h264::kNalPps; });
        if (complete) {
            BT_CHECK_HR(layout.video_type->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, video_sets.data(),
                                                   static_cast<UINT32>(video_sets.size())));
        } else {
            // In the form VideoBytes gives, so later keyframes compare byte for byte.
            video_sets = h264::parameter_set_bytes(h264::split_annex_b(blob(layout.video_type.Get(), MF_MT_MPEG_SEQUENCE_HEADER)));
            if (video_sets.empty()) {
                throw RepairError("the video's settings (SPS/PPS) are missing");
            }
        }

        BT_CHECK_HR(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
        BT_CHECK_HR(reader->SetStreamSelection(layout.video, TRUE));
        for (const SourceAudio& audio : layout.audio) {
            BT_CHECK_HR(reader->SetStreamSelection(audio.stream, TRUE));
        }

        ComPtr<IMFAttributes> attributes;
        BT_CHECK_HR(MFCreateAttributes(&attributes, 3));
        // The .tmp extension would not tell the sink writer which container to use.
        BT_CHECK_HR(attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));
        // The copied packets are written as fast as the disk allows; the mix is made only as far
        // as they have got (MixWriter), so nothing piles up in the sink writer.
        BT_CHECK_HR(attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));
        BT_CHECK_HR(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE));
        // Opened read-write by us, as ClipWriter does: the sink reads back what it wrote when it
        // finishes, and a file it opens itself from a URL is write-only (Finalize then failed with
        // E_ACCESSDENIED, measured).
        ComPtr<IMFByteStream> file;
        BT_CHECK_HR(MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_FAIL_IF_EXIST, MF_FILEFLAGS_NONE, temp.c_str(), &file));
        // On a failure or cancel the sink writer is let go without Finalize, and its work queue may
        // still hold the file for a moment; Close releases the handle whoever holds a reference, so
        // the temporary file can be deleted (the same as the exporter does).
        struct CloseFile {
            ComPtr<IMFByteStream> stream;
            ~CloseFile() { close(); }
            void close() {
                if (stream) {
                    stream->Close();
                    stream.Reset();
                }
            }
        } close_file{file};
        ComPtr<IMFSinkWriter> writer;
        BT_CHECK_HR(MFCreateSinkWriterFromURL(nullptr, file.Get(), attributes.Get(), &writer));

        // The same type as input and output: the writer passes the packets through untouched.
        DWORD video_stream = 0;
        BT_CHECK_HR(writer->AddStream(layout.video_type.Get(), &video_stream));
        BT_CHECK_HR(writer->SetInputMediaType(video_stream, layout.video_type.Get(), nullptr));

        // The sink writes the tracks in the order they are added: the mix is the first sound track.
        ComPtr<IMFMediaType> aac;
        BT_CHECK_HR(MFCreateMediaType(&aac));
        BT_CHECK_HR(aac->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
        BT_CHECK_HR(aac->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC));
        BT_CHECK_HR(aac->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, kPcmBits));
        BT_CHECK_HR(aac->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate));
        BT_CHECK_HR(aac->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels));
        BT_CHECK_HR(aac->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, mix_bytes));
        BT_CHECK_HR(aac->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, kAacPayloadRaw));
        BT_CHECK_HR(aac->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, kAacLcLevel));
        DWORD mix_stream = 0;
        BT_CHECK_HR(writer->AddStream(aac.Get(), &mix_stream));
        const ComPtr<IMFMediaType> pcm = pcm_type(MFAudioFormat_PCM, kPcmBits, rate, channels);
        const HRESULT encoder_hr = writer->SetInputMediaType(mix_stream, pcm.Get(), nullptr);
        if (FAILED(encoder_hr)) {
            log::warn("Repair: the AAC encoder refuses {} Hz {} channels at {} bytes/s: {}", rate, channels, mix_bytes,
                      hresult_text(encoder_hr));
            throw RepairError("the mixed sound cannot be encoded");
        }

        // Windows' own MP4 reader (Media Player, Films & TV, Backtrack's player) plays the sound
        // track that declares the highest bitrate, not the one marked as the default (measured,
        // see ClipWriter): each copied track declares at least one byte per second less than the
        // one before it, so the mix stays the one Windows plays.
        std::vector<DWORD> copy_streams;
        std::vector<std::string> names{kMixTrackName};
        UINT32 declared_before = mix_bytes;
        for (const SourceAudio& audio : layout.audio) {
            ComPtr<IMFMediaType> type;
            BT_CHECK_HR(MFCreateMediaType(&type));
            BT_CHECK_HR(audio.type->CopyAllItems(type.Get()));
            UINT32 bytes = MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 0);
            if (declared_before > 1 && bytes >= declared_before) {
                bytes = declared_before - 1;
                BT_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytes));
            }
            declared_before = bytes;
            DWORD stream = 0;
            BT_CHECK_HR(writer->AddStream(type.Get(), &stream));
            BT_CHECK_HR(writer->SetInputMediaType(stream, type.Get(), nullptr));
            copy_streams.push_back(stream);
            names.push_back(audio.name);
        }
        BT_CHECK_HR(writer->BeginWriting());
        log::info("Repair: {} ({:.1f} s), {} sound tracks mixed at {} Hz, {} channels, {} kbit/s", name,
                  static_cast<double>(layout.duration) / kTicksPerSecond, layout.audio.size(), rate, channels,
                  mix_bytes * 8 / 1000);

        const Time100ns latest = latest_time(layout.duration);
        MixWriter mix(source, layout.audio, writer.Get(), mix_stream, latest, rate, channels);
        ProgressReporter reporter(progress, layout.duration);
        bool video_done = false;
        std::vector<bool> audio_done(layout.audio.size(), false);
        const auto all_done = [&] {
            return video_done && std::find(audio_done.begin(), audio_done.end(), false) == audio_done.end();
        };
        std::optional<Time100ns> last_video;
        std::size_t video_frames = 0;
        Time100ns frontier = 0;  // the latest packet copied, on the clip's timeline
        int empty_reads = 0;
        while (!all_done()) {
            check_cancel(cancel);
            DWORD index = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            BT_CHECK_HR(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, &index, &flags, &timestamp,
                                           &sample));
            if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
                throw std::runtime_error("the source reader reported a stream error");
            }
            if ((flags & (MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED | MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)) != 0) {
                throw RepairError("the clip changes format partway through");
            }
            const bool is_video = index == layout.video;
            std::size_t track = 0;
            while (track < layout.audio.size() && layout.audio[track].stream != index) {
                ++track;
            }
            const bool is_audio = track < layout.audio.size();
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                video_done = video_done || is_video;
                if (is_audio) {
                    audio_done[track] = true;
                }
                continue;
            }
            if (!sample || (!is_video && !is_audio)) {
                if (++empty_reads > kMaxEmptyReads) {
                    throw std::runtime_error("the source reader returns no samples");
                }
                continue;
            }
            empty_reads = 0;
            LONGLONG pts = 0;
            if (FAILED(sample->GetSampleTime(&pts))) {
                throw RepairError("a packet in the clip has no timestamp");
            }
            check_time(pts, latest);
            if (is_video) {
                if (last_video && pts <= *last_video) {
                    // Decode order is not display order: B-frames, which the MP4 sink would store
                    // without their reordering (decide_repair already refuses files that say so).
                    throw RepairError("the video uses B-frames, which the repair cannot copy");
                }
                if (video_frames >= frames.size()) {
                    throw RepairError("the video has more frames than its index lists");
                }
                // Keyframes are where SPS/PPS can change; checking only those reads about one frame
                // a second a second time.
                if (MFGetAttributeUINT32(sample.Get(), MFSampleExtension_CleanPoint, FALSE) != FALSE) {
                    const std::vector<std::uint8_t> sets = frames.parameter_sets(video_frames, sample.Get());
                    if (!sets.empty() && sets != video_sets) {
                        throw RepairError("the video changes its settings partway through, which the repair cannot copy");
                    }
                }
                last_video = pts;
                ++video_frames;
                reporter.at(pts);
            }
            if (is_audio && layout.audio[track].shift > 0) {
                // A track that starts late by its edit list: the MP4 sink writes no edit lists, so
                // the delay goes into the packets' times. (A negative shift, the encoder's first
                // samples skipped, cannot: a time before 0. That copy then starts up to 50 ms
                // late in players that honour edit lists, as it already does in Windows' own.)
                pts += layout.audio[track].shift;
                BT_CHECK_HR(sample->SetSampleTime(pts));
            }
            // The packet goes on as it came: its bytes, time, duration and keyframe flag.
            BT_CHECK_HR(writer->WriteSample(is_video ? video_stream : copy_streams[track], sample.Get()));
            frontier = std::max(frontier, pts);
            mix.advance_to(frontier, cancel);
        }
        if (video_frames != frames.size()) {
            log::warn("Repair of {}: the reader gave {} video frames, the index lists {}", name, video_frames, frames.size());
            throw RepairError("the video has fewer frames than its index lists");
        }
        mix.finish(cancel);
        check_cancel(cancel);
        BT_CHECK_HR(writer->Finalize());
        writer.Reset();
        close_file.close();
        file.Reset();
        reader.Reset();

        // Unlike a clip, a repair without the index moved and labeled is not kept: the names are
        // what tells a repaired clip (first track "Game + mic") from one that needs it, and without
        // them a second repair would mix the mix in again.
        bool fast_start = false;
        try {
            fast_start = move_index_to_front(temp, fast, names, user_data);
        } catch (const std::exception& e) {
            log::warn("Repair: moving the MP4 index failed: {}", e.what());
        }
        check_cancel(cancel);  // the copy takes a few seconds for a long clip
        const auto written_moov = fast_start ? read_moov(fast) : std::nullopt;
        const auto written_tracks = written_moov ? mp4::tracks(*written_moov) : std::nullopt;
        const RepairCheck again = written_tracks ? decide_repair(*written_tracks) : RepairCheck{};
        if (!fast_start || !written_tracks || again.needed ||
            again.audio_tracks != static_cast<int>(layout.audio.size()) + 1) {
            log::warn("Repair of {}: the finished file does not read back as repaired (index moved: {}, {} sound tracks)",
                      name, fast_start, again.audio_tracks);
            return fail("the repaired clip could not be finished");
        }
        if (!MoveFileExW(fast.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
            const DWORD error = GetLastError();
            log::warn("Repair: renaming to {} failed: {}", to_utf8(destination.wstring()), hresult_text(HRESULT_FROM_WIN32(error)));
            return fail(error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS ? "a file with that name already exists"
                                                                                   : "the repaired clip could not be saved");
        }
        result.ok = true;
        result.message = "Repaired: the first sound track now has all the clip's sound together, and the original tracks are kept after it.";
        // The repaired clip exists now: nothing after this may turn the result into a failure.
        try {
            reporter.done();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            log::info("Repaired {} as {}: {} video frames, {:.3f} s of mixed sound, {} frames limited (lowest gain {:.3f}), {} "
                      "bytes, {} ms",
                      name, to_utf8(destination.filename().wstring()), video_frames,
                      static_cast<double>(mix.frames_written()) / rate, mix.limiter().limited_frames(),
                      mix.limiter().lowest_gain(), std::filesystem::file_size(destination, ec), ms);
        } catch (...) {
        }
        return result;
    } catch (const Cancelled&) {
        log::info("Repair of {} cancelled", name);
        result.cancelled = true;
        result.message = "Repair cancelled.";
        return result;
    } catch (const RepairError& e) {
        log::info("Repair of {} refused: {}", name, e.what());
        return fail(e.what());
    } catch (const HresultError& e) {
        log::warn("Repair of {} failed: {}", name, e.what());
        return fail(disk_full(e.hr()) ? "the disk is full" : "the clip could not be copied (it may be damaged)");
    } catch (const std::exception& e) {
        log::warn("Repair of {} failed: {}", name, e.what());
        return fail("the clip could not be copied (it may be damaged)");
    }
}

}  // namespace

std::optional<RepairCheck> check_for_repair(const std::filesystem::path& clip) {
    try {
        const auto moov = read_moov(clip);
        if (!moov) {
            return std::nullopt;
        }
        const auto tracks = mp4::tracks(*moov);
        if (!tracks) {
            return std::nullopt;
        }
        return decide_repair(*tracks);
    } catch (...) {
        return std::nullopt;
    }
}

// The caller is a worker thread: nothing may escape, not even from a log line or a path conversion.
RepairResult repair_clip(const std::filesystem::path& source, const std::filesystem::path& destination,
                         const std::function<void(double)>& progress, const std::atomic<bool>& cancel) {
    try {
        return repair_unguarded(source, destination, progress, cancel);
    } catch (...) {
        RepairResult result;
        result.message = "Could not repair the clip: an unexpected error.";
        return result;
    }
}

std::filesystem::path backup_path(const std::filesystem::path& clips_folder, const std::filesystem::path& file_name) {
    const std::filesystem::path folder = clips_folder / L"Before repair";
    const std::wstring name = file_name.filename().wstring();
    for (int n = 1; n <= kMaxFreeNames; ++n) {
        const std::filesystem::path path = folder / backup_file_name(name, n);
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) && !ec) {
            return path;
        }
    }
    return {};
}

}  // namespace bt
