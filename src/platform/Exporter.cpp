#include "platform/Exporter.h"

#include "core/Mp4.h"
#include "platform/Com.h"
#include "platform/FrameReader.h"
#include "platform/Log.h"
#include "platform/Text.h"

#include <codecapi.h>
#include <d3d11_4.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <strmif.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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
// Sound read before the first frame of the piece is known waits in memory; more than this means
// the video never comes.
constexpr std::size_t kMaxWaitingAudioSamples = 2000;
constexpr UINT32 kAacPayloadRaw = 0;
constexpr UINT32 kAacLcLevel = 0x29;  // AAC Low Complexity, the only profile Microsoft's encoder makes
constexpr UINT32 kBitsPerSample = 16;
constexpr double kPeakToMean = 1.5;  // variable bitrate: the busiest moments may take 1.5x the average
constexpr double kGopSeconds = 2.0;  // a keyframe every 2 s: players seek quickly, compression stays good
constexpr auto kProgressInterval = std::chrono::milliseconds(100);
constexpr int kMaxFreeNames = 9999;
// "To the end": far past any clip, and small enough that end * 48000 (sound frames) fits in int64.
constexpr Time100ns kOpenEnd = 100ll * 24 * 3600 * kTicksPerSecond;
constexpr Time100ns kMaxAudioLag = 3 * kTicksPerSecond;
constexpr Time100ns kAudioLagPadding = 2 * kTicksPerSecond;

// The temporary files every export writes next to the destination.
constexpr wchar_t kTempSuffix[] = L".tmp";
constexpr wchar_t kFastSuffix[] = L".fast.tmp";

// A problem the user can understand: shown as it is. Anything else (a failed HRESULT) is logged
// in full, and the hardware path then starts over in software.
class ExportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Cancelled {};

std::filesystem::path with_suffix(const std::filesystem::path& path, const wchar_t* suffix) {
    std::filesystem::path out = path;
    out += suffix;
    return out;
}

// Deletes the export's temporary files when the export ends, however it ends. Declared before
// the Media Foundation objects that hold the files open, so those are released first.
class TempFiles {
public:
    explicit TempFiles(std::vector<std::filesystem::path> paths) : paths_(std::move(paths)) {}
    ~TempFiles() { remove(); }
    TempFiles(const TempFiles&) = delete;
    TempFiles& operator=(const TempFiles&) = delete;
    void remove() noexcept {
        for (const std::filesystem::path& path : paths_) {
            DeleteFileW(path.c_str());
        }
    }

private:
    std::vector<std::filesystem::path> paths_;
};

bool disk_full(HRESULT hr) {
    return hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL) || hr == HRESULT_FROM_WIN32(ERROR_HANDLE_DISK_FULL);
}

void check_cancel(const std::atomic<bool>& cancel) {
    if (cancel.load(std::memory_order_relaxed)) {
        throw Cancelled{};
    }
}

Time100ns to_ticks(double seconds) {
    return static_cast<Time100ns>(std::llround(seconds * static_cast<double>(kTicksPerSecond)));
}

double to_seconds(Time100ns ticks) {
    return static_cast<double>(ticks) / static_cast<double>(kTicksPerSecond);
}

std::string megabytes(std::uint64_t bytes) {
    return std::format("{:.1f} MB", static_cast<double>(bytes) / 1'000'000.0);
}

// The graphics card for decoding, scaling and encoding: its own device, so an export never
// touches the recorder's or the player's.
struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<IMFDXGIDeviceManager> manager;
};

Gpu make_gpu() {
    Gpu gpu;
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    BT_CHECK_HR(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                  static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &gpu.device, nullptr,
                                  nullptr));
    // The decoder, the video processor and the encoder call into the device from Media
    // Foundation's own threads.
    ComPtr<ID3D11Multithread> multithread;
    BT_CHECK_HR(gpu.device.As(&multithread));
    multithread->SetMultithreadProtected(TRUE);
    UINT token = 0;
    BT_CHECK_HR(MFCreateDXGIDeviceManager(&token, &gpu.manager));
    BT_CHECK_HR(gpu.manager->ResetDevice(gpu.device.Get(), token));
    return gpu;
}

ComPtr<IMFSourceReader> open_reader(const std::filesystem::path& file, IMFDXGIDeviceManager* manager) {
    ComPtr<IMFAttributes> attributes;
    BT_CHECK_HR(MFCreateAttributes(&attributes, 3));
    // The advanced video processor scales and converts; with a device manager it runs on the
    // graphics card (DXVA), without one on the CPU.
    BT_CHECK_HR(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE));
    if (manager != nullptr) {
        BT_CHECK_HR(attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, manager));
        BT_CHECK_HR(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));
    }
    ComPtr<IMFSourceReader> reader;
    const HRESULT hr = MFCreateSourceReaderFromURL(file.c_str(), attributes.Get(), &reader);
    if (FAILED(hr)) {
        log::warn("Export: cannot open {}: {}", to_utf8(file.filename().wstring()), hresult_text(hr));
        throw ExportError("the clip could not be opened");
    }
    return reader;
}

struct Streams {
    DWORD video = 0;
    std::optional<DWORD> audio;  // the first sound track: the mix of game sound and microphone
    UINT32 fps_num = 60;
    UINT32 fps_den = 1;
    ExportSource info;
};

// The kind of each track of an MP4 file, in the file's order, read from its moov box. Empty when
// the file is not an MP4 Backtrack understands; find_streams then falls back to the measured order.
std::vector<TrackKind> file_track_kinds(const std::filesystem::path& file) {
    constexpr std::uint64_t kMaxMoovBytes = 64ull * 1024 * 1024;
    constexpr std::size_t kLargestBoxHeader = 16;
    std::error_code ec;
    const std::uint64_t file_size = std::filesystem::file_size(file, ec);
    if (ec) return {};
    std::ifstream in(file, std::ios::binary);
    for (std::uint64_t at = 0; at < file_size;) {
        std::uint8_t header[kLargestBoxHeader] = {};
        const auto available = static_cast<std::size_t>(std::min<std::uint64_t>(sizeof(header), file_size - at));
        in.seekg(static_cast<std::streamoff>(at));
        in.read(reinterpret_cast<char*>(header), static_cast<std::streamsize>(available));
        const auto box = mp4::read_box_header(std::span<const std::uint8_t>(header, available), 0, file_size - at);
        if (!in || !box || box->size < 8 || box->size > file_size - at) return {};
        if (box->type == "moov") {
            if (box->size > kMaxMoovBytes) return {};
            std::vector<std::uint8_t> moov(static_cast<std::size_t>(box->size));
            in.seekg(static_cast<std::streamoff>(at));
            in.read(reinterpret_cast<char*>(moov.data()), static_cast<std::streamsize>(moov.size()));
            if (!in) return {};
            return mp4_track_kinds(moov);
        }
        at += box->size;
    }
    return {};
}

