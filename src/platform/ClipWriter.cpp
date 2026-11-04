#include "platform/ClipWriter.h"

#include "core/ClipPlan.h"
#include "core/H264.h"
#include "core/Mp4.h"
#include "platform/Log.h"

#include <mfapi.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace bt {
namespace {

constexpr double kBytesPerMegabyte = 1024.0 * 1024.0;

// A moov box this large would mean something unexpected; the file is then left as written.
constexpr std::uint64_t kMaxMoovBytes = 64ull * 1024 * 1024;
constexpr std::size_t kCopyChunkBytes = 1024 * 1024;
constexpr std::size_t kLargestBoxHeader = 16;

bool copy_range(std::ifstream& in, std::ofstream& out, std::uint64_t from, std::uint64_t to) {
    std::vector<char> chunk(kCopyChunkBytes);
    in.seekg(static_cast<std::streamoff>(from));
    for (std::uint64_t at = from; at < to && in && out;) {
        const auto n = static_cast<std::streamsize>(std::min<std::uint64_t>(chunk.size(), to - at));
        in.read(chunk.data(), n);
        out.write(chunk.data(), n);
        at += static_cast<std::uint64_t>(n);
    }
    return static_cast<bool>(in) && static_cast<bool>(out);
}

// "Fast start": writes a copy of `in` with the moov box (the index) in front of the media data,
// so Discord previews and phones can start playing without fetching the end of the file first.
// False when the layout is not [header boxes, mdat, moov] or the offsets cannot be moved; the
// caller then keeps the file as written, which plays everywhere, only less quickly when streamed.
bool move_index_to_front(const std::filesystem::path& in_path, const std::filesystem::path& out_path) {
    std::error_code ec;
    const std::uint64_t file_size = std::filesystem::file_size(in_path, ec);
    if (ec) {
        return false;
    }
    std::ifstream in(in_path, std::ios::binary);
    std::vector<mp4::BoxHeader> boxes;
    for (std::uint64_t at = 0; at < file_size;) {
        std::uint8_t header[kLargestBoxHeader] = {};
        const auto available = static_cast<std::size_t>(std::min<std::uint64_t>(sizeof(header), file_size - at));
        in.seekg(static_cast<std::streamoff>(at));
        in.read(reinterpret_cast<char*>(header), static_cast<std::streamsize>(available));
        auto box = mp4::read_box_header(std::span<const std::uint8_t>(header, available), 0, file_size - at);
        if (!in || !box || box->size > file_size - at) {
            return false;
        }
        box->offset = at;
        boxes.push_back(*box);
        at += box->size;
    }
    if (boxes.size() < 2 || boxes.back().type != "moov" || boxes[boxes.size() - 2].type != "mdat" ||
        boxes.back().size > kMaxMoovBytes) {
        return false;
    }
    const mp4::BoxHeader moov = boxes.back();
    const mp4::BoxHeader mdat = boxes[boxes.size() - 2];
    std::vector<std::uint8_t> moov_bytes(static_cast<std::size_t>(moov.size));
    in.seekg(static_cast<std::streamoff>(moov.offset));
    in.read(reinterpret_cast<char*>(moov_bytes.data()), static_cast<std::streamsize>(moov_bytes.size()));
    if (!in) {
        return false;
    }
    // Inserting the moov in front of mdat moves every media byte by the moov's size.
    const auto moved = mp4::shift_chunk_offsets(moov_bytes, moov.size);
    if (!moved) {
        return false;
    }
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    bool ok = copy_range(in, out, 0, mdat.offset);
    out.write(reinterpret_cast<const char*>(moved->data()), static_cast<std::streamsize>(moved->size()));
    ok = ok && copy_range(in, out, mdat.offset, moov.offset);
    out.close();
    return ok && !out.fail();
}

ComPtr<IMFMediaType> copy_type(IMFMediaType* source) {
    ComPtr<IMFMediaType> copy;
    BT_CHECK_HR(MFCreateMediaType(&copy));
    BT_CHECK_HR(source->CopyAllItems(copy.Get()));
    return copy;
}

ComPtr<IMFSample> make_sample(const Packet& packet, const ClipSample& planned) {
    ComPtr<IMFMediaBuffer> buffer;
    const std::vector<std::uint8_t>& bytes = *packet.data;
    BT_CHECK_HR(MFCreateMemoryBuffer(static_cast<DWORD>(bytes.size()), &buffer));
    BYTE* data = nullptr;
    BT_CHECK_HR(buffer->Lock(&data, nullptr, nullptr));
    std::memcpy(data, bytes.data(), bytes.size());
    BT_CHECK_HR(buffer->Unlock());
    BT_CHECK_HR(buffer->SetCurrentLength(static_cast<DWORD>(bytes.size())));

    ComPtr<IMFSample> sample;
    BT_CHECK_HR(MFCreateSample(&sample));
    BT_CHECK_HR(sample->AddBuffer(buffer.Get()));
    BT_CHECK_HR(sample->SetSampleTime(planned.time));
    BT_CHECK_HR(sample->SetSampleDuration(planned.duration));
    if (!planned.audio) {
        BT_CHECK_HR(sample->SetUINT32(MFSampleExtension_CleanPoint, planned.keyframe ? TRUE : FALSE));
    }
    return sample;
}

}  // namespace

