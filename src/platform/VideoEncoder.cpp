#include "platform/VideoEncoder.h"

#include "core/H264.h"

#include <codecapi.h>
#include <mferror.h>
#include <strmif.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstring>

namespace bt {
namespace {

// Slack between the capture tick and the encoder's NeedInput events. With a game loading the GPU,
// AMD's encoder stalls for a few frames at a time; three frames of slack dropped 49 of 75,000
// frames in a 21-minute run (measured), so it holds six. A replay buffer does not care about the
// extra latency. ScreenCapture's sample pool is sized for this (§3.5).
constexpr std::size_t kMaxPendingFrames = 6;

// How long shutdown waits for the encoder's event loop to acknowledge IMFShutdown.
constexpr DWORD kShutdownTimeoutMs = 2000;

// The first keyframe intervals are logged at info level as proof of the one second GOP (§3.1).
constexpr int kKeyframeIntervalsToLog = 5;

// Half a second of refused frames at 60 fps: no longer a hiccup, the encoder is broken and the
// owner should replace it.
constexpr int kMaxConsecutiveInputErrors = 30;

constexpr UINT32 kBitsPerKilobit = 1000;

VARIANT variant_ui4(ULONG value) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    return v;
}

VARIANT variant_bool(bool value) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    return v;
}

// Encoders support different subsets of ICodecAPI. A refused property is logged, not fatal; the
// properties that matter (GOP, B-frames) are verified from the actual output instead.
void set_codec_value(ICodecAPI* codec, const GUID& property, const char* what, VARIANT value) {
    const HRESULT hr = codec->SetValue(&property, &value);
    if (FAILED(hr)) {
        log::warn("Video encoder refused {}: {}", what, hresult_text(hr));
    }
}

// §3.6: tell players exactly what the video processor produced, so they do not guess BT.601 or
// full range and show washed out colours.
void set_colour_attributes(IMFMediaType* type) {
    BT_CHECK_HR(type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
    BT_CHECK_HR(type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
    BT_CHECK_HR(type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
    BT_CHECK_HR(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));
}

void set_video_basics(IMFMediaType* type, const GUID& subtype, const VideoEncoderSettings& s) {
    BT_CHECK_HR(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    BT_CHECK_HR(type->SetGUID(MF_MT_SUBTYPE, subtype));
    BT_CHECK_HR(MFSetAttributeSize(type, MF_MT_FRAME_SIZE, static_cast<UINT32>(s.size.width),
                                   static_cast<UINT32>(s.size.height)));
    BT_CHECK_HR(MFSetAttributeRatio(type, MF_MT_FRAME_RATE, static_cast<UINT32>(s.fps), 1));
    BT_CHECK_HR(MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    BT_CHECK_HR(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    set_colour_attributes(type);
}

std::string friendly_name(IMFActivate* activate) {
    wchar_t* name = nullptr;
    UINT32 length = 0;
    if (FAILED(activate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &length))) {
        return "unnamed hardware encoder";
    }
    std::string out = to_utf8(std::wstring_view(name, length));
    CoTaskMemFree(name);
    return out;
}

double seconds(Time100ns t) {
    return static_cast<double>(t) / static_cast<double>(kTicksPerSecond);
}

}  // namespace

// Delivers the hardware encoder's events (§3.4.4). Holds a plain pointer to the encoder that
// detach() clears under the same lock Invoke() runs under, so an event that arrives during or
// after shutdown can never reach a destroyed VideoEncoder.
class VideoEncoder::EventCallback final : public IMFAsyncCallback {
public:
    explicit EventCallback(VideoEncoder* owner) : owner_(owner) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback)) {
            *object = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG refs = --refs_;
        if (refs == 0) {
            delete this;
        }
        return refs;
    }
    STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult* result) override {
        VideoEncoder* owner = nullptr;
        {
            std::lock_guard lock(mutex_);
            if (owner_ == nullptr) {
                return S_OK;
            }
            owner = owner_;
            ++active_;
        }
        // Called without the lock held: on_event re-arms with BeginGetEvent, and a driver may
        // complete that synchronously, re-entering Invoke on this same thread.
        owner->on_event(result);
        {
            std::lock_guard lock(mutex_);
            --active_;
        }
        idle_.notify_all();
        return S_OK;
    }

    // After detach() returns, no Invoke is running and none will reach the encoder again.
    void detach() {
        std::unique_lock lock(mutex_);
        owner_ = nullptr;
        idle_.wait(lock, [this] { return active_ == 0; });
    }

private:
    std::atomic<ULONG> refs_{1};
    std::mutex mutex_;
    std::condition_variable idle_;
    int active_ = 0;
    VideoEncoder* owner_;
};