Streams find_streams(IMFSourceReader* reader, const std::filesystem::path& file) {
    Streams s;
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
    // The first track of each kind; for sound that is the mix of game sound and microphone. When
    // the file's own order cannot be read, the reader's streams are taken as last to first, as
    // Media Foundation lists them (measured).
    const std::vector<TrackKind> file_kinds = file_track_kinds(file);
    const auto pick = [&](TrackKind kind) -> std::optional<DWORD> {
        if (const auto mapped = reader_stream_of_first_track(file_kinds, reader_kinds, kind)) {
            return static_cast<DWORD>(*mapped);
        }
        for (std::size_t i = reader_kinds.size(); i-- > 0;) {
            if (reader_kinds[i] == kind) return static_cast<DWORD>(i);
        }
        return std::nullopt;
    };
    const std::optional<DWORD> video = pick(TrackKind::Video);
    s.audio = pick(TrackKind::Audio);
    UINT32 audio_bytes_per_second = 0;
    if (video) {
        IMFMediaType* native = natives[*video].Get();
        s.video = *video;
        const VisibleArea area = visible_area(native);
        s.info.width = static_cast<int>(area.width);
        s.info.height = static_cast<int>(area.height);
        UINT32 num = 0;
        UINT32 den = 0;
        // A variable frame rate file may state a time base (90000/1) instead of a rate; the encoder
        // would refuse it or make an absurd keyframe interval. Such a file counts as 60 fps.
        if (SUCCEEDED(MFGetAttributeRatio(native, MF_MT_FRAME_RATE, &num, &den)) && num > 0 && den > 0 &&
            num / den >= 1 && num / den <= 1000) {
            s.fps_num = num;
            s.fps_den = den;
        }
        s.info.fps = static_cast<double>(s.fps_num) / s.fps_den;
        s.info.video_kbps = static_cast<int>(MFGetAttributeUINT32(native, MF_MT_AVG_BITRATE, 0) / 1000);
    }
    if (s.audio) {
        audio_bytes_per_second = MFGetAttributeUINT32(natives[*s.audio].Get(), MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 0);
    }
    log::debug("Export: {} streams, {} tracks in the file; video is stream {}, sound stream {}", reader_kinds.size(),
               file_kinds.size(), video ? static_cast<int>(*video) : -1, s.audio ? static_cast<int>(*s.audio) : -1);
    const bool have_video = video.has_value();
    if (!have_video || s.info.width <= 0 || s.info.height <= 0) {
        throw ExportError("the file has no video");
    }
    s.info.has_audio = s.audio.has_value();
    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION,
                                                   &value))) {
        ULONGLONG duration = 0;
        if (SUCCEEDED(PropVariantToUInt64(value, &duration))) {
            s.info.duration_s = to_seconds(static_cast<Time100ns>(duration));
        }
        PropVariantClear(&value);
    }
    if (s.info.video_kbps <= 0 && s.info.duration_s > 0.0) {
        // The MP4 source does not always say; the file size minus the sound is close enough.
        std::error_code ec;
        const auto bytes = std::filesystem::file_size(file, ec);
        if (!ec) {
            const double total = static_cast<double>(bytes) * 8.0 / s.info.duration_s / 1000.0;
            const double kbps = total - audio_bytes_per_second * 8.0 / 1000.0;
            s.info.video_kbps = kbps > 0.0 ? static_cast<int>(kbps) : 0;
        }
    }
    return s;
}

ComPtr<IMFMediaType> set_video_output(IMFSourceReader* reader, DWORD stream, const GUID& subtype, int width,
                                      int height, UINT32 fps_num, UINT32 fps_den) {
    ComPtr<IMFMediaType> type;
    BT_CHECK_HR(MFCreateMediaType(&type));
    BT_CHECK_HR(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    BT_CHECK_HR(type->SetGUID(MF_MT_SUBTYPE, subtype));
    BT_CHECK_HR(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width), static_cast<UINT32>(height)));
    BT_CHECK_HR(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fps_num, fps_den));
    BT_CHECK_HR(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    BT_CHECK_HR(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    BT_CHECK_HR(reader->SetCurrentMediaType(stream, nullptr, type.Get()));
    ComPtr<IMFMediaType> current;
    BT_CHECK_HR(reader->GetCurrentMediaType(stream, &current));
    UINT32 w = 0;
    UINT32 h = 0;
    BT_CHECK_HR(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h));
    if (w != static_cast<UINT32>(width) || h != static_cast<UINT32>(height)) {
        throw std::runtime_error(std::format("the video processor gives {}x{} instead of {}x{}", w, h, width, height));
    }
    return current;
}

// 16-bit PCM at 44.1 or 48 kHz in mono or stereo: what Microsoft's AAC encoder takes.
ComPtr<IMFMediaType> set_audio_output(IMFSourceReader* reader, DWORD stream) {
    ComPtr<IMFMediaType> native;
    BT_CHECK_HR(reader->GetNativeMediaType(stream, 0, &native));
    UINT32 rate = MFGetAttributeUINT32(native.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 48'000);
    if (rate != 44'100 && rate != 48'000) {
        rate = 48'000;
    }
    UINT32 channels = MFGetAttributeUINT32(native.Get(), MF_MT_AUDIO_NUM_CHANNELS, 2);
    if (channels != 1) {
        channels = 2;
    }
    ComPtr<IMFMediaType> type;
    BT_CHECK_HR(MFCreateMediaType(&type));
    BT_CHECK_HR(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    BT_CHECK_HR(type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
    BT_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, kBitsPerSample));
    BT_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate));
    BT_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels));
    BT_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * kBitsPerSample / 8));
    BT_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * channels * kBitsPerSample / 8));
    const HRESULT hr = reader->SetCurrentMediaType(stream, nullptr, type.Get());
    if (FAILED(hr)) {
        log::warn("Export: the sound cannot be decoded to {} Hz {} channel PCM: {}", rate, channels, hresult_text(hr));
        throw ExportError("the clip's sound cannot be converted");
    }
    ComPtr<IMFMediaType> current;
    BT_CHECK_HR(reader->GetCurrentMediaType(stream, &current));
    return current;
}

// Reads the piece [start, end) of the clip: hands every video frame that belongs to it (dropping
// frames down to `fps` when asked) to `on_video` with its time since the piece's first frame, and
// every piece of sound with its time on the same timeline. The first frame is the one on screen
// at `start`; the timeline's zero is that frame, so picture and sound keep their exact offset.
class RangeReader {
public:
    using VideoFn = std::function<void(IMFSample* sample, Time100ns time)>;
    using AudioFn = std::function<void(IMFSample* sample, Time100ns time)>;

    RangeReader(IMFSourceReader* reader, DWORD video, std::optional<DWORD> audio, Time100ns start, Time100ns end,
                Time100ns frame_duration, double decimate_to_fps, const std::atomic<bool>& cancel)
        : reader_(reader), video_(video), audio_(audio), start_(start), end_(end), frame_duration_(frame_duration),
          decimator_(decimate_to_fps), cancel_(cancel) {}