ClipWriteResult write_clip(const ClipWriteRequest& request) {
    ClipWriteResult result;
    const ClipPlan plan = plan_clip(request.video, request.audio);
    if (plan.samples.empty()) {
        result.error = "the clip does not start on a keyframe";
        return result;
    }
    if (!request.video_type) {
        result.error = "the video format is not known yet";
        return result;
    }

    // §7: written under a temporary name and renamed at the end, so a crash never leaves a broken
    // .mp4 with the final name.
    std::filesystem::path temp = request.path;
    temp += L".tmp";
    const auto started = std::chrono::steady_clock::now();
    try {
        ComPtr<IMFAttributes> attributes;
        BT_CHECK_HR(MFCreateAttributes(&attributes, 5));
        // Compressed data is passed through; no throttling (we write as fast as the disk allows)
        // and no hardware transform may be inserted.
        BT_CHECK_HR(attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));
        BT_CHECK_HR(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE));
        // The .tmp extension would not tell the sink writer which container to use.
        BT_CHECK_HR(attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));
        // Not MF_MPEG4SINK_MOOV_BEFORE_MDAT: it moves the moov box but leaves the chunk offsets
        // pointing at the old positions, so nothing decodes (measured). move_index_to_front()
        // below does it properly.
        // Not MF_MPEG4SINK_SPSPPS_PASSTHROUGH: with it the sink stops converting the Annex B
        // samples to length-prefixed NAL units and every frame is unreadable (measured).

        ComPtr<IMFSinkWriter> writer;
        // The file is opened read-write by us: moving the moov box to the front means the sink
        // reads back what it wrote, and with a plain URL it opens the file write-only, so
        // Finalize failed with E_ACCESSDENIED (measured).
        ComPtr<IMFByteStream> file;
        BT_CHECK_HR(MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_DELETE_IF_EXIST, MF_FILEFLAGS_NONE, temp.c_str(),
                                 &file));
        BT_CHECK_HR(MFCreateSinkWriterFromURL(nullptr, file.Get(), attributes.Get(), &writer));

        // §7: the stream type is the encoder's output type, and the same type as input makes the
        // writer pass the samples through untouched.
        ComPtr<IMFMediaType> video_type = copy_type(request.video_type.Get());
        // The MP4 sink strips SPS/PPS from the samples and stores the ones from the type's
        // sequence header in the file. The clip's own first keyframe is the truth: AMD's output
        // type carries a header that does not match what it encodes (measured: every frame decoded
        // as garbage), so the in-band parameter sets win whenever the keyframe has them.
        // The first keyframe normally carries them; otherwise any keyframe of the clip (same
        // encoder, same parameter sets). Only when none does is the type's header used.
        std::vector<std::uint8_t> in_band;
        for (const Packet& packet : request.video) {
            if (packet.keyframe) {
                in_band = h264::parameter_sets(*packet.data);
                if (!in_band.empty()) {
                    break;
                }
            }
        }
        UINT32 header_size = 0;
        if (!in_band.empty()) {
            BT_CHECK_HR(video_type->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, in_band.data(), static_cast<UINT32>(in_band.size())));
        } else if (FAILED(video_type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &header_size)) || header_size == 0) {
            throw std::runtime_error("no keyframe has SPS/PPS and the encoder gave no sequence header");
        } else {
            log::warn("No keyframe in the clip carries SPS/PPS; using the encoder's own header, which may not play");
        }
        DWORD video_stream = 0;
        BT_CHECK_HR(writer->AddStream(video_type.Get(), &video_stream));
        BT_CHECK_HR(writer->SetInputMediaType(video_stream, video_type.Get(), nullptr));

        DWORD audio_stream = 0;
        const bool with_audio = request.audio_type && plan.audio_samples > 0;
        if (with_audio) {
            ComPtr<IMFMediaType> audio_type = copy_type(request.audio_type.Get());
            BT_CHECK_HR(writer->AddStream(audio_type.Get(), &audio_stream));
            BT_CHECK_HR(writer->SetInputMediaType(audio_stream, audio_type.Get(), nullptr));
        }

        BT_CHECK_HR(writer->BeginWriting());
        for (const ClipSample& planned : plan.samples) {
            if (planned.audio && !with_audio) {
                continue;
            }
            const Packet& packet = planned.audio ? request.audio[planned.index] : request.video[planned.index];
            const ComPtr<IMFSample> sample = make_sample(packet, planned);
            BT_CHECK_HR(writer->WriteSample(planned.audio ? audio_stream : video_stream, sample.Get()));
        }
        BT_CHECK_HR(writer->Finalize());
        writer.Reset();  // closes the file
        file.Reset();

        std::filesystem::path fast = request.path;
        fast += L".fast.tmp";
        bool fast_start = false;
        try {
            fast_start = move_index_to_front(temp, fast);
        } catch (const std::exception& e) {
            log::debug("Moving the MP4 index failed: {}", e.what());
        }
        if (fast_start) {
            // Renamed first, deleted second: if the rename fails, the original is still there and
            // the catch below cleans up only temporary files.
            BT_CHECK_WIN32(MoveFileExW(fast.c_str(), request.path.c_str(), MOVEFILE_WRITE_THROUGH));
            DeleteFileW(temp.c_str());
        } else {
            DeleteFileW(fast.c_str());
            log::debug("MP4 index left at the end of {}", to_utf8(request.path.filename().wstring()));
            BT_CHECK_WIN32(MoveFileExW(temp.c_str(), request.path.c_str(), MOVEFILE_WRITE_THROUGH));
        }
        result.audio_frames = with_audio ? plan.audio_samples : 0;
    } catch (const std::exception& e) {
        DeleteFileW(temp.c_str());
        std::filesystem::path fast = request.path;
        fast += L".fast.tmp";
        DeleteFileW(fast.c_str());
        result.error = e.what();
        return result;
    }

    std::error_code ec;
    result.ok = true;
    result.bytes = std::filesystem::file_size(request.path, ec);
    result.seconds = static_cast<double>(plan.video_duration) / static_cast<double>(kTicksPerSecond);
    result.video_frames = plan.video_samples;
    const auto write_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    log::info("Saved {}: {:.3f} s, {} video frames, {} audio frames ({} dropped before the first frame), {:.1f} MB, "
              "written in {} ms",
              to_utf8(request.path.wstring()), result.seconds, result.video_frames, result.audio_frames,
              plan.audio_dropped, static_cast<double>(result.bytes) / kBytesPerMegabyte, write_ms);
    return result;
}

}  // namespace bt
