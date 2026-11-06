#include "platform/Trim.h"

#include "core/ClipBuffer.h"
#include "core/H264.h"
#include "core/TrimPlan.h"
#include "platform/ClipWriter.h"
#include "platform/Com.h"
#include "platform/Log.h"
#include "platform/Text.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <span>

namespace bt {
namespace {

// Far beyond any clip, and small enough that seconds * 10^7 fits in an int64 with room to spare.
constexpr double kMaxSeconds = 100.0 * 24 * 3600;
constexpr Time100ns kFallbackFrame = kTicksPerSecond / 60;
constexpr Time100ns kAacSamplesPerFrame = 1024;
constexpr Time100ns kFallbackAacFrame = kAacSamplesPerFrame * kTicksPerSecond / 48'000;
// ReadSample can hand out gaps and stream ticks without a sample; a file that only does that is
// broken, and the loop must still end.
constexpr int kMaxEmptyReads = 1000;

// A problem the user can do something about, or at least understand. Anything else (a failed
// HRESULT) is logged in full and reported with a general line.
class TrimError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The attributes write_clip and the MP4 sink need, copied from the source's native types. Not
// the whole type: the source adds its own MP4 sample description (MF_MT_MPEG4_SAMPLE_DESCRIPTION),
// which describes the old file, not the one being written.
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

struct SourceAudio {
    DWORD stream = 0;
    ComPtr<IMFMediaType> type;
    Time100ns frame_duration = kFallbackAacFrame;  // for a packet without its own duration
    std::vector<std::uint8_t> sample_description;  // the track's stsd box (MF_MT_MPEG4_SAMPLE_DESCRIPTION)
    std::string name;  // from the file's hdlr; empty when the order could not be matched
};

struct SourceStreams {
    std::optional<DWORD> video;
    ComPtr<IMFMediaType> video_type;
    Time100ns frame_duration = kFallbackFrame;  // for a frame without its own duration
    // Every sound track in file order (separate tracks: the mix, the system sound, the microphone).
    std::vector<SourceAudio> audio;
};

SourceStreams find_streams(IMFSourceReader* reader) {
    SourceStreams s;
    for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> native;
        const HRESULT hr = reader->GetNativeMediaType(index, 0, &native);
        if (hr == MF_E_INVALIDSTREAMNUMBER) {
            break;
        }
        BT_CHECK_HR(hr);
        GUID major{};
        GUID subtype{};
        BT_CHECK_HR(native->GetGUID(MF_MT_MAJOR_TYPE, &major));
        BT_CHECK_HR(native->GetGUID(MF_MT_SUBTYPE, &subtype));
        if (major == MFMediaType_Video && !s.video) {
            if (subtype != MFVideoFormat_H264) {
                throw TrimError("only H.264 video can be trimmed");
            }
            s.video = index;
            s.video_type = copy_keys(native.Get(), kVideoKeys);
            UINT32 num = 0;
            UINT32 den = 0;
            if (SUCCEEDED(MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &num, &den)) && num > 0 && den > 0) {
                s.frame_duration = std::max<Time100ns>(1, kTicksPerSecond * den / num);
            }
        } else if (major == MFMediaType_Audio) {
            if (subtype != MFAudioFormat_AAC) {
                throw TrimError("only clips with AAC sound can be trimmed");
            }
            SourceAudio audio;
            audio.stream = index;
            audio.type = copy_keys(native.Get(), kAudioKeys);
            UINT32 size = 0;
            if (SUCCEEDED(native->GetBlobSize(MF_MT_MPEG4_SAMPLE_DESCRIPTION, &size)) && size > 0) {
                audio.sample_description.resize(size);
                BT_CHECK_HR(native->GetBlob(MF_MT_MPEG4_SAMPLE_DESCRIPTION, audio.sample_description.data(), size, nullptr));
            }
            const UINT32 rate = MFGetAttributeUINT32(native.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
            if (rate > 0) {
                audio.frame_duration = std::max<Time100ns>(1, kAacSamplesPerFrame * kTicksPerSecond / rate);
            }
            s.audio.push_back(std::move(audio));
        }
    }
    if (!s.video) {
        throw TrimError("the file has no video");
    }
    return s;
}

// Puts the sound tracks in the file's order and gives them the file's names. Media Foundation's
// source reader numbers them its own way (measured: the last track in the file comes first), and a
// trim that kept that order would put the microphone where the mix belongs. Each track in the file
// is matched to the one stream with exactly its sample description (Backtrack's own tracks always
// differ there, see ClipWriter). When that does not work out for every track, the reader's order
// is turned around, which was the file's order on every file tried, and the names stay empty.
void put_audio_in_file_order(std::vector<SourceAudio>& audio, const std::vector<mp4::AudioTrackInfo>& file) {
    if (audio.size() < 2) {
        if (audio.size() == 1 && file.size() == 1) {
            audio.front().name = file.front().name;
        }
        return;
    }
    std::vector<SourceAudio> ordered;
    if (file.size() == audio.size()) {
        std::vector<bool> used(audio.size(), false);
        for (const mp4::AudioTrackInfo& track : file) {
            std::size_t match = audio.size();
            std::size_t matches = 0;
            for (std::size_t i = 0; i < audio.size(); ++i) {
                if (!track.sample_description.empty() && audio[i].sample_description == track.sample_description) {
                    match = i;
                    ++matches;
                }
            }
            if (matches != 1 || used[match]) {
                break;
            }
            used[match] = true;
            ordered.push_back(audio[match]);
            ordered.back().name = track.name;
        }
    }
    if (ordered.size() == audio.size()) {
        audio = std::move(ordered);
        return;
    }
    log::info("Trim: the sound tracks could not be matched to the file; keeping them in the reader's order, reversed");
    std::reverse(audio.begin(), audio.end());
    for (SourceAudio& track : audio) {
        track.name.clear();
    }
}

PacketBytes sample_bytes(IMFSample* sample) {
    ComPtr<IMFMediaBuffer> buffer;
    BT_CHECK_HR(sample->ConvertToContiguousBuffer(&buffer));
    BYTE* data = nullptr;
    DWORD length = 0;
    BT_CHECK_HR(buffer->Lock(&data, nullptr, &length));
    std::vector<std::uint8_t> bytes(data, data + length);
    BT_CHECK_HR(buffer->Unlock());
    return make_packet_bytes(std::move(bytes));
}

// What was read from the source: the timing of every video frame up to `out`, but the bytes only
// from the last keyframe at or before `in` on (trim_restarts_at), so trimming the end of a long
// clip does not hold the whole clip in memory.
struct SourceData {
    std::vector<TrimFrame> frames;
    std::vector<Time100ns> frame_durations;
    std::vector<PacketBytes> frame_bytes;  // null for frames that were let go
    std::vector<std::vector<Packet>> audio;  // per sound track
    bool video_reaches_out = false;  // a frame at or after `out` was seen
};

SourceData read_source(IMFSourceReader* reader, const SourceStreams& streams, Time100ns in, Time100ns out) {
    SourceData data;
    data.audio.resize(streams.audio.size());
    BT_CHECK_HR(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
    BT_CHECK_HR(reader->SetStreamSelection(*streams.video, TRUE));
    for (const SourceAudio& audio : streams.audio) {
        BT_CHECK_HR(reader->SetStreamSelection(audio.stream, TRUE));
    }
    bool video_done = false;
    std::vector<bool> audio_done(streams.audio.size(), false);
    const auto all_audio_done = [&] { return std::find(audio_done.begin(), audio_done.end(), false) == audio_done.end(); };
    std::size_t released = 0;  // frame_bytes before this index are already let go
    std::optional<Time100ns> restart;  // pts of the last restart keyframe
    int empty_reads = 0;

    while (!video_done || !all_audio_done()) {
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
            throw TrimError("the video changes format partway through");
        }
        const bool is_video = index == *streams.video;
        std::size_t track = 0;
        while (track < streams.audio.size() && streams.audio[track].stream != index) {
            ++track;
        }
        const bool is_audio = track < streams.audio.size();
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
            throw TrimError("a frame in the file has no timestamp");
        }
        LONGLONG duration = 0;
        if (FAILED(sample->GetSampleDuration(&duration))) {
            duration = 0;
        }

        if (is_video) {
            if (pts >= out) {
                video_done = true;
                data.video_reaches_out = true;
                BT_CHECK_HR(reader->SetStreamSelection(*streams.video, FALSE));
                continue;
            }
            const TrimFrame frame{pts, MFGetAttributeUINT32(sample.Get(), MFSampleExtension_CleanPoint, FALSE) != FALSE};
            if (!data.frames.empty() && frame.pts <= data.frames.back().pts) {
                // Decode order is not presentation order: the file has B-frames. plan_trim
                // refuses them; no need to read the rest.
                data.frames.push_back(frame);
                break;
            }
            if (trim_restarts_at(frame, in)) {
                for (; released < data.frame_bytes.size(); ++released) {
                    data.frame_bytes[released].reset();
                }
                for (std::vector<Packet>& audio : data.audio) {
                    std::erase_if(audio, [&](const Packet& p) { return p.pts < frame.pts; });
                }
                restart = frame.pts;
            }
            data.frames.push_back(frame);
            data.frame_durations.push_back(duration > 0 ? duration : streams.frame_duration);
            data.frame_bytes.push_back(sample_bytes(sample.Get()));
        } else {
            if (pts >= out) {
                audio_done[track] = true;
                BT_CHECK_HR(reader->SetStreamSelection(streams.audio[track].stream, FALSE));
                continue;
            }
            if (restart && pts < *restart) {
                continue;
            }
            const Time100ns span = duration > 0 ? duration : streams.audio[track].frame_duration;
            data.audio[track].push_back(Packet{pts, span, true, sample_bytes(sample.Get())});
        }
    }
    return data;
}

