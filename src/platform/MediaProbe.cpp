#include "platform/MediaProbe.h"

#include "platform/Com.h"
#include "platform/Log.h"
#include "platform/Text.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

#include <algorithm>
#include <climits>
#include <array>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>

namespace bt {
namespace {

constexpr char kCacheMagic[4] = {'B', 'T', 'T', 'H'};
constexpr int kMaxEmptyReads = 30;  // ReadSample may return a gap or a format change before a frame
constexpr double kThumbPosition = 1.0 / 3.0;  // a third in: past the start of a clip, before the end
constexpr int kSamplesPerAxis = 3;  // 3x3 source pixels averaged per thumbnail pixel
constexpr double kTicksPerSecond = 10'000'000.0;

std::uint64_t fnv1a(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string lower_ascii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

bool read_cache(const std::filesystem::path& path, std::vector<std::uint8_t>& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    char magic[4] = {};
    std::uint32_t size[2] = {};
    file.read(magic, sizeof(magic));
    file.read(reinterpret_cast<char*>(size), sizeof(size));
    if (!file || std::memcmp(magic, kCacheMagic, sizeof(magic)) != 0 || size[0] != kThumbWidth ||
        size[1] != kThumbHeight) {
        return false;
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kThumbWidth) * kThumbHeight * 4);
    file.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    if (!file) return false;
    out = std::move(pixels);
    return true;
}

void write_cache(const std::filesystem::path& path, const std::vector<std::uint8_t>& pixels) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path temp = path;
    temp += L".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        const std::uint32_t size[2] = {kThumbWidth, kThumbHeight};
        file.write(kCacheMagic, sizeof(kCacheMagic));
        file.write(reinterpret_cast<const char*>(size), sizeof(size));
        file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
        if (!file) {
            file.close();
            std::filesystem::remove(temp, ec);
            return;  // a missing cache entry only means the thumbnail is made again next time
        }
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        std::filesystem::remove(temp, ec);
    }
}

// The visible part of a decoded frame: H.264 1080p is coded as 1088 lines and the extra 8 are
// padding, named by the minimum display aperture.
struct Visible {
    UINT32 x = 0, y = 0, width = 0, height = 0;
};

Visible visible_area(IMFMediaType* type) {
    Visible v;
    MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &v.width, &v.height);
    MFVideoArea area{};
    if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr)) &&
        area.Area.cx > 0 && area.Area.cy > 0) {
        const auto x = static_cast<UINT32>(std::max<LONG>(area.OffsetX.value, 0));
        const auto y = static_cast<UINT32>(std::max<LONG>(area.OffsetY.value, 0));
        if (x + static_cast<UINT32>(area.Area.cx) <= v.width && y + static_cast<UINT32>(area.Area.cy) <= v.height) {
            v = Visible{x, y, static_cast<UINT32>(area.Area.cx), static_cast<UINT32>(area.Area.cy)};
        }
    }
    return v;
}

// Scales the visible part of a BGRA frame to the thumbnail size, cropping to 16:9 (cover) and
// averaging a few source pixels per thumbnail pixel so fine text does not shimmer.
std::vector<std::uint8_t> make_thumbnail(const BYTE* scan0, LONG pitch, const Visible& v) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(kThumbWidth) * kThumbHeight * 4);
    const double scale = std::max(static_cast<double>(kThumbWidth) / v.width, static_cast<double>(kThumbHeight) / v.height);
    const double crop_x = (v.width - kThumbWidth / scale) / 2.0;
    const double crop_y = (v.height - kThumbHeight / scale) / 2.0;
    const double step = 1.0 / scale;
    for (int ty = 0; ty < kThumbHeight; ++ty) {
        for (int tx = 0; tx < kThumbWidth; ++tx) {
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
            std::uint8_t* o = out.data() + (static_cast<std::size_t>(ty) * kThumbWidth + tx) * 4;
            o[0] = static_cast<std::uint8_t>(sum[0] / kCount);
            o[1] = static_cast<std::uint8_t>(sum[1] / kCount);
            o[2] = static_cast<std::uint8_t>(sum[2] / kCount);
            o[3] = 0xFF;
        }
    }
    return out;
}

}  // namespace

MediaProbe::MediaProbe(std::filesystem::path cache_dir, std::function<void()> ready)
    : cache_dir_(std::move(cache_dir)), ready_(std::move(ready)), thread_(&MediaProbe::run, this) {}

MediaProbe::~MediaProbe() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    thread_.join();
}

void MediaProbe::request(const std::filesystem::path& folder, const std::string& file, std::int64_t size_bytes,
                         bool need_info, bool urgent) {
    {
        std::lock_guard lock(mutex_);
        for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
            if (it->file == file && it->folder == folder) {
                Job job = std::move(*it);
                job.need_info = job.need_info || need_info;
                job.size_bytes = size_bytes;
                if (urgent) {
                    jobs_.erase(it);
                    jobs_.push_front(std::move(job));
                } else {
                    *it = std::move(job);
                }
                return;
            }
        }
        if (urgent) {
            jobs_.push_front(Job{folder, file, size_bytes, need_info});
        } else {
            jobs_.push_back(Job{folder, file, size_bytes, need_info});
        }
    }
    wake_.notify_one();
}

