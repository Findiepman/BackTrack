#include "platform/FrameReader.h"

#include "platform/Log.h"

#include <mfapi.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdlib>

namespace bt {
namespace {

constexpr int kMaxEmptyReads = 30;  // ReadSample may return a gap or a format change before a frame
constexpr int kSamplesPerAxis = 3;  // 3x3 source pixels averaged per output pixel
constexpr int kMaxOutputSide = 8192;

std::vector<std::uint8_t> scale_frame(const BYTE* scan0, LONG pitch, const VisibleArea& v, int width, int height) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height * 4);
    const double scale = std::max(static_cast<double>(width) / v.width, static_cast<double>(height) / v.height);
    const double crop_x = (v.width - width / scale) / 2.0;
    const double crop_y = (v.height - height / scale) / 2.0;
    const double step = 1.0 / scale;
    for (int ty = 0; ty < height; ++ty) {
        for (int tx = 0; tx < width; ++tx) {
            std::array<unsigned, 3> sum{};
            for (int sy = 0; sy < kSamplesPerAxis; ++sy) {
                for (int sx = 0; sx < kSamplesPerAxis; ++sx) {
                    const double fx = crop_x + (tx + (sx + 0.5) / kSamplesPerAxis) * step;
                    const double fy = crop_y + (ty + (sy + 0.5) / kSamplesPerAxis) * step;
                    const auto px = std::clamp<long long>(static_cast<long long>(fx), 0, v.width - 1) + v.x;
                    const auto py = std::clamp<long long>(static_cast<long long>(fy), 0, v.height - 1) + v.y;
                    const BYTE* p = scan0 + py * pitch + px * 4;
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                }
            }
            constexpr unsigned kCount = kSamplesPerAxis * kSamplesPerAxis;
            std::uint8_t* o = out.data() + (static_cast<std::size_t>(ty) * width + tx) * 4;
            o[0] = static_cast<std::uint8_t>(sum[0] / kCount);
            o[1] = static_cast<std::uint8_t>(sum[1] / kCount);
            o[2] = static_cast<std::uint8_t>(sum[2] / kCount);
            o[3] = 0xFF;
        }
    }
    return out;
}

}  // namespace

VisibleArea visible_area(IMFMediaType* type) {
    VisibleArea v;
    MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &v.width, &v.height);
    MFVideoArea area{};
    if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr)) &&
        area.Area.cx > 0 && area.Area.cy > 0) {
        const auto x = static_cast<UINT32>(std::max<LONG>(area.OffsetX.value, 0));
        const auto y = static_cast<UINT32>(std::max<LONG>(area.OffsetY.value, 0));
        if (x + static_cast<UINT32>(area.Area.cx) <= v.width && y + static_cast<UINT32>(area.Area.cy) <= v.height) {
            v = VisibleArea{x, y, static_cast<UINT32>(area.Area.cx), static_cast<UINT32>(area.Area.cy)};
        }
    }
    return v;
}

bool select_rgb_video(IMFSourceReader* reader) {
    constexpr auto kVideo = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    ComPtr<IMFMediaType> rgb;
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(kVideo, TRUE);
    return SUCCEEDED(MFCreateMediaType(&rgb)) && SUCCEEDED(rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) &&
           SUCCEEDED(rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) &&
           SUCCEEDED(reader->SetCurrentMediaType(kVideo, nullptr, rgb.Get()));
}

std::vector<std::uint8_t> read_scaled_frame(IMFSourceReader* reader, int width, int height, std::string_view name,
                                            LONGLONG* time) {
    constexpr auto kVideo = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    std::vector<std::uint8_t> out;
    if (width <= 0 || height <= 0 || width > kMaxOutputSide || height > kMaxOutputSide) {
        return out;
    }
    for (int attempt = 0; attempt < kMaxEmptyReads; ++attempt) {
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = reader->ReadSample(kVideo, 0, nullptr, &flags, &timestamp, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            log::debug("No frame to show for {}: {}", name, FAILED(hr) ? hresult_text(hr) : std::string("end of file"));
            break;
        }
        if (!sample) continue;  // a gap or a format change; the next read has the frame
        ComPtr<IMFMediaType> current;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(hr = reader->GetCurrentMediaType(kVideo, &current)) ||
            FAILED(hr = sample->ConvertToContiguousBuffer(&buffer))) {
            log::debug("Cannot read a frame of {}: {}", name, hresult_text(hr));
            break;
        }
        const VisibleArea frame = visible_area(current.Get());
        UINT32 coded_width = 0;
        UINT32 coded_height = 0;
        MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &coded_width, &coded_height);
        if (frame.width == 0 || frame.height == 0) break;

        // The video processor hands back either a 2D buffer (pitch known) or a plain one; for a
        // plain one the stride is in the media type, and RGB without one is stored bottom-up.
        ComPtr<IMF2DBuffer> buffer2d;
        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch))) {
            out = scale_frame(scan0, pitch, frame, width, height);
            buffer2d->Unlock2D();
        } else {
            LONG stride = static_cast<LONG>(MFGetAttributeUINT32(current.Get(), MF_MT_DEFAULT_STRIDE, 0));
            if (stride == 0 && SUCCEEDED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, coded_width, &stride))) {
                stride = -std::abs(stride);
            }
            // A row must hold the visible width; LONG_MIN has no absolute value.
            const long long row_bytes = (static_cast<long long>(frame.x) + frame.width) * 4;
            if (stride == LONG_MIN || std::abs(static_cast<long long>(stride)) < row_bytes) {
                stride = 0;
            }
            BYTE* data = nullptr;
            DWORD length = 0;
            if (stride == 0 || FAILED(buffer->Lock(&data, nullptr, &length))) {
                log::debug("Cannot lock a frame of {}", name);
                break;
            }
            const std::size_t needed = static_cast<std::size_t>(std::abs(stride)) * coded_height;
            if (length >= needed && coded_height >= frame.y + frame.height) {
                BYTE* first_row = stride > 0 ? data : data + static_cast<std::size_t>(-stride) * (coded_height - 1);
                out = scale_frame(first_row, stride, frame, width, height);
            } else {
                log::debug("A frame of {} is smaller than its format says", name);
            }
            buffer->Unlock();
        }
        if (time != nullptr && !out.empty()) {
            *time = timestamp;
        }
        break;
    }
    return out;
}

}  // namespace bt