const char* problem_text(TrimProblem problem) {
    switch (problem) {
        case TrimProblem::NoVideo: return "the file has no video frames";
        case TrimProblem::NoKeyframe: return "the video has no keyframe to start from";
        case TrimProblem::Reordered:
            return "this video uses B-frames, which cannot be trimmed without re-encoding (Backtrack's own clips can)";
        case TrimProblem::EmptyRange: return "the selection is empty";
        case TrimProblem::StartPastEnd: return "the selection starts after the end of the video";
        case TrimProblem::None: break;
    }
    return "";
}

// write_clip reports raw HRESULT texts; only a full disk is worth telling the user as such.
std::string write_error_text(const std::string& error) {
    if (error.find("0x80070070") != std::string::npos || error.find("0x80070027") != std::string::npos) {
        return "the disk is full";
    }
    return "the trimmed clip could not be saved";
}

TrimResult trim_clip_unguarded(const TrimRequest& request) {
    TrimResult result;
    const std::string name = to_utf8(request.source.filename().wstring());
    if (!std::isfinite(request.in_s) || !std::isfinite(request.out_s) || std::abs(request.in_s) > kMaxSeconds ||
        std::abs(request.out_s) > kMaxSeconds) {
        result.error = "the selection is not valid";
        return result;
    }
    if (request.out_s <= request.in_s) {
        result.error = problem_text(TrimProblem::EmptyRange);
        return result;
    }
    // write_clip never replaces a file (the final rename fails), but saying so up front costs
    // the user no wait.
    std::error_code ec;
    if (std::filesystem::exists(request.destination, ec)) {
        result.error = "a file with that name already exists";
        return result;
    }
    if (!std::filesystem::is_directory(request.destination.parent_path(), ec)) {
        result.error = "the folder for the trimmed clip does not exist";
        return result;
    }
    const auto in = static_cast<Time100ns>(std::llround(request.in_s * static_cast<double>(kTicksPerSecond)));
    const auto out = static_cast<Time100ns>(std::llround(request.out_s * static_cast<double>(kTicksPerSecond)));
    const auto started = std::chrono::steady_clock::now();

    ClipWriteRequest write;
    TrimPlan plan;
    try {
        ComPtr<IMFSourceReader> reader;
        const HRESULT open_hr = MFCreateSourceReaderFromURL(request.source.c_str(), nullptr, &reader);
        if (FAILED(open_hr)) {
            log::warn("Trim: cannot open {}: {}", name, hresult_text(open_hr));
            result.error = "the clip could not be opened";
            return result;
        }
        SourceStreams streams = find_streams(reader.Get());
        put_audio_in_file_order(streams.audio, read_audio_tracks(request.source));
        SourceData data = read_source(reader.Get(), streams, in, out);
        reader.Reset();  // the source file is closed before the new one is written

        if (data.frames.empty() && data.video_reaches_out) {
            throw TrimError(problem_text(TrimProblem::EmptyRange));  // the selection ends before the first frame
        }
        plan = plan_trim(data.frames, in, out);
        if (plan.problem != TrimProblem::None) {
            throw TrimError(problem_text(plan.problem));
        }
        for (std::size_t i = plan.first; i < plan.end; ++i) {
            if (!data.frame_bytes[i]) {
                throw std::logic_error("a frame of the cut was let go while reading");
            }
            write.video.push_back(Packet{data.frames[i].pts, data.frame_durations[i], data.frames[i].keyframe,
                                         std::move(data.frame_bytes[i])});
        }
        // The Media Foundation MP4 source hands out H.264 as an Annex B byte stream (start codes,
        // SPS/PPS repeated on every keyframe), the same form the encoder gives the Recorder.
        // Anything else would be written as garbage, so it is refused here.
        if (h264::inspect(*write.video.front().data).slices == 0) {
            throw TrimError("the video frames are in a form Backtrack cannot copy");
        }

        // Every sound track is cut the same way, so a clip with separate tracks keeps all of them,
        // in their order and with their names.
        for (std::size_t t = 0; t < streams.audio.size(); ++t) {
            std::vector<Time100ns> audio_pts;
            audio_pts.reserve(data.audio[t].size());
            for (const Packet& p : data.audio[t]) {
                audio_pts.push_back(p.pts);
            }
            const TrimRange range = trim_audio(audio_pts, plan.start, out);
            ClipAudioTrack track{streams.audio[t].type, {}, streams.audio[t].name};
            for (std::size_t i = range.first; i < range.end; ++i) {
                track.packets.push_back(std::move(data.audio[t][i]));
            }
            write.audio.push_back(std::move(track));
        }
        write.path = request.destination;
        write.video_type = streams.video_type;
    } catch (const TrimError& e) {
        log::info("Trim of {} refused: {}", name, e.what());
        result.error = e.what();
        return result;
    } catch (const std::exception& e) {
        log::warn("Trim of {} failed: {}", name, e.what());
        result.error = "the clip could not be read";
        return result;
    } catch (...) {
        log::warn("Trim of {} failed: unknown exception", name);
        result.error = "the clip could not be read";
        return result;
    }

    const ClipWriteResult written = write_clip(write);
    if (!written.ok) {
        log::warn("Trim of {} could not be written: {}", name, written.error);
        result.error = write_error_text(written.error);
        return result;
    }
    result.ok = true;
    result.start_s = static_cast<double>(plan.start) / static_cast<double>(kTicksPerSecond);
    result.seconds = written.seconds;
    result.bytes = written.bytes;
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    log::info("Trimmed {} from {:.3f} s (asked {:.3f} s) to {:.3f} s: {:.3f} s, {} video and {} audio frames ({} sound "
              "tracks), {} ms",
              name, result.start_s, request.in_s, request.out_s, result.seconds, written.video_frames,
              written.audio_frames, written.audio_tracks, ms);
    return result;
}

}  // namespace

// The caller is a worker thread: nothing may escape, not even from a log line or a path conversion.
TrimResult trim_clip(const TrimRequest& request) {
    try {
        return trim_clip_unguarded(request);
    } catch (...) {
        TrimResult result;
        result.error = "the clip could not be trimmed";
        return result;
    }
}

}  // namespace bt