std::vector<MediaInfo> MediaProbe::take_results() {
    std::lock_guard lock(mutex_);
    return std::exchange(results_, {});
}

std::filesystem::path MediaProbe::cache_path(const Job& job) const {
    const std::uint64_t key = fnv1a(lower_ascii(job.file) + "|" + std::to_string(job.size_bytes));
    return cache_dir_ / std::format(L"{:016x}.thumb", key);
}

void MediaProbe::run() {
    // §9: nothing escapes a thread. A failure here only costs thumbnails.
    try {
        ComApartment com(COINIT_MULTITHREADED);
        while (true) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                if (stop_) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            MediaInfo info = probe(job);
            {
                std::lock_guard lock(mutex_);
                if (stop_) return;
                results_.push_back(std::move(info));
            }
            ready_();
        }
    } catch (const std::exception& e) {
        log::error("Reading clip thumbnails stopped: {}", e.what());
    } catch (...) {
        log::error("Reading clip thumbnails stopped: unknown exception");
    }
}

MediaInfo MediaProbe::probe(const Job& job) {
    MediaInfo info;
    info.file = job.file;
    info.size_bytes = job.size_bytes;
    const std::filesystem::path cache = cache_path(job);
    if (!job.need_info && read_cache(cache, info.thumbnail)) {
        info.ok = true;
        return info;
    }

    const std::filesystem::path path = job.folder / to_wide(job.file);
    ComPtr<IMFAttributes> attributes;
    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateAttributes(&attributes, 1);
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader);
    if (FAILED(hr)) {
        // A clip still being written (another program) or not a video: no info, the UI shows a plain card.
        log::debug("Cannot read {}: {}", job.file, hresult_text(hr));
        return info;
    }

    PROPVARIANT duration;
    PropVariantInit(&duration);
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION,
                                                   &duration)) &&
        duration.vt == VT_UI8) {
        info.duration_s = static_cast<double>(duration.uhVal.QuadPart) / kTicksPerSecond;
    }
    PropVariantClear(&duration);

    constexpr auto kVideo = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    ComPtr<IMFMediaType> native;
    if (FAILED(reader->GetNativeMediaType(kVideo, 0, &native))) {
        log::debug("{} has no video stream", job.file);
        return info;
    }
    const Visible shown = visible_area(native.Get());
    info.width = static_cast<int>(shown.width);
    info.height = static_cast<int>(shown.height);
    info.ok = info.width > 0 && info.height > 0;

    if (read_cache(cache, info.thumbnail)) {
        return info;
    }

    ComPtr<IMFMediaType> rgb;
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(kVideo, TRUE);
    if (FAILED(MFCreateMediaType(&rgb)) || FAILED(rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
        FAILED(reader->SetCurrentMediaType(kVideo, nullptr, rgb.Get()))) {
        log::debug("Cannot decode {} to RGB for a thumbnail", job.file);
        return info;
    }
    if (info.duration_s > 0.0) {
        PROPVARIANT position;
        InitPropVariantFromInt64(static_cast<LONGLONG>(info.duration_s * kThumbPosition * kTicksPerSecond), &position);
        reader->SetCurrentPosition(GUID_NULL, position);
        PropVariantClear(&position);
    }

    for (int attempt = 0; attempt < kMaxEmptyReads; ++attempt) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(kVideo, 0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            log::debug("No frame for the thumbnail of {}: {}", job.file,
                       FAILED(hr) ? hresult_text(hr) : std::string("end of file"));
            break;
        }
        if (!sample) continue;  // a gap or a format change; the next read has the frame
        ComPtr<IMFMediaType> current;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(hr = reader->GetCurrentMediaType(kVideo, &current)) ||
            FAILED(hr = sample->ConvertToContiguousBuffer(&buffer))) {
            log::debug("Cannot read the thumbnail frame of {}: {}", job.file, hresult_text(hr));
            break;
        }
        const Visible frame = visible_area(current.Get());
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
            info.thumbnail = make_thumbnail(scan0, pitch, frame);
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
                log::debug("Cannot lock the thumbnail frame of {}", job.file);
                break;
            }
            const std::size_t needed = static_cast<std::size_t>(std::abs(stride)) * coded_height;
            if (length >= needed && coded_height >= frame.y + frame.height) {
                BYTE* first_row = stride > 0 ? data : data + static_cast<std::size_t>(-stride) * (coded_height - 1);
                info.thumbnail = make_thumbnail(first_row, stride, frame);
            } else {
                log::debug("Thumbnail frame of {} is smaller than its format says", job.file);
            }
            buffer->Unlock();
        }
        if (!info.thumbnail.empty()) write_cache(cache, info.thumbnail);
        break;
    }
    return info;
}

}  // namespace bt