    void run(const VideoFn& on_video, const AudioFn& on_audio) {
        BT_CHECK_HR(reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
        BT_CHECK_HR(reader_->SetStreamSelection(video_, TRUE));
        if (audio_) {
            BT_CHECK_HR(reader_->SetStreamSelection(*audio_, TRUE));
        }
        {
            // The video processor's output size is fixed; a type change that alters it would
            // hand the encoder frames of the wrong size.
            ComPtr<IMFMediaType> current;
            BT_CHECK_HR(reader_->GetCurrentMediaType(video_, &current));
            BT_CHECK_HR(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &expected_width_, &expected_height_));
        }
        if (start_ > 0) {
            seek(start_);
        }
        bool video_done = false;
        audio_done_ = !audio_;
        int empty_reads = 0;
        while (!video_done || !audio_done_) {
            check_cancel(cancel_);
            if (video_done && !zero_) {
                break;  // no frame of the piece: the sound alone is of no use
            }
            DWORD index = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            const auto read_started = std::chrono::steady_clock::now();
            BT_CHECK_HR(reader_->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, &index, &flags,
                                            &timestamp, &sample));
            read_time_ += std::chrono::steady_clock::now() - read_started;
            if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
                throw std::runtime_error("the source reader reported a stream error");
            }
            const bool is_video = index == video_;
            const bool is_audio = audio_ && index == *audio_;
            if (is_video && (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
                check_video_size();
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                video_done = video_done || is_video;
                audio_done_ = audio_done_ || is_audio;
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
                continue;  // nothing to place it by
            }
            LONGLONG duration = 0;
            if (FAILED(sample->GetSampleDuration(&duration)) || duration <= 0) {
                duration = is_video ? frame_duration_ : 0;
            }
            if (is_video) {
                if (seeked_ && !reseeked_ && !zero_ && first_after_seek_ && pts > start_ + kFrameSlack) {
                    // The first frame after the seek lies past the frame on screen at `start`: the
                    // source seeked to the nearest keyframe, not the one before. Checked before
                    // anything else, since that frame may even lie past the end of a short piece.
                    log::info("Export: the seek to {:.3f} s landed on {:.3f} s; reading from the start",
                              to_seconds(start_), to_seconds(pts));
                    reseeked_ = true;
                    waiting_audio_.clear();
                    if (audio_ && audio_done_) {
                        BT_CHECK_HR(reader_->SetStreamSelection(*audio_, TRUE));
                        audio_done_ = false;
                    }
                    seek(0);
                    continue;
                }
                first_after_seek_ = false;
                if (pts >= end_) {
                    video_done = true;
                    BT_CHECK_HR(reader_->SetStreamSelection(video_, FALSE));
                    continue;
                }
                if (!zero_) {
                    if (!frame_in_range(pts, duration, start_, end_)) {
                        continue;
                    }
                    zero_ = pts;
                    for (const ComPtr<IMFSample>& waiting : waiting_audio_) {
                        deliver_audio(waiting.Get(), on_audio);
                    }
                    waiting_audio_.clear();
                }
                last_video_end_ = std::max(last_video_end_, pts + duration - *zero_);
                if (decimator_.keep(pts - *zero_)) {
                    any_video_ = true;
                    on_video(sample.Get(), pts - *zero_);
                }
            } else {
                // Sound past the last frame is left out: the MP4 sink would wait for picture to
                // interleave it with, and the export ends with the picture anyway.
                if (pts >= end_ || (video_done && zero_ && pts - *zero_ >= last_video_end_)) {
                    audio_done_ = true;
                    BT_CHECK_HR(reader_->SetStreamSelection(*audio_, FALSE));
                    continue;
                }
                if (!zero_) {
                    // Sound that ends before the piece can start is of no use: the first frame
                    // starts at most a frame before `start`. Without this, reading a long way up
                    // to `start` would queue all of it.
                    if (pts + duration <= start_ - frame_duration_ - kFrameSlack) {
                        continue;
                    }
                    if (waiting_audio_.size() >= kMaxWaitingAudioSamples) {
                        throw std::runtime_error("the sound runs far ahead of the video");
                    }
                    waiting_audio_.push_back(sample);
                    continue;
                }
                deliver_audio(sample.Get(), on_audio);
            }
        }
        if (!any_video_) {
            throw ExportError("the selection starts after the end of the video");
        }
        log::debug("Export: piece starts at {:.6f} s on the clip's timeline, video read to {:.6f} s", to_seconds(*zero_),
                   to_seconds(*zero_ + last_video_end_));
    }

    // On the piece's timeline: where the last frame read ends.
    Time100ns video_end() const { return last_video_end_; }
    // The sound track has ended (or there is none); the picture may go on.
    bool audio_finished() const { return audio_done_; }
    // Time spent waiting for decoded frames and sound, for the log.
    std::chrono::steady_clock::duration read_time() const { return read_time_; }
    // The piece's zero on the clip's timeline (the first frame); valid after run().
    Time100ns zero() const { return zero_.value_or(start_); }

private:
    void seek(Time100ns position) {
        PROPVARIANT value;
        BT_CHECK_HR(InitPropVariantFromInt64(position, &value));
        const HRESULT hr = reader_->SetCurrentPosition(GUID_NULL, value);
        PropVariantClear(&value);
        BT_CHECK_HR(hr);
        seeked_ = position > 0;
        first_after_seek_ = true;
    }

    void check_video_size() {
        ComPtr<IMFMediaType> current;
        BT_CHECK_HR(reader_->GetCurrentMediaType(video_, &current));
        UINT32 w = 0;
        UINT32 h = 0;
        MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h);
        if (w != expected_width_ || h != expected_height_) {
            throw ExportError("the video changes size partway through");
        }
    }

    void deliver_audio(IMFSample* sample, const AudioFn& on_audio) {
        LONGLONG pts = 0;
        if (SUCCEEDED(sample->GetSampleTime(&pts))) {
            on_audio(sample, pts - *zero_);
        }
    }

    IMFSourceReader* reader_;
    DWORD video_;
    std::optional<DWORD> audio_;
    Time100ns start_;
    Time100ns end_;
    Time100ns frame_duration_;
    FrameDecimator decimator_;
    const std::atomic<bool>& cancel_;
    std::optional<Time100ns> zero_;
    std::vector<ComPtr<IMFSample>> waiting_audio_;
    Time100ns last_video_end_ = 0;
    bool any_video_ = false;
    bool audio_done_ = false;
    bool seeked_ = false;
    bool reseeked_ = false;
    bool first_after_seek_ = false;
    UINT32 expected_width_ = 0;
    UINT32 expected_height_ = 0;
    std::chrono::steady_clock::duration read_time_{};
};