VideoEncoder::VideoEncoder(const VideoEncoderSettings& settings, IMFDXGIDeviceManager* device_manager,
                           PacketCallback on_packet)
    : settings_(settings), on_packet_(std::move(on_packet)) {
    event_loop_done_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    BT_CHECK_WIN32(static_cast<bool>(event_loop_done_));

    if (settings_.preference != EncoderPreference::Software) {
        // Only encoders on the GPU that holds the frames: a texture cannot cross to another GPU.
        ComPtr<IMFAttributes> filter;
        BT_CHECK_HR(MFCreateAttributes(&filter, 1));
        BT_CHECK_HR(filter->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&settings_.adapter_luid),
                                    sizeof(LUID)));
        MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
        MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** raw = nullptr;
        UINT32 count = 0;
        const HRESULT hr = MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                                    &input, &output, filter.Get(), &raw, &count);
        std::vector<ComPtr<IMFActivate>> candidates;
        for (UINT32 i = 0; i < count; ++i) {
            candidates.emplace_back().Attach(raw[i]);
        }
        CoTaskMemFree(raw);
        if (FAILED(hr)) {
            log::warn("Looking for hardware H.264 encoders failed: {}", hresult_text(hr));
        } else {
            log::info("Found {} hardware H.264 encoder(s) on this GPU", candidates.size());
        }
        for (const ComPtr<IMFActivate>& candidate : candidates) {
            const std::string name = friendly_name(candidate.Get());
            try {
                if (try_hardware(candidate.Get(), device_manager)) {
                    break;
                }
            } catch (const std::exception& e) {
                log::warn("Hardware encoder '{}' could not be set up: {}", name, e.what());
            }
            shutdown();
            candidate->ShutdownObject();
        }
    }

    if (!hardware_) {
        if (settings_.preference == EncoderPreference::Hardware) {
            log::error("encoder = hardware, but no hardware H.264 encoder could be set up. Using the software "
                       "encoder so recording still works");
        } else if (settings_.preference == EncoderPreference::Auto) {
            log::warn("No usable hardware H.264 encoder, using the SOFTWARE encoder. Expect high CPU use");
        }
        setup_software();
    }
    log::info("Video encoder: {} ({}), {}x{} at {} fps, {} kbit/s CBR, keyframe every {} frames, no B-frames",
              name_, hardware_ ? "hardware" : "SOFTWARE", settings_.size.width, settings_.size.height, settings_.fps,
              settings_.bitrate_kbps, settings_.fps);
}

VideoEncoder::~VideoEncoder() {
    shutdown();
}

void VideoEncoder::reset_for_attempt() {
    // A failed earlier candidate may have left any of this behind; every attempt starts clean.
    shutting_down_ = false;
    failed_ = false;
    std::lock_guard lock(input_mutex_);
    pending_.clear();
    cpu_pending_.clear();
    need_input_ = 0;
    consecutive_input_errors_ = 0;
    stop_worker_ = false;
}

