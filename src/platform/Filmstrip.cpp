#include "platform/Filmstrip.h"

#include "platform/Com.h"
#include "platform/FrameReader.h"
#include "platform/Log.h"
#include "platform/Text.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

#include <chrono>

namespace bt {
namespace {

constexpr int kMaxCount = 1000;
constexpr int kMaxSide = 4096;
// All pictures together; a caller bug asking for 1000 pictures of 4096 x 4096 gets nothing
// instead of an out-of-memory exception on its worker thread.
constexpr std::uint64_t kMaxTotalBytes = 256ull * 1024 * 1024;

// The H.264 decoder normally waits for several frames before it hands out the first one (it
// decodes them in parallel). After every seek that wait is paid again; in low latency mode the
// keyframe comes out at once (measured: 12 pictures of a 1080p60 clip in ~650 ms instead of ~1300).
void ask_decoder_for_low_latency(IMFSourceReader* reader) {
    ComPtr<IMFSourceReaderEx> ex;
    if (FAILED(reader->QueryInterface(IID_PPV_ARGS(&ex)))) {
        return;
    }
    for (DWORD i = 0;; ++i) {
        GUID category{};
        ComPtr<IMFTransform> transform;
        if (FAILED(ex->GetTransformForStream(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, &category,
                                             &transform))) {
            return;
        }
        ComPtr<IMFAttributes> attributes;
        if (category == MFT_CATEGORY_VIDEO_DECODER && SUCCEEDED(transform->GetAttributes(&attributes))) {
            attributes->SetUINT32(MF_LOW_LATENCY, TRUE);  // only a speed-up; no harm if refused
        }
    }
}

}  // namespace

std::vector<std::vector<std::uint8_t>> extract_frames(const std::filesystem::path& file, int count, int width,
                                                      int height) {
    std::vector<std::vector<std::uint8_t>> frames;
    if (count <= 0 || count > kMaxCount || width <= 0 || height <= 0 || width > kMaxSide || height > kMaxSide ||
        static_cast<std::uint64_t>(count) * static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4 >
            kMaxTotalBytes) {
        return frames;
    }
    const std::string name = to_utf8(file.filename().wstring());
    const auto started = std::chrono::steady_clock::now();

    ComPtr<IMFAttributes> attributes;
    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateAttributes(&attributes, 1);
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromURL(file.c_str(), attributes.Get(), &reader);
    if (FAILED(hr)) {
        log::debug("Filmstrip: cannot read {}: {}", name, hresult_text(hr));
        return frames;
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    LONGLONG duration = 0;
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION,
                                                   &value)) &&
        value.vt == VT_UI8) {
        duration = static_cast<LONGLONG>(value.uhVal.QuadPart);
    }
    PropVariantClear(&value);
    if (duration <= 0 || !select_rgb_video(reader.Get())) {
        log::debug("Filmstrip: {} has no length or cannot be decoded", name);
        return frames;
    }
    ask_decoder_for_low_latency(reader.Get());

    frames.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        // The middle of each slot, so the first and last pictures are not the very first and
        // very last frame (often a fade or a menu).
        const auto position = static_cast<LONGLONG>(static_cast<double>(duration) * (i + 0.5) / count);
        PROPVARIANT seek;
        InitPropVariantFromInt64(position, &seek);
        hr = reader->SetCurrentPosition(GUID_NULL, seek);
        PropVariantClear(&seek);
        std::vector<std::uint8_t> frame;
        if (SUCCEEDED(hr)) {
            frame = read_scaled_frame(reader.Get(), width, height, name);
        }
        if (frame.empty()) {
            if (frames.empty()) {
                log::debug("Filmstrip: no frame at {:.3f} s of {}", static_cast<double>(position) / 1e7, name);
                return {};
            }
            frame = frames.back();  // a gap near the end: repeat the last picture rather than fail
        }
        frames.push_back(std::move(frame));
    }
    log::debug("Filmstrip of {}: {} frames in {} ms", name, count,
               std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
    return frames;
}

}  // namespace bt