// Calls the caller's progress callback at most every 100 ms, always with a rising fraction.
class ProgressReporter {
public:
    ProgressReporter(const ExportProgress& progress, Time100ns length) : progress_(progress), length_(length) {}
    void at(Time100ns time) {
        if (!progress_ || length_ <= 0) return;
        const double fraction = std::clamp(static_cast<double>(time) / static_cast<double>(length_), 0.0, 1.0);
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
    const ExportProgress& progress_;
    Time100ns length_;
    double last_ = -1.0;
    std::chrono::steady_clock::time_point last_call_{};
};

// Writes decoded sound to the sink writer as one gapless track: gaps in the source become
// silence and overlaps are cut, so the sound never drifts against the picture.
class PcmWriter {
public:
    PcmWriter(IMFSinkWriter* writer, DWORD stream, IMFMediaType* pcm)
        : writer_(writer), stream_(stream), rate_(MFGetAttributeUINT32(pcm, MF_MT_AUDIO_SAMPLES_PER_SECOND, 48'000)),
          block_(MFGetAttributeUINT32(pcm, MF_MT_AUDIO_BLOCK_ALIGNMENT, 4)) {
        if (rate_ == 0 || block_ == 0) {
            throw std::runtime_error("the decoded sound has no sample rate");
        }
    }

    void set_end(Time100ns end) { end_frames_ = frames_at(end); }

    void add(IMFSample* sample, Time100ns time) {
        ComPtr<IMFMediaBuffer> buffer;
        BT_CHECK_HR(sample->ConvertToContiguousBuffer(&buffer));
        BYTE* data = nullptr;
        DWORD length = 0;
        BT_CHECK_HR(buffer->Lock(&data, nullptr, &length));
        try {
            const std::int64_t frames = length / block_;
            const PcmStep step = place_pcm(written_, frames_at(time), frames, end_frames_, rate_ / 1000);
            write_silence(step.silence);
            if (step.take > 0) {
                write(data + static_cast<std::size_t>(step.skip) * block_, step.take);
            }
        } catch (...) {
            buffer->Unlock();
            throw;
        }
        BT_CHECK_HR(buffer->Unlock());
    }

    // Pads the track with silence up to `time`: the sound then lasts as long as the picture, and
    // a piece whose sound starts after it still gets a valid (silent) track.
    void pad_to(Time100ns time) {
        const std::int64_t target = std::min(frames_at(time), end_frames_);
        write_silence(target - written_);
    }

    Time100ns written_time() const { return written_ * kTicksPerSecond / rate_; }

private:
    std::int64_t frames_at(Time100ns time) const {
        // time * rate stays far inside int64 for any clip length (10^13 * 48000 < 9 * 10^18).
        return (time * static_cast<std::int64_t>(rate_) + kTicksPerSecond / 2) / kTicksPerSecond;
    }

    void write_silence(std::int64_t frames) {
        const std::int64_t chunk = rate_ / 10;
        while (frames > 0) {
            const std::int64_t n = std::min(frames, chunk);
            if (silence_.size() < static_cast<std::size_t>(n) * block_) {
                silence_.assign(static_cast<std::size_t>(chunk) * block_, 0);
            }
            write(silence_.data(), n);
            frames -= n;
        }
    }

    void write(const BYTE* data, std::int64_t frames) {
        const auto bytes = static_cast<DWORD>(frames * block_);
        ComPtr<IMFMediaBuffer> buffer;
        BT_CHECK_HR(MFCreateMemoryBuffer(bytes, &buffer));
        BYTE* out = nullptr;
        BT_CHECK_HR(buffer->Lock(&out, nullptr, nullptr));
        std::memcpy(out, data, bytes);
        BT_CHECK_HR(buffer->Unlock());
        BT_CHECK_HR(buffer->SetCurrentLength(bytes));
        ComPtr<IMFSample> sample;
        BT_CHECK_HR(MFCreateSample(&sample));
        BT_CHECK_HR(sample->AddBuffer(buffer.Get()));
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
    UINT32 block_;
    std::int64_t end_frames_ = 0;
    std::int64_t written_ = 0;
    std::vector<BYTE> silence_;
};

// The video encoder the sink writer picked, for the log and the result.
std::string encoder_name(IMFSinkWriter* writer, DWORD stream, bool& hardware) {
    hardware = false;
    ComPtr<IMFSinkWriterEx> ex;
    if (FAILED(writer->QueryInterface(IID_PPV_ARGS(&ex)))) {
        return "unknown encoder";
    }
    for (DWORD i = 0;; ++i) {
        GUID category{};
        ComPtr<IMFTransform> transform;
        if (FAILED(ex->GetTransformForStream(stream, i, &category, &transform))) {
            return "unknown encoder";
        }
        if (category != MFT_CATEGORY_VIDEO_ENCODER) {
            continue;
        }
        ComPtr<IMFAttributes> attributes;
        if (FAILED(transform->GetAttributes(&attributes))) {
            return "an encoder without attributes";
        }
        UINT32 length = 0;
        hardware = SUCCEEDED(attributes->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &length));
        ComPtr<ICodecAPI> codec;
        if (SUCCEEDED(transform.As(&codec))) {
            VARIANT mode;
            VARIANT mean;
            VariantInit(&mode);
            VariantInit(&mean);
            const HRESULT mode_hr = codec->GetValue(&CODECAPI_AVEncCommonRateControlMode, &mode);
            const HRESULT mean_hr = codec->GetValue(&CODECAPI_AVEncCommonMeanBitRate, &mean);
            log::debug("Export: encoder rate control mode {} ({}), mean bitrate {} ({})",
                       mode.vt == VT_UI4 ? static_cast<long long>(mode.ulVal) : -1, hresult_text(mode_hr),
                       mean.vt == VT_UI4 ? static_cast<long long>(mean.ulVal) : -1, hresult_text(mean_hr));
            VariantClear(&mode);
            VariantClear(&mean);
        }
        wchar_t* name = nullptr;
        if (SUCCEEDED(attributes->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &length))) {
            std::string out = to_utf8(std::wstring_view(name, length));
            CoTaskMemFree(name);
            return out;
        }
        return hardware ? "a hardware encoder" : "Microsoft H.264 software encoder";
    }
}

struct PassResult {
    std::filesystem::path file;  // the finished temporary file
    std::uint64_t bytes = 0;
    double seconds = 0.0;
    bool hardware = false;
};

// "Fast start", the same idea as ClipWriter's: a copy of `in` with the moov box (the index) in
// front of the media data, so Discord's preview and phones start playing before the whole file
// is downloaded. False when the layout is not [header boxes, mdat, moov]; the file as written
// then plays everywhere, only not while it is still downloading.
bool move_index_to_front(const std::filesystem::path& in_path, const std::filesystem::path& out_path) {
    constexpr std::uint64_t kMaxMoovBytes = 64ull * 1024 * 1024;
    constexpr std::size_t kCopyChunkBytes = 1024 * 1024;
    constexpr std::size_t kLargestBoxHeader = 16;
    std::error_code ec;
    const std::uint64_t file_size = std::filesystem::file_size(in_path, ec);
    if (ec) return false;
    std::ifstream in(in_path, std::ios::binary);
    std::vector<mp4::BoxHeader> boxes;
    for (std::uint64_t at = 0; at < file_size;) {
        std::uint8_t header[kLargestBoxHeader] = {};
        const auto available = static_cast<std::size_t>(std::min<std::uint64_t>(sizeof(header), file_size - at));
        in.seekg(static_cast<std::streamoff>(at));
        in.read(reinterpret_cast<char*>(header), static_cast<std::streamsize>(available));
        auto box = mp4::read_box_header(std::span<const std::uint8_t>(header, available), 0, file_size - at);
        if (!in || !box || box->size > file_size - at) return false;
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
    if (!in) return false;
    const auto moved = mp4::shift_chunk_offsets(moov_bytes, moov.size);
    if (!moved) return false;
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    const auto copy_range = [&](std::uint64_t from, std::uint64_t to) {
        std::vector<char> chunk(kCopyChunkBytes);
        in.seekg(static_cast<std::streamoff>(from));
        for (std::uint64_t at = from; at < to && in && out;) {
            const auto n = static_cast<std::streamsize>(std::min<std::uint64_t>(chunk.size(), to - at));
            in.read(chunk.data(), n);
            out.write(chunk.data(), n);
            at += static_cast<std::uint64_t>(n);
        }
        return static_cast<bool>(in) && static_cast<bool>(out);
    };
    bool ok = copy_range(0, mdat.offset);
    out.write(reinterpret_cast<const char*>(moved->data()), static_cast<std::streamsize>(moved->size()));
    ok = ok && copy_range(mdat.offset, moov.offset);
    out.close();
    return ok && !out.fail();
}

struct Job {
    std::filesystem::path source;
    std::filesystem::path temp;
    std::filesystem::path fast;
    std::string name;  // the clip's file name, for the log
    Streams streams;
    const std::atomic<bool>* cancel = nullptr;
    const ExportProgress* progress = nullptr;
    bool to_end = false;  // the piece runs to the end of the clip
};

void set_codec_param(IMFAttributes* params, const GUID& key, UINT32 value) {
    BT_CHECK_HR(params->SetUINT32(key, value));
}

PassResult run_mp4_pass(const Job& job, const ExportPlan& plan, std::optional<Gpu> gpu, bool hardware_encoder) {
    const Time100ns start = to_ticks(plan.start_s);
    // A piece that runs to the end is read to the end of the video, not to the file's stated length:
    // with B-frames the Media Foundation source starts the video a frame or two late, and the last
    // frame then lies past the length (measured on files written by ffmpeg).
    const Time100ns end = job.to_end ? kOpenEnd : to_ticks(plan.end_s);
    const Streams& s = job.streams;
    // The output frame rate: the clip's own exact ratio (59.94 stays 59.94), or a whole number
    // when frames are dropped.
    UINT32 fps_num = s.fps_num;
    UINT32 fps_den = s.fps_den;
    if (!plan.every_frame) {
        fps_num = static_cast<UINT32>(std::lround(plan.fps));
        fps_den = 1;
    }
    const Time100ns out_frame = std::max<Time100ns>(1, kTicksPerSecond * fps_den / fps_num);
    const Time100ns source_frame = std::max<Time100ns>(1, kTicksPerSecond * s.fps_den / s.fps_num);
    const bool with_audio = plan.audio_kbps > 0 && s.audio.has_value();

    ComPtr<IMFSourceReader> reader = open_reader(job.source, gpu ? gpu->manager.Get() : nullptr);
    ComPtr<IMFMediaType> nv12 =
        set_video_output(reader.Get(), s.video, MFVideoFormat_NV12, plan.width, plan.height, s.fps_num, s.fps_den);
    ComPtr<IMFMediaType> pcm = with_audio ? set_audio_output(reader.Get(), *s.audio) : nullptr;

    ComPtr<IMFAttributes> attributes;
    BT_CHECK_HR(MFCreateAttributes(&attributes, 3));
    // The .tmp extension would not tell the sink writer which container to use.
    BT_CHECK_HR(attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));
    hardware_encoder = hardware_encoder && gpu.has_value();
    BT_CHECK_HR(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware_encoder ? TRUE : FALSE));
    if (hardware_encoder) {
        BT_CHECK_HR(attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, gpu->manager.Get()));
    }
    // Throttling stays on: WriteSample waits while the encoder is behind, so the reader never
    // decodes far ahead into graphics memory, whatever the encoder holds back internally (the
    // software encoder holds more than 8 frames on this PC before the first comes out; measured).
    // The price: the MP4 sink interleaves, so neither track may stop while the other goes on.
    // RangeReader ends the sound with the picture, and an ended sound track is padded with
    // silence as the picture goes on.
    ComPtr<IMFByteStream> file;
    BT_CHECK_HR(
        MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_DELETE_IF_EXIST, MF_FILEFLAGS_NONE, job.temp.c_str(), &file));
    // On a failure or cancel the sink writer is let go without Finalize, and the encoder's work
    // queue may still hold the file for a moment; Close releases the handle whoever holds a
    // reference, so the temporary file can be deleted (or opened again by the software retry).
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

    ComPtr<IMFMediaType> h264;
    BT_CHECK_HR(MFCreateMediaType(&h264));
    BT_CHECK_HR(h264->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    BT_CHECK_HR(h264->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
    BT_CHECK_HR(MFSetAttributeSize(h264.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(plan.width),
                                   static_cast<UINT32>(plan.height)));
    BT_CHECK_HR(MFSetAttributeRatio(h264.Get(), MF_MT_FRAME_RATE, fps_num, fps_den));
    BT_CHECK_HR(MFSetAttributeRatio(h264.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    BT_CHECK_HR(h264->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    BT_CHECK_HR(h264->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(plan.video_kbps) * 1000));
    BT_CHECK_HR(h264->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High));
    // Players are told the colours are BT.709 limited range, what the video processor makes from
    // an HD clip, so they do not guess BT.601 and show it washed out.
    BT_CHECK_HR(h264->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
    BT_CHECK_HR(h264->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
    BT_CHECK_HR(h264->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
    BT_CHECK_HR(h264->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));
    DWORD video_stream = 0;
    BT_CHECK_HR(writer->AddStream(h264.Get(), &video_stream));

    ComPtr<IMFMediaType> input;
    BT_CHECK_HR(MFCreateMediaType(&input));
    BT_CHECK_HR(nv12->CopyAllItems(input.Get()));
    BT_CHECK_HR(MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, fps_num, fps_den));
    ComPtr<IMFAttributes> params;
    BT_CHECK_HR(MFCreateAttributes(&params, 4));
    const auto mean = static_cast<UINT32>(plan.video_kbps) * 1000;
    if (plan.constant_bitrate) {
        set_codec_param(params.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
    } else {
        set_codec_param(params.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_PeakConstrainedVBR);
        set_codec_param(params.Get(), CODECAPI_AVEncCommonMaxBitRate, static_cast<UINT32>(mean * kPeakToMean));
    }
    set_codec_param(params.Get(), CODECAPI_AVEncCommonMeanBitRate, mean);
    // No B-frames: with them Microsoft's encoder shifts every frame one frame later and the MP4
    // sink writes no edit list to undo it, so the picture lagged the sound by a frame (measured:
    // video start_time 0.0167 s at 60 fps).
    set_codec_param(params.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    set_codec_param(params.Get(), CODECAPI_AVEncMPVGOPSize,
                    static_cast<UINT32>(std::max(1.0, std::round(kGopSeconds * fps_num / fps_den))));
    HRESULT hr = writer->SetInputMediaType(video_stream, input.Get(), params.Get());
    if (FAILED(hr)) {
        // An encoder that refuses one of the settings still gets the bitrate from the type.
        log::info("Export: the encoder refused the rate settings ({}), using its defaults", hresult_text(hr));
        hr = writer->SetInputMediaType(video_stream, input.Get(), nullptr);
    }
    BT_CHECK_HR(hr);

    DWORD audio_stream = 0;
    if (with_audio) {
        ComPtr<IMFMediaType> aac;
        BT_CHECK_HR(MFCreateMediaType(&aac));
        BT_CHECK_HR(aac->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
        BT_CHECK_HR(aac->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC));
        BT_CHECK_HR(aac->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, kBitsPerSample));
        BT_CHECK_HR(aac->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, MFGetAttributeUINT32(pcm.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 48'000)));
        BT_CHECK_HR(aac->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, MFGetAttributeUINT32(pcm.Get(), MF_MT_AUDIO_NUM_CHANNELS, 2)));
        BT_CHECK_HR(aac->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(plan.audio_kbps) * 1000 / 8));
        BT_CHECK_HR(aac->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, kAacPayloadRaw));
        BT_CHECK_HR(aac->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, kAacLcLevel));
        BT_CHECK_HR(writer->AddStream(aac.Get(), &audio_stream));
        hr = writer->SetInputMediaType(audio_stream, pcm.Get(), nullptr);
        if (FAILED(hr)) {
            log::warn("Export: the AAC encoder refuses the sound: {}", hresult_text(hr));
            throw ExportError("the clip's sound cannot be encoded");
        }
    }
    BT_CHECK_HR(writer->BeginWriting());
    PassResult result;
    const std::string encoder = encoder_name(writer.Get(), video_stream, result.hardware);
    log::info("Export: {} {}x{} at {:.2f} fps, {} kbit/s {}, sound {} kbit/s, {} decode, encoder {}", job.name,
              plan.width, plan.height, static_cast<double>(fps_num) / fps_den, plan.video_kbps,
              plan.constant_bitrate ? "CBR" : "VBR", plan.audio_kbps, gpu ? "GPU" : "CPU", encoder);

    std::optional<PcmWriter> pcm_writer;
    if (with_audio) {
        pcm_writer.emplace(writer.Get(), audio_stream, pcm.Get());
    }
    ProgressReporter progress(*job.progress, to_ticks(plan.end_s) - start);
    RangeReader range(reader.Get(), s.video, with_audio ? s.audio : std::nullopt, start, end, source_frame,
                      plan.every_frame ? 0.0 : plan.fps, *job.cancel);
    bool end_set = false;
    Time100ns written_end = 0;
    std::chrono::steady_clock::duration write_time{};
    range.run(
        [&](IMFSample* sample, Time100ns time) {
            BT_CHECK_HR(sample->SetSampleTime(time));
            BT_CHECK_HR(sample->SetSampleDuration(out_frame));
            const auto write_started = std::chrono::steady_clock::now();
            BT_CHECK_HR(writer->WriteSample(video_stream, sample));
            write_time += std::chrono::steady_clock::now() - write_started;
            written_end = time + out_frame;
            if (pcm_writer) {
                if (!end_set) {
                    pcm_writer->set_end(end - range.zero());
                    end_set = true;
                }
                if (range.audio_finished()) {
                    pcm_writer->pad_to(written_end);  // the clip's sound ended before its picture
                } else if (pcm_writer->written_time() < written_end - kMaxAudioLag) {
                    // No sound for seconds while the picture goes on (a gap in the clip's sound,
                    // or sound that starts late): silence keeps the MP4 sink from waiting for it
                    // with WriteSample blocked. Far beyond the file's own interleave (about a
                    // second), so real sound is never overwritten.
                    pcm_writer->pad_to(written_end - kAudioLagPadding);
                }
            }
            progress.at(time);
        },
        [&](IMFSample* sample, Time100ns time) {
            if (!end_set) {
                pcm_writer->set_end(end - range.zero());
                end_set = true;
            }
            pcm_writer->add(sample, time);
        });
    if (pcm_writer) {
        if (!end_set) {
            pcm_writer->set_end(end - range.zero());
        }
        pcm_writer->pad_to(std::min(written_end, end - range.zero()));
    }
    check_cancel(*job.cancel);
    const auto finalize_started = std::chrono::steady_clock::now();
    BT_CHECK_HR(writer->Finalize());
    log::debug("Export: {} ms reading, {} ms handing frames to the encoder, {} ms finishing",
               std::chrono::duration_cast<std::chrono::milliseconds>(range.read_time()).count(),
               std::chrono::duration_cast<std::chrono::milliseconds>(write_time).count(),
               std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - finalize_started)
                   .count());
    writer.Reset();
    close_file.close();
    file.Reset();
    reader.Reset();

    result.seconds = to_seconds(written_end);
    bool fast_start = false;
    try {
        fast_start = move_index_to_front(job.temp, job.fast);
    } catch (const std::exception& e) {
        log::debug("Export: moving the MP4 index failed: {}", e.what());
    }
    check_cancel(*job.cancel);  // the copy takes a few seconds for a long clip
    if (fast_start) {
        DeleteFileW(job.temp.c_str());
        result.file = job.fast;
    } else {
        DeleteFileW(job.fast.c_str());
        log::debug("Export: MP4 index left at the end");
        result.file = job.temp;
    }
    std::error_code ec;
    result.bytes = std::filesystem::file_size(result.file, ec);
    if (ec) {
        throw std::runtime_error("the exported file disappeared");
    }
    progress.done();
    return result;
}

// Copies a decoded RGB32 frame into tightly packed, top-down BGRA rows.
void copy_bgra(IMFSample* sample, IMFMediaType* type, int width, int height, std::vector<BYTE>& out) {
    out.resize(static_cast<std::size_t>(width) * height * 4);
    const std::size_t row = static_cast<std::size_t>(width) * 4;
    ComPtr<IMFMediaBuffer> buffer;
    BT_CHECK_HR(sample->GetBufferByIndex(0, &buffer));
    ComPtr<IMF2DBuffer> buffer2d;
    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    if (SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch))) {
        // Lock2D points at the top row whichever way the rows are stored; a negative pitch walks up.
        if (static_cast<std::size_t>(std::abs(static_cast<long long>(pitch))) < row) {
            buffer2d->Unlock2D();
            throw std::runtime_error("a decoded frame is narrower than its format");
        }
        for (int y = 0; y < height; ++y) {
            std::memcpy(out.data() + y * row, scan0 + static_cast<std::ptrdiff_t>(y) * pitch, row);
        }
        BT_CHECK_HR(buffer2d->Unlock2D());
        return;
    }
    // A plain buffer: the stride is in the media type, and RGB without one is stored bottom-up.
    ComPtr<IMFMediaBuffer> contiguous;
    BT_CHECK_HR(sample->ConvertToContiguousBuffer(&contiguous));
    auto stride = static_cast<LONG>(MFGetAttributeUINT32(type, MF_MT_DEFAULT_STRIDE, 0));
    if (stride == 0) {
        stride = -static_cast<LONG>(row);
    }
    BYTE* data = nullptr;
    DWORD length = 0;
    BT_CHECK_HR(contiguous->Lock(&data, nullptr, &length));
    const std::size_t abs_stride = static_cast<std::size_t>(std::abs(static_cast<long long>(stride)));
    if (abs_stride < row || length < abs_stride * static_cast<std::size_t>(height)) {
        contiguous->Unlock();
        throw std::runtime_error("a decoded frame is smaller than its format");
    }
    for (int y = 0; y < height; ++y) {
        const std::size_t source_row = stride > 0 ? static_cast<std::size_t>(y) : static_cast<std::size_t>(height - 1 - y);
        std::memcpy(out.data() + y * row, data + source_row * abs_stride, row);
    }
    BT_CHECK_HR(contiguous->Unlock());
}

