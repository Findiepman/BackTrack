#include "platform/ClipWriter.h"

#include "core/ClipPlan.h"
#include "core/H264.h"
#include "platform/Log.h"

#include <mfapi.h>
#include <mfreadwrite.h>

#include <cstring>

namespace bt {
namespace {

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
        BT_CHECK_HR(MFCreateAttributes(&attributes, 3));
        // Compressed data is passed through; no throttling (we write as fast as the disk allows)
        // and no hardware transform may be inserted.
        BT_CHECK_HR(attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));
        BT_CHECK_HR(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE));
        // The .tmp extension would not tell the sink writer which container to use.
        BT_CHECK_HR(attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));

        ComPtr<IMFSinkWriter> writer;
        BT_CHECK_HR(MFCreateSinkWriterFromURL(temp.c_str(), nullptr, attributes.Get(), &writer));

        // §7: the stream type is the encoder's output type, and the same type as input makes the
        // writer pass the samples through untouched.
        ComPtr<IMFMediaType> video_type = copy_type(request.video_type.Get());
        // The MP4 sink strips SPS/PPS from the samples and stores the ones from the type's
        // sequence header in the file. The clip's own first keyframe is the truth: AMD's output
        // type carries a header that does not match what it encodes (measured: every frame decoded
        // as garbage), so the in-band parameter sets win whenever the keyframe has them.
        const std::vector<std::uint8_t> in_band = h264::parameter_sets(*request.video.front().data);
        UINT32 header_size = 0;
        if (!in_band.empty()) {
            BT_CHECK_HR(video_type->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, in_band.data(), static_cast<UINT32>(in_band.size())));
        } else if (FAILED(video_type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &header_size)) || header_size == 0) {
            throw std::runtime_error("the first keyframe has no SPS/PPS and the encoder gave no sequence header");
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

        BT_CHECK_WIN32(MoveFileExW(temp.c_str(), request.path.c_str(), MOVEFILE_WRITE_THROUGH));
        result.audio_frames = with_audio ? plan.audio_samples : 0;
    } catch (const std::exception& e) {
        DeleteFileW(temp.c_str());
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
              plan.audio_dropped, static_cast<double>(result.bytes) / (1024.0 * 1024.0), write_ms);
    return result;
}

}  // namespace bt