bool VideoEncoder::try_hardware(IMFActivate* activate, IMFDXGIDeviceManager* device_manager) {
    reset_for_attempt();
    name_ = friendly_name(activate);
    ComPtr<IMFTransform> transform;
    BT_CHECK_HR(activate->ActivateObject(IID_PPV_ARGS(&transform)));
    ComPtr<IMFAttributes> attributes;
    BT_CHECK_HR(transform->GetAttributes(&attributes));
    if (!MFGetAttributeUINT32(attributes.Get(), MF_TRANSFORM_ASYNC, FALSE)) {
        log::info("Skipping '{}': not an asynchronous MFT", name_);
        return false;
    }
    if (!MFGetAttributeUINT32(attributes.Get(), MF_SA_D3D11_AWARE, FALSE)) {
        log::info("Skipping '{}': cannot take Direct3D 11 textures", name_);
        return false;
    }
    // §3.4.1: without this every call fails with MF_E_TRANSFORM_ASYNC_LOCKED.
    BT_CHECK_HR(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE));
    if (FAILED(attributes->SetUINT32(MF_LOW_LATENCY, TRUE))) {
        log::info("'{}' does not take MF_LOW_LATENCY, relying on CODECAPI_AVLowLatencyMode", name_);
    }
    // §3.4.2: the device goes in before the media types; the encoder sizes its surfaces from it.
    BT_CHECK_HR(transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(device_manager)));

    transform_ = transform;
    configure(transform.Get());

    // §3.4.5
    BT_CHECK_HR(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    BT_CHECK_HR(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));

    // §3.4.4: driven by its events. BeginGetEvent, not a blocking GetEvent loop, so shutdown
    // can never hang on a thread stuck in GetEvent. Armed last: events raised meanwhile wait in
    // the queue, and a setup failure above leaves no event loop that would have to be drained.
    // callback_ is only non-null while the loop is armed; shutdown() relies on that.
    BT_CHECK_HR(transform.As(&events_));
    BT_CHECK_WIN32(ResetEvent(event_loop_done_.get()));
    callback_.Attach(new EventCallback(this));
    const HRESULT armed = events_->BeginGetEvent(callback_.Get(), nullptr);
    if (FAILED(armed)) {
        callback_.Reset();
        BT_CHECK_HR(armed);
    }
    activate_ = activate;
    hardware_ = true;
    return true;
}

void VideoEncoder::setup_software() {
    reset_for_attempt();
    name_ = "Microsoft H.264 software encoder";
    hardware_ = false;
    try {
        ComPtr<IMFTransform> transform;
        BT_CHECK_HR(
            CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&transform)));
        transform_ = transform;
        configure(transform.Get());
        BT_CHECK_HR(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
        BT_CHECK_HR(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    } catch (const std::exception& e) {
        // The software encoder refuses what H.264 level 5.2 cannot carry, e.g. 4K above 60 fps.
        transform_.Reset();
        throw std::runtime_error(std::format("The software H.264 encoder cannot encode {}x{} at {} fps: {}",
                                             settings_.size.width, settings_.size.height, settings_.fps, e.what()));
    }
    worker_ = std::thread(&VideoEncoder::software_loop, this);
}

void VideoEncoder::configure(IMFTransform* transform) {
    const HRESULT ids = transform->GetStreamIDs(1, &input_id_, 1, &output_id_);
    if (ids == E_NOTIMPL) {
        input_id_ = 0;  // fixed stream count: the IDs are the indices
        output_id_ = 0;
    } else {
        BT_CHECK_HR(ids);
    }

    // §3.1: one keyframe per second, no B-frames (decode order = presentation order, so the MP4
    // muxer never reorders), low latency so packets come out immediately.
    ComPtr<ICodecAPI> codec;
    if (SUCCEEDED(transform->QueryInterface(IID_PPV_ARGS(&codec)))) {
        set_codec_value(codec.Get(), CODECAPI_AVEncCommonRateControlMode, "constant bitrate",
                        variant_ui4(eAVEncCommonRateControlMode_CBR));
        set_codec_value(codec.Get(), CODECAPI_AVEncCommonMeanBitRate, "the bitrate",
                        variant_ui4(static_cast<ULONG>(settings_.bitrate_kbps) * kBitsPerKilobit));
        set_codec_value(codec.Get(), CODECAPI_AVEncMPVGOPSize, "a one second GOP",
                        variant_ui4(static_cast<ULONG>(settings_.fps)));
        set_codec_value(codec.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, "zero B-frames", variant_ui4(0));
        set_codec_value(codec.Get(), CODECAPI_AVLowLatencyMode, "low latency mode", variant_bool(true));
    } else {
        log::warn("Video encoder has no ICodecAPI; GOP length and B-frames use its defaults");
    }

    // §3.4.3: output type first, then input.
    ComPtr<IMFMediaType> output;
    BT_CHECK_HR(MFCreateMediaType(&output));
    set_video_basics(output.Get(), MFVideoFormat_H264, settings_);
    BT_CHECK_HR(output->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(settings_.bitrate_kbps) * kBitsPerKilobit));
    BT_CHECK_HR(output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High));
    BT_CHECK_HR(transform->SetOutputType(output_id_, output.Get(), 0));

    ComPtr<IMFMediaType> input;
    BT_CHECK_HR(MFCreateMediaType(&input));
    set_video_basics(input.Get(), MFVideoFormat_NV12, settings_);
    BT_CHECK_HR(transform->SetInputType(input_id_, input.Get(), 0));

    read_stream_info();
}