PROPVARIANT bytes_variant(UCHAR* data, ULONG count) {
    PROPVARIANT value;
    PropVariantInit(&value);
    value.vt = VT_UI1 | VT_VECTOR;
    value.caub.cElems = count;
    value.caub.pElems = data;  // borrowed: never PropVariantClear'ed
    return value;
}

class GifWriter {
public:
    GifWriter(const std::filesystem::path& path, int width, int height) : width_(width), height_(height) {
        BT_CHECK_HR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory_)));
        BT_CHECK_HR(factory_->CreateStream(&stream_));
        BT_CHECK_HR(stream_->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
        BT_CHECK_HR(factory_->CreateEncoder(GUID_ContainerFormatGif, nullptr, &encoder_));
        BT_CHECK_HR(encoder_->Initialize(stream_.Get(), WICBitmapEncoderNoCache));
        // Loop forever: the NETSCAPE2.0 application extension with a loop count of 0.
        ComPtr<IWICMetadataQueryWriter> query;
        BT_CHECK_HR(encoder_->GetMetadataQueryWriter(&query));
        static UCHAR application[] = {'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0'};
        static UCHAR loop[] = {3, 1, 0, 0, 0};  // sub-block size, loop sub-block id, count 0 (little endian), end
        PROPVARIANT value = bytes_variant(application, static_cast<ULONG>(std::size(application)));
        BT_CHECK_HR(query->SetMetadataByName(L"/appext/Application", &value));
        value = bytes_variant(loop, static_cast<ULONG>(std::size(loop)));
        BT_CHECK_HR(query->SetMetadataByName(L"/appext/Data", &value));
        PROPVARIANT side;
        PropVariantInit(&side);
        side.vt = VT_UI2;
        side.uiVal = static_cast<USHORT>(width);
        BT_CHECK_HR(query->SetMetadataByName(L"/logscrdesc/Width", &side));
        side.uiVal = static_cast<USHORT>(height);
        BT_CHECK_HR(query->SetMetadataByName(L"/logscrdesc/Height", &side));
    }

    // Each frame gets its own 256-colour palette (a game changes colours from scene to scene) and
    // error diffusion, so gradients do not band.
    void add(std::vector<BYTE>& bgra, std::uint16_t delay_cs) {
        const UINT stride = static_cast<UINT>(width_) * 4;
        ComPtr<IWICBitmap> bitmap;
        BT_CHECK_HR(factory_->CreateBitmapFromMemory(static_cast<UINT>(width_), static_cast<UINT>(height_),
                                                     GUID_WICPixelFormat32bppBGR, stride,
                                                     static_cast<UINT>(bgra.size()), bgra.data(), &bitmap));
        ComPtr<IWICPalette> palette;
        BT_CHECK_HR(factory_->CreatePalette(&palette));
        BT_CHECK_HR(palette->InitializeFromBitmap(bitmap.Get(), 256, FALSE));
        ComPtr<IWICFormatConverter> converter;
        BT_CHECK_HR(factory_->CreateFormatConverter(&converter));
        BT_CHECK_HR(converter->Initialize(bitmap.Get(), GUID_WICPixelFormat8bppIndexed, WICBitmapDitherTypeErrorDiffusion,
                                          palette.Get(), 0.0, WICBitmapPaletteTypeCustom));
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> properties;
        BT_CHECK_HR(encoder_->CreateNewFrame(&frame, &properties));
        BT_CHECK_HR(frame->Initialize(properties.Get()));
        BT_CHECK_HR(frame->SetSize(static_cast<UINT>(width_), static_cast<UINT>(height_)));
        WICPixelFormatGUID format = GUID_WICPixelFormat8bppIndexed;
        BT_CHECK_HR(frame->SetPixelFormat(&format));
        BT_CHECK_HR(frame->SetPalette(palette.Get()));
        ComPtr<IWICMetadataQueryWriter> query;
        BT_CHECK_HR(frame->GetMetadataQueryWriter(&query));
        PROPVARIANT value;
        PropVariantInit(&value);
        value.vt = VT_UI2;
        value.uiVal = delay_cs;
        BT_CHECK_HR(query->SetMetadataByName(L"/grctlext/Delay", &value));
        value.vt = VT_UI1;
        value.bVal = 1;  // keep the frame: the next one covers it completely anyway
        BT_CHECK_HR(query->SetMetadataByName(L"/grctlext/Disposal", &value));
        BT_CHECK_HR(frame->WriteSource(converter.Get(), nullptr));
        BT_CHECK_HR(frame->Commit());
        ++frames_;
    }

    int frames() const { return frames_; }

    void finish() {
        BT_CHECK_HR(encoder_->Commit());
        encoder_.Reset();
        BT_CHECK_HR(stream_->Commit(STGC_DEFAULT));
        stream_.Reset();
    }

