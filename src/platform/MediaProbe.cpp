#include "platform/MediaProbe.h"

#include "platform/Com.h"
#include "platform/FrameReader.h"
#include "platform/Log.h"
#include "platform/Text.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

#include <cstring>
#include <format>
#include <fstream>

namespace bt {
namespace {

constexpr char kCacheMagic[4] = {'B', 'T', 'T', 'H'};
constexpr double kThumbPosition = 1.0 / 3.0;  // a third in: past the start of a clip, before the end
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
    const VisibleArea shown = visible_area(native.Get());
    info.width = static_cast<int>(shown.width);
    info.height = static_cast<int>(shown.height);
    info.ok = info.width > 0 && info.height > 0;

    if (read_cache(cache, info.thumbnail)) {
        return info;
    }

    if (!select_rgb_video(reader.Get())) {
        log::debug("Cannot decode {} to RGB for a thumbnail", job.file);
        return info;
    }
    if (info.duration_s > 0.0) {
        PROPVARIANT position;
        InitPropVariantFromInt64(static_cast<LONGLONG>(info.duration_s * kThumbPosition * kTicksPerSecond), &position);
        reader->SetCurrentPosition(GUID_NULL, position);
        PropVariantClear(&position);
    }

    info.thumbnail = read_scaled_frame(reader.Get(), kThumbWidth, kThumbHeight, job.file);
    if (!info.thumbnail.empty()) write_cache(cache, info.thumbnail);
    return info;
}

}  // namespace bt