void VideoEncoder::read_stream_info() {
    MFT_OUTPUT_STREAM_INFO info{};
    BT_CHECK_HR(transform_->GetOutputStreamInfo(output_id_, &info));
    encoder_allocates_output_ =
        (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    // A compressed frame is never bigger than the raw NV12 frame; used when the encoder gives no size.
    const auto raw_frame_bytes = static_cast<DWORD>(settings_.size.width * settings_.size.height * 3 / 2);
    output_buffer_size_ = info.cbSize != 0 ? info.cbSize : raw_frame_bytes;
}

void VideoEncoder::encode(IMFSample* nv12_gpu_sample) {
    ++counters_.frames_in;
    std::lock_guard lock(input_mutex_);
    if (shutting_down_ || failed_) {
        return;
    }
    if (pending_.size() >= kMaxPendingFrames) {
        ++counters_.frames_dropped;  // §3.5: the encoder is behind; never block the capture thread
    } else {
        pending_.emplace_back(nv12_gpu_sample);
    }
    feed_locked();
}

// The one place frames go into the hardware encoder: oldest queued frame first, one per open
// NeedInput request. Called whenever either side changes (a frame arrives, a request arrives),
// so a request given back after a failed ProcessInput is always picked up again.
void VideoEncoder::feed_locked() {
    while (need_input_ > 0 && !pending_.empty() && !failed_) {
        const ComPtr<IMFSample> sample = std::move(pending_.front());
        pending_.pop_front();
        --need_input_;
        process_input_locked(sample.Get());
    }
}

void VideoEncoder::encode_system_memory(std::vector<std::uint8_t> nv12, Time100ns pts, Time100ns duration) {
    ++counters_.frames_in;
    {
        std::lock_guard lock(input_mutex_);
        if (shutting_down_ || failed_) {
            return;
        }
        if (cpu_pending_.size() >= kMaxPendingFrames) {
            ++counters_.frames_dropped;
            return;
        }
        cpu_pending_.push_back(CpuFrame{std::move(nv12), pts, duration});
    }
    worker_wake_.notify_one();
}

void VideoEncoder::process_input_locked(IMFSample* sample) {
    // §3.4.4: only called in answer to METransformNeedInput, otherwise MF_E_NOTACCEPTING loses the frame.
    const HRESULT hr = transform_->ProcessInput(input_id_, sample, 0);
    if (SUCCEEDED(hr)) {
        consecutive_input_errors_ = 0;
        return;
    }
    ++counters_.input_errors;
    if (input_error_log_.allow()) {
        log::error("Video encoder ProcessInput failed: {}", hresult_text(hr));
    }
    if (hr != MF_E_NOTACCEPTING) {
        // The encoder never repeats a NeedInput it already sent. Without giving the request back,
        // one failed frame would leave it waiting for input forever while frames pile up unseen.
        ++need_input_;
    }
    if (++consecutive_input_errors_ >= kMaxConsecutiveInputErrors) {
        log::error("Video encoder refused {} frames in a row, giving up on it", consecutive_input_errors_);
        failed_ = true;
    }
}

void VideoEncoder::on_event(IMFAsyncResult* result) {
    ComPtr<IMFMediaEvent> event;
    HRESULT hr = events_->EndGetEvent(result, &event);
    // During shutdown the loop keeps running until the encoder confirms the drain, so it ends on
    // an event we asked for instead of on a timeout.
    bool keep_listening = SUCCEEDED(hr);
    if (FAILED(hr) && hr != MF_E_SHUTDOWN) {
        log::error("Video encoder event queue failed: {}", hresult_text(hr));
        failed_ = true;
    }
    if (SUCCEEDED(hr)) {
        MediaEventType type = MEUnknown;
        event->GetType(&type);
        try {
            if (type == METransformDrainComplete) {
                keep_listening = false;
            } else if (shutting_down_) {
                // Draining: pull remaining output so the encoder can finish, but deliver nothing.
                if (type == METransformHaveOutput) {
                    process_output(false);
                }
            } else if (type == METransformNeedInput) {
                std::lock_guard lock(input_mutex_);
                if (shutting_down_) {
                    // shutdown() started between the check above and this lock; it already
                    // cleared need_input_, so do not count a request nobody will answer.
                } else {
                    ++need_input_;
                    feed_locked();
                }
            } else if (type == METransformHaveOutput) {
                process_output(true);
            } else if (type == MEError) {
                HRESULT status = S_OK;
                event->GetStatus(&status);
                log::error("Video encoder reported an error: {}", hresult_text(status));
                failed_ = true;
                keep_listening = false;
            }
        } catch (const std::exception& e) {
            if (shutting_down_) {
                // The flush in shutdown() throws away output an already queued HaveOutput event
                // announced; ProcessOutput then fails (AMD: E_UNEXPECTED). Expected, and the loop
                // keeps listening for the drain to complete.
                log::debug("Video encoder output discarded during shutdown: {}", e.what());
            } else {
                log::error("Video encoder failed while handling an event: {}", e.what());
                failed_ = true;
                keep_listening = false;
            }
        }
    }
    if (keep_listening) {
        hr = events_->BeginGetEvent(callback_.Get(), nullptr);
        if (SUCCEEDED(hr)) {
            return;
        }
        log::error("Video encoder BeginGetEvent failed: {}", hresult_text(hr));
        failed_ = true;
    }
    SetEvent(event_loop_done_.get());
}

bool VideoEncoder::process_output(bool deliver) {
    ComPtr<IMFSample> our_sample;
    if (!encoder_allocates_output_) {
        BT_CHECK_HR(MFCreateSample(&our_sample));
        ComPtr<IMFMediaBuffer> buffer;
        BT_CHECK_HR(MFCreateMemoryBuffer(output_buffer_size_, &buffer));
        BT_CHECK_HR(our_sample->AddBuffer(buffer.Get()));
    }
    MFT_OUTPUT_DATA_BUFFER output{};
    output.dwStreamID = output_id_;
    output.pSample = our_sample.Get();
    DWORD status = 0;
    const HRESULT hr = transform_->ProcessOutput(0, 1, &output, &status);
    ComPtr<IMFCollection> events;
    events.Attach(output.pEvents);
    ComPtr<IMFSample> produced;
    if (encoder_allocates_output_) {
        produced.Attach(output.pSample);
    } else {
        produced = our_sample;
    }

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        return false;
    }
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        // Some drivers renegotiate their output format once after the first frame.
        ComPtr<IMFMediaType> type;
        BT_CHECK_HR(transform_->GetOutputAvailableType(output_id_, 0, &type));
        BT_CHECK_HR(transform_->SetOutputType(output_id_, type.Get(), 0));
        read_stream_info();
        // The MP4 writer needs the sequence header of the format packets are now in.
        ComPtr<IMFMediaType> current;
        if (counters_.packets_out > 0 && SUCCEEDED(transform_->GetOutputCurrentType(output_id_, &current))) {
            std::lock_guard lock(output_type_mutex_);
            output_type_ = current;
        }
        log::info("Video encoder changed its output format");
        return true;
    }
    BT_CHECK_HR(hr);
    if (output.dwStatus & MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE) {
        return true;  // success without a packet this time
    }
    if (produced && deliver) {
        emit_packet(produced.Get());
    }
    return true;
}