private:
    int width_;
    int height_;
    int frames_ = 0;
    ComPtr<IWICImagingFactory> factory_;
    ComPtr<IWICStream> stream_;
    ComPtr<IWICBitmapEncoder> encoder_;
};

PassResult run_gif_pass(const Job& job, const ExportPlan& plan, std::optional<Gpu> gpu) {
    const Time100ns start = to_ticks(plan.start_s);
    // A piece that runs to the end is read to the end of the video, not to the file's stated length:
    // with B-frames the Media Foundation source starts the video a frame or two late, and the last
    // frame then lies past the length (measured on files written by ffmpeg).
    const Time100ns end = job.to_end ? kOpenEnd : to_ticks(plan.end_s);
    const Streams& s = job.streams;
    const Time100ns source_frame = std::max<Time100ns>(1, kTicksPerSecond * s.fps_den / s.fps_num);
    const Time100ns out_frame = std::max<Time100ns>(1, to_ticks(1.0 / plan.fps));

    ComPtr<IMFSourceReader> reader = open_reader(job.source, gpu ? gpu->manager.Get() : nullptr);
    ComPtr<IMFMediaType> rgb =
        set_video_output(reader.Get(), s.video, MFVideoFormat_RGB32, plan.width, plan.height, s.fps_num, s.fps_den);
    log::info("Export: {} as a {}x{} GIF at {:.2f} fps, {} decode", job.name, plan.width, plan.height, plan.fps,
              gpu ? "GPU" : "CPU");

    PassResult result;
    result.hardware = gpu.has_value();
    std::optional<GifWriter> gif;
    gif.emplace(job.temp, plan.width, plan.height);
    ProgressReporter progress(*job.progress, to_ticks(plan.end_s) - start);
    RangeReader range(reader.Get(), s.video, std::nullopt, start, end, source_frame,
                      plan.every_frame ? 0.0 : plan.fps, *job.cancel);
    // A frame is written when the next one arrives: its delay is the time until then.
    std::vector<BYTE> held;
    std::vector<BYTE> next;
    std::optional<Time100ns> held_time;
    range.run(
        [&](IMFSample* sample, Time100ns time) {
            copy_bgra(sample, rgb.Get(), plan.width, plan.height, next);
            if (held_time) {
                gif->add(held, gif_delay_cs(*held_time, time));
            }
            std::swap(held, next);
            held_time = time;
            progress.at(time);
        },
        [](IMFSample*, Time100ns) {});
    check_cancel(*job.cancel);
    Time100ns last_end = std::min(end - range.zero(), *held_time + out_frame);
    last_end = std::max(last_end, *held_time + 1);
    gif->add(held, gif_delay_cs(*held_time, last_end));
    gif->finish();
    result.seconds = to_seconds(last_end);
    gif.reset();
    reader.Reset();

    result.file = job.temp;
    std::error_code ec;
    result.bytes = std::filesystem::file_size(result.file, ec);
    if (ec) {
        throw std::runtime_error("the exported file disappeared");
    }
    progress.done();
    return result;
}