void VideoEncoder::emit_packet(IMFSample* sample) {
    LONGLONG pts = 0;
    if (FAILED(sample->GetSampleTime(&pts))) {
        if (timestamp_error_log_.allow()) {
            log::warn("Video encoder produced a packet without a timestamp, dropped");
        }
        return;
    }
    LONGLONG duration = 0;
    if (FAILED(sample->GetSampleDuration(&duration)) || duration <= 0) {
        duration = frame_offset(1, settings_.fps);
    }

    // §3.4.6: the bytes are copied out so no encoder owned sample ever sits in the ring buffer.
    std::vector<std::uint8_t> bytes;
    {
        ComPtr<IMFMediaBuffer> buffer;
        BT_CHECK_HR(sample->ConvertToContiguousBuffer(&buffer));
        BYTE* data = nullptr;
        DWORD length = 0;
        BT_CHECK_HR(buffer->Lock(&data, nullptr, &length));
        struct Unlock {
            IMFMediaBuffer* buffer;
            ~Unlock() { buffer->Unlock(); }
        } unlock{buffer.Get()};
        bytes.assign(data, data + length);
    }
    Packet packet{pts, duration, false, make_packet_bytes(std::move(bytes))};

    // §3.4.6: the keyframe flag comes from CleanPoint. AMD sets it only on keyframes and leaves it
    // out elsewhere (measured), so an absent attribute is not an error. When it is absent the
    // bitstream decides: an encoder that never set it would otherwise never produce a clip.
    UINT32 clean_point = 0;
    if (SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean_point))) {
        packet.keyframe = clean_point != 0;
    } else {
        packet.keyframe = h264::inspect(*packet.data).has_idr;
        if (packet.keyframe && !clean_point_fallback_logged_) {
            log::warn("Video encoder did not mark a keyframe, detecting keyframes from the H.264 stream");
            clean_point_fallback_logged_ = true;
        }
    }
    const bool keyframe = packet.keyframe;

    // The output type is what the MP4 writer is built from (§7). Asked for again on later packets if
    // the encoder could not say it yet, rather than leaving every save refused for the session.
    bool have_type = false;
    {
        std::lock_guard lock(output_type_mutex_);
        have_type = output_type_ != nullptr;
    }
    if (!have_type) {
        ComPtr<IMFMediaType> type;
        if (SUCCEEDED(transform_->GetOutputCurrentType(output_id_, &type))) {
            std::lock_guard lock(output_type_mutex_);
            output_type_ = type;
        } else if (type_error_log_.allow()) {
            log::error("Video encoder does not report its output format yet; clips cannot be saved until it does");
        }
    }
    if (counters_.packets_out == 0) {
        UINT32 header_bytes = 0;
        {
            std::lock_guard lock(output_type_mutex_);
            if (!output_type_ || FAILED(output_type_->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &header_bytes))) {
                header_bytes = 0;
            }
        }
        const h264::AccessUnitInfo info = h264::inspect(*packet.data);
        log::info("First video packet: {} bytes, keyframe {}, SPS {}, PPS {}, IDR {}; sequence header in the output "
                  "type: {}",
                  packet.size(), keyframe, info.has_sps, info.has_pps, info.has_idr,
                  header_bytes != 0 ? std::to_string(header_bytes) + " bytes" : std::string("none"));
    }

    if (have_last_pts_ && pts <= last_pts_) {
        ++counters_.non_increasing_pts;
        if (counters_.non_increasing_pts == 1) {
            log::warn("Video encoder output went back in time ({:.3f} s after {:.3f} s): B-frames or a broken encoder",
                      seconds(pts), seconds(last_pts_));
        }
    }
    have_last_pts_ = true;
    last_pts_ = pts;

    if (keyframe) {
        ++counters_.keyframes;
        if (have_keyframe_) {
            const Time100ns interval = pts - last_keyframe_pts_;
            if (counters_.min_keyframe_interval == 0 || interval < counters_.min_keyframe_interval) {
                counters_.min_keyframe_interval = interval;
            }
            counters_.max_keyframe_interval = std::max<Time100ns>(counters_.max_keyframe_interval, interval);
            if (keyframe_intervals_logged_ < kKeyframeIntervalsToLog) {
                ++keyframe_intervals_logged_;
                log::info("Keyframe interval {:.3f} s", seconds(interval));
            }
        }
        have_keyframe_ = true;
        last_keyframe_pts_ = pts;
    }
    ++counters_.packets_out;
    counters_.bytes_out += packet.size();
    ComPtr<IMFMediaType> type;
    {
        std::lock_guard lock(output_type_mutex_);
        type = output_type_;
    }
    on_packet_(std::move(packet), type.Get());
}

void VideoEncoder::software_loop() {
    try {
        ComApartment com(COINIT_MULTITHREADED);
        int consecutive_errors = 0;
        while (true) {
            CpuFrame frame;
            {
                std::unique_lock lock(input_mutex_);
                worker_wake_.wait(lock, [this] { return stop_worker_ || !cpu_pending_.empty(); });
                if (stop_worker_) {
                    return;
                }
                frame = std::move(cpu_pending_.front());
                cpu_pending_.pop_front();
            }
            ComPtr<IMFMediaBuffer> buffer;
            BT_CHECK_HR(MFCreateMemoryBuffer(static_cast<DWORD>(frame.nv12.size()), &buffer));
            BYTE* data = nullptr;
            BT_CHECK_HR(buffer->Lock(&data, nullptr, nullptr));
            std::memcpy(data, frame.nv12.data(), frame.nv12.size());
            BT_CHECK_HR(buffer->Unlock());
            BT_CHECK_HR(buffer->SetCurrentLength(static_cast<DWORD>(frame.nv12.size())));
            ComPtr<IMFSample> sample;
            BT_CHECK_HR(MFCreateSample(&sample));
            BT_CHECK_HR(sample->AddBuffer(buffer.Get()));
            BT_CHECK_HR(sample->SetSampleTime(frame.pts));
            BT_CHECK_HR(sample->SetSampleDuration(frame.duration));

            // §3.4: the software MFT is synchronous: ProcessInput, then ProcessOutput until it
            // asks for more input.
            HRESULT hr = transform_->ProcessInput(input_id_, sample.Get(), 0);
            if (hr == MF_E_NOTACCEPTING) {
                while (process_output(true)) {
                }
                hr = transform_->ProcessInput(input_id_, sample.Get(), 0);
            }
            if (FAILED(hr)) {
                ++counters_.input_errors;
                if (input_error_log_.allow()) {
                    log::error("Software encoder ProcessInput failed: {}", hresult_text(hr));
                }
                if (++consecutive_errors >= kMaxConsecutiveInputErrors) {
                    throw std::runtime_error(std::format("it refused {} frames in a row", consecutive_errors));
                }
            } else {
                consecutive_errors = 0;
            }
            while (process_output(true)) {
            }
        }
    } catch (const std::exception& e) {
        log::error("Software video encoder stopped: {}", e.what());
        failed_ = true;
    } catch (...) {
        log::error("Software video encoder stopped: unknown exception");
        failed_ = true;
    }
}