PassResult run_pass(const Job& job, const ExportPlan& plan, bool hardware) {
    // The graphics card first; anything that goes wrong there (a driver without a video
    // processor, an encoder that refuses the size) gets a second try in software.
    if (hardware) {
        try {
            std::optional<Gpu> gpu = make_gpu();
            // Fits Discord decodes and scales on the graphics card but encodes in software: the
            // AMD encoder overshot a constant bitrate by 14 to 38 % (measured), so nearly every
            // Fits Discord export would need its second pass, while Microsoft's encoder lands
            // within a few percent and, at Discord's sizes, is as fast.
            const bool hardware_encoder = !plan.constant_bitrate;
            return plan.format == ExportFormat::Gif ? run_gif_pass(job, plan, std::move(gpu))
                                                    : run_mp4_pass(job, plan, std::move(gpu), hardware_encoder);
        } catch (const ExportError&) {
            throw;
        } catch (const std::exception& e) {
            if (const auto* hr = dynamic_cast<const HresultError*>(&e); hr != nullptr && disk_full(hr->hr())) {
                throw;
            }
            log::warn("Export on the graphics card failed, trying again in software: {}", e.what());
            DeleteFileW(job.temp.c_str());
            DeleteFileW(job.fast.c_str());
        }
    }
    return plan.format == ExportFormat::Gif ? run_gif_pass(job, plan, std::nullopt)
                                            : run_mp4_pass(job, plan, std::nullopt, false);
}

std::string success_text(const ExportPlan& plan, const PassResult& pass, const ExportOptions& options) {
    if (plan.format == ExportFormat::Gif) {
        return std::format("Saved a {} x {} GIF of {:.1f} s: {}.", plan.width, plan.height, pass.seconds,
                           megabytes(pass.bytes));
    }
    if (options.quality == ExportQuality::FitsDiscord) {
        return std::format("Saved a {} x {} MP4 of {:.1f} s that fits Discord: {}.", plan.width, plan.height,
                           pass.seconds, megabytes(pass.bytes));
    }
    return std::format("Saved a {} x {} MP4 of {:.1f} s: {}.", plan.width, plan.height, pass.seconds,
                       megabytes(pass.bytes));
}

ExportResult export_unguarded(const std::filesystem::path& source, const std::filesystem::path& destination,
                              const ExportOptions& options, const ExportProgress& progress,
                              const std::atomic<bool>& cancel) {
    ExportResult result;
    const auto fail = [&](const std::string& why) {
        result.message = "Could not export: " + why + ".";
        return result;
    };
    std::error_code ec;
    if (std::filesystem::exists(destination, ec)) {
        return fail("a file with that name already exists");
    }
    if (!std::filesystem::is_directory(destination.parent_path(), ec)) {
        return fail("the folder for the export does not exist");
    }
    Job job;
    job.source = source;
    job.temp = with_suffix(destination, kTempSuffix);
    job.fast = with_suffix(destination, kFastSuffix);
    if (std::filesystem::exists(job.temp, ec) || std::filesystem::exists(job.fast, ec)) {
        // Another export to the same name is running; its files must not be touched.
        return fail("another export to that name is still running");
    }
    job.name = to_utf8(source.filename().wstring());
    job.cancel = &cancel;
    job.progress = &progress;
    TempFiles temp_files({job.temp, job.fast});
    const auto started = std::chrono::steady_clock::now();

    try {
        {
            ComPtr<IMFSourceReader> probe = open_reader(source, nullptr);
            job.streams = find_streams(probe.Get(), source);
        }
        ExportPlan plan = plan_export(options, job.streams.info);
        job.to_end = !(options.out_seconds > 0.0) || options.out_seconds >= job.streams.info.duration_s;
        if (!plan.problem.empty()) {
            log::info("Export of {} refused: {}", job.name, plan.problem);
            return fail(plan.problem);
        }
        PassResult pass = run_pass(job, plan, options.hardware);
        const bool budgeted = options.format == ExportFormat::Mp4 && options.quality == ExportQuality::FitsDiscord;
        if (budgeted && pass.bytes > options.byte_budget) {
            const ExportPlan retry = plan_budget_retry(options, job.streams.info, plan, pass.bytes);
            log::info("Export: {} is over the {} byte budget; second try at {}x{}, {} kbit/s", megabytes(pass.bytes),
                      options.byte_budget, retry.width, retry.height, retry.video_kbps);
            if (!retry.problem.empty()) {
                return fail(retry.problem);
            }
            DeleteFileW(job.temp.c_str());
            DeleteFileW(job.fast.c_str());
            plan = retry;
            pass = run_pass(job, plan, options.hardware);
            if (pass.bytes > options.byte_budget) {
                log::warn("Export: still {} after the second try", megabytes(pass.bytes));
                return fail(std::format("it came out at {}, over the {:.0f} MB limit; try a shorter piece",
                                        megabytes(pass.bytes), static_cast<double>(options.byte_budget) / 1e6));
            }
        }
        if (!MoveFileExW(pass.file.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
            const DWORD error = GetLastError();
            log::warn("Export: renaming to {} failed: {}", to_utf8(destination.wstring()),
                      hresult_text(HRESULT_FROM_WIN32(error)));
            return fail(error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS
                            ? "a file with that name already exists"
                            : "the file could not be saved");
        }
        result.ok = true;
        result.path = destination;
        result.bytes = pass.bytes;
        result.width = plan.width;
        result.height = plan.height;
        result.fps = plan.fps;
        result.seconds = pass.seconds;
        result.hardware = pass.hardware;
        result.message = success_text(plan, pass, options);
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        log::info("Exported {} from {:.3f} s to {:.3f} s as {}: {:.3f} s, {} bytes (estimated {}), {} ms", job.name,
                  plan.start_s, plan.end_s, to_utf8(destination.filename().wstring()), pass.seconds, pass.bytes,
                  plan.estimated_bytes, ms);
        return result;
    } catch (const Cancelled&) {
        log::info("Export of {} cancelled", job.name);
        result.cancelled = true;
        result.message = "Export cancelled.";
        return result;
    } catch (const ExportError& e) {
        log::info("Export of {} refused: {}", job.name, e.what());
        return fail(e.what());
    } catch (const HresultError& e) {
        log::warn("Export of {} failed: {}", job.name, e.what());
        return fail(disk_full(e.hr()) ? "the disk is full" : "the clip could not be converted");
    } catch (const std::exception& e) {
        log::warn("Export of {} failed: {}", job.name, e.what());
        return fail("the clip could not be converted");
    }
}

}  // namespace

std::optional<ExportSource> read_export_source(const std::filesystem::path& clip) {
    try {
        ComPtr<IMFSourceReader> reader = open_reader(clip, nullptr);
        return find_streams(reader.Get(), clip).info;
    } catch (...) {
        return std::nullopt;
    }
}

// The caller is a worker thread: nothing may escape, not even from a log line or a path conversion.
ExportResult export_clip(const std::filesystem::path& source, const std::filesystem::path& destination,
                         const ExportOptions& options, const ExportProgress& progress, const std::atomic<bool>& cancel) {
    try {
        return export_unguarded(source, destination, options, progress, cancel);
    } catch (...) {
        ExportResult result;
        result.message = "Could not export: an unexpected error.";
        return result;
    }
}

std::filesystem::path free_export_path(const std::filesystem::path& folder, const std::wstring& stem,
                                       ExportFormat format) {
    for (int n = 1; n <= kMaxFreeNames; ++n) {
        const std::filesystem::path path = folder / export_file_name(stem, n, format);
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) && !ec && !std::filesystem::exists(with_suffix(path, kTempSuffix), ec) &&
            !ec && !std::filesystem::exists(with_suffix(path, kFastSuffix), ec) && !ec) {
            return path;
        }
    }
    return {};
}

}  // namespace bt