void VideoEncoder::shutdown() {
    shutting_down_ = true;
    if (worker_.joinable()) {
        {
            std::lock_guard lock(input_mutex_);
            stop_worker_ = true;
        }
        worker_wake_.notify_one();
        worker_.join();
    }
    {
        std::lock_guard lock(input_mutex_);
        pending_.clear();
        cpu_pending_.clear();
        need_input_ = 0;
    }
    if (transform_) {
        // §3.4.5: frames still inside the encoder are discarded, not waited for.
        const HRESULT hr = transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        if (FAILED(hr)) {
            log::warn("Video encoder flush failed: {}", hresult_text(hr));
        }
    }
    if (callback_) {
        // End of stream + drain makes the encoder answer with METransformDrainComplete, which
        // ends the event loop cleanly. (AMD's encoder does not complete a pending BeginGetEvent
        // on IMFShutdown alone; measured: the wait always timed out.)
        bool loop_ended = false;
        HRESULT hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        if (SUCCEEDED(hr)) {
            hr = transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        }
        if (SUCCEEDED(hr)) {
            loop_ended = WaitForSingleObject(event_loop_done_.get(), kShutdownTimeoutMs) == WAIT_OBJECT_0;
        } else {
            log::warn("Video encoder drain failed: {}", hresult_text(hr));
        }
        // IMFShutdown releases the encoder's resources and, for encoders that implement it fully,
        // completes a still pending BeginGetEvent with MF_E_SHUTDOWN.
        ComPtr<IMFShutdown> shutdown;
        if (SUCCEEDED(transform_.As(&shutdown)) && SUCCEEDED(shutdown->Shutdown()) && !loop_ended) {
            loop_ended = WaitForSingleObject(event_loop_done_.get(), kShutdownTimeoutMs) == WAIT_OBJECT_0;
        }
        if (!loop_ended) {
            log::warn("Video encoder did not finish its event loop within {} ms", kShutdownTimeoutMs);
        }
        callback_->detach();
    }
    callback_.Reset();
    events_.Reset();
    transform_.Reset();
    if (activate_) {
        activate_->ShutdownObject();
        activate_.Reset();
    }
}

ComPtr<IMFMediaType> VideoEncoder::output_type() const {
    std::lock_guard lock(output_type_mutex_);
    return output_type_;
}

VideoEncoderStats VideoEncoder::stats() const {
    VideoEncoderStats s;
    s.frames_in = counters_.frames_in;
    s.frames_dropped = counters_.frames_dropped;
    s.input_errors = counters_.input_errors;
    s.packets_out = counters_.packets_out;
    s.keyframes = counters_.keyframes;
    s.bytes_out = counters_.bytes_out;
    s.min_keyframe_interval = counters_.min_keyframe_interval;
    s.max_keyframe_interval = counters_.max_keyframe_interval;
    s.non_increasing_pts = counters_.non_increasing_pts;
    return s;
}

}  // namespace bt
