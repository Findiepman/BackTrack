#include "platform/MicCapture.h"

#include "core/Clock.h"

#include <avrt.h>
#include <mfapi.h>
#include <mferror.h>
#include <propidl.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstring>

namespace bt {
namespace {

constexpr UINT32 kOutputRate = 48000;  // what the AudioFifo and AudioCapture's mix run at
constexpr UINT32 kFloatBytes = 4;
constexpr DWORD kPollIntervalMs = 10;  // like the loopback: the device buffer holds 100 ms
constexpr REFERENCE_TIME kDeviceBufferDuration = 100 * kTicksPerMillisecond;
constexpr UINT32 kChunkMilliseconds = 100;
constexpr UINT32 kMillisecondsPerSecond = 1000;
constexpr ULONGLONG kReopenDelayMs = 2000;
constexpr ULONGLONG kSettleMs = 1000;  // see AudioCapture: a device failing right after opening waits
// Windows sends no notification when the privacy setting changes, and a missing microphone may
// come back without one on some drivers: both are looked at again this often.
constexpr ULONGLONG kBlockedRetryMs = 5000;
constexpr ULONGLONG kNoDeviceRetryMs = 5000;
// Choosing the channels that carry the voice (see deliver).
constexpr float kChannelPowerSmoothing = 0.98f;  // per ~10 ms packet: about half a second
constexpr float kQuietChannel = 0.1f;  // 10 dB below the loudest channel

// PKEY_Device_FriendlyName, spelled out so the header's DEFINE_PROPERTYKEY needs no INITGUID.
constexpr PROPERTYKEY kDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

struct CoTaskMemDeleter {
    void operator()(void* p) const { CoTaskMemFree(p); }
};

std::wstring id_of(IMMDevice* device) {
    LPWSTR raw = nullptr;
    if (FAILED(device->GetId(&raw))) {
        return {};
    }
    const std::unique_ptr<wchar_t, CoTaskMemDeleter> id(raw);
    return id.get();
}

std::string friendly_name(IMMDevice* device) {
    ComPtr<IPropertyStore> properties;
    std::string name = "Microphone";
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(properties->GetValue(kDeviceFriendlyName, &value)) && value.vt == VT_LPWSTR) {
            name = to_utf8(value.pwszVal);
        }
        PropVariantClear(&value);
    }
    return name;
}

bool is_active_capture(IMMDevice* device) {
    DWORD state = 0;
    if (FAILED(device->GetState(&state)) || state != DEVICE_STATE_ACTIVE) {
        return false;
    }
    // An id from the settings file could be a playback device; it cannot be recorded from.
    ComPtr<IMMEndpoint> endpoint;
    EDataFlow flow = eRender;
    return SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&endpoint))) && SUCCEEDED(endpoint->GetDataFlow(&flow)) &&
           flow == eCapture;
}

// The device to record now: the chosen one when it is there, else the Windows default.
// Throws (E_NOTFOUND) when there is no microphone at all.
ComPtr<IMMDevice> pick_device(IMMDeviceEnumerator* enumerator, const std::wstring& wanted, bool& fallback) {
    fallback = false;
    if (!wanted.empty()) {
        ComPtr<IMMDevice> device;
        if (SUCCEEDED(enumerator->GetDevice(wanted.c_str(), &device)) && is_active_capture(device.Get())) {
            return device;
        }
        fallback = true;
    }
    ComPtr<IMMDevice> device;
    BT_CHECK_HR(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device));
    return device;
}

std::wstring pick_device_id(IMMDeviceEnumerator* enumerator, const std::wstring& wanted) {
    try {
        bool fallback = false;
        return id_of(pick_device(enumerator, wanted, fallback).Get());
    } catch (const std::exception&) {
        return {};
    }
}

ComPtr<IMFSample> make_buffer_sample(DWORD bytes) {
    ComPtr<IMFMediaBuffer> buffer;
    BT_CHECK_HR(MFCreateMemoryBuffer(bytes, &buffer));
    ComPtr<IMFSample> sample;
    BT_CHECK_HR(MFCreateSample(&sample));
    BT_CHECK_HR(sample->AddBuffer(buffer.Get()));
    return sample;
}

// 48 kHz 32-bit float with the device's own channels: the resampler only changes the rate and
// the sample type; mixing down to mono is done here, the same way for every channel layout.
ComPtr<IMFMediaType> float_type(UINT32 channels, UINT32 channel_mask) {
    ComPtr<IMFMediaType> type;
    BT_CHECK_HR(MFCreateMediaType(&type));
    BT_CHECK_HR(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    BT_CHECK_HR(type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float));
    BT_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, kFloatBytes * 8));
    BT_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kOutputRate));
    BT_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels));
    BT_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * kFloatBytes));
    BT_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kOutputRate * channels * kFloatBytes));
    BT_CHECK_HR(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    if (channel_mask != 0) {
        BT_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK, channel_mask));
    }
    return type;
}

}  // namespace

std::vector<MicDevice> list_microphones() {
    // The UI thread is usually STA already; then COM stays as it is (RPC_E_CHANGED_MODE).
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct Uninitialize {
        bool active;
        ~Uninitialize() {
            if (active) {
                CoUninitialize();
            }
        }
    } uninitialize{SUCCEEDED(co)};

    std::vector<MicDevice> devices;
    try {
        ComPtr<IMMDeviceEnumerator> enumerator;
        BT_CHECK_HR(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));
        std::wstring default_id;
        ComPtr<IMMDevice> default_device;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &default_device))) {
            default_id = id_of(default_device.Get());
        }
        ComPtr<IMMDeviceCollection> collection;
        BT_CHECK_HR(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection));
        UINT count = 0;
        BT_CHECK_HR(collection->GetCount(&count));
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(i, &device))) {
                continue;
            }
            MicDevice entry;
            entry.id = id_of(device.Get());
            if (entry.id.empty()) {
                continue;
            }
            entry.name = friendly_name(device.Get());
            entry.is_default = entry.id == default_id;
            devices.push_back(std::move(entry));
        }
    } catch (const std::exception& e) {
        log::warn("Cannot list the microphones: {}", e.what());
    }
    std::stable_partition(devices.begin(), devices.end(), [](const MicDevice& d) { return d.is_default; });
    return devices;
}

// Turns "a capture device changed" into a flag for the microphone thread. The callback runs on
// a Windows thread and must not block or call back into the audio API.
class MicCapture::DeviceNotifier final : public IMMNotificationClient {
public:
    DeviceNotifier(std::atomic<bool>* changed, HANDLE wake) : changed_(changed), wake_(wake) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *object = static_cast<IMMNotificationClient*>(this);
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
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eCapture && role == eConsole) {
            signal();
        }
        return S_OK;
    }
    // Plugged in, unplugged, enabled or disabled: the thread checks whether the device it
    // should record has changed (cheap, and these are rare).
    STDMETHODIMP OnDeviceAdded(LPCWSTR) override {
        signal();
        return S_OK;
    }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR) override {
        signal();
        return S_OK;
    }
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override {
        signal();
        return S_OK;
    }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    void signal() {
        changed_->store(true);
        SetEvent(wake_);
    }

    std::atomic<ULONG> refs_{1};
    std::atomic<bool>* changed_;
    HANDLE wake_;
};

MicCapture::MicCapture(std::wstring device_id, bool noise_suppression, AudioFifo& fifo)
    : wanted_id_(std::move(device_id)), fifo_(fifo), noise_suppression_(noise_suppression) {
    stop_event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    BT_CHECK_WIN32(static_cast<bool>(stop_event_));
    wake_event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    BT_CHECK_WIN32(static_cast<bool>(wake_event_));
    suppressor_ = std::make_unique<NoiseSuppressor>(noise_suppression);
}

MicCapture::~MicCapture() {
    stop();
}

void MicCapture::start() {
    if (thread_.joinable()) {
        if (running_) {
            return;
        }
        thread_.join();  // it ended on its own (failed); start a fresh one
    }
    BT_CHECK_WIN32(ResetEvent(stop_event_.get()));
    status_ = MicStatus::Starting;
    running_ = true;
    thread_ = std::thread(&MicCapture::thread_main, this);
}

void MicCapture::stop() {
    if (!thread_.joinable()) {
        return;
    }
    SetEvent(stop_event_.get());
    thread_.join();
    status_ = MicStatus::Off;
}

std::string MicCapture::device_name() const {
    std::lock_guard lock(name_mutex_);
    return device_name_;
}

MicStats MicCapture::stats() const {
    MicStats s;
    s.packets = counters_.packets;
    s.frames_out = counters_.frames_out;
    s.device_opens = counters_.device_opens;
    return s;
}

void MicCapture::thread_main() {
    // Like the loopback thread: device errors close the device and retry; only a failure to reach
    // the Windows audio system at all ends the thread. The game sound keeps recording either way.
    try {
        ComApartment com(COINIT_MULTITHREADED);
        // A game keeps every core busy, and a poll that comes a tick late eats into the FIFO's
        // headroom. MMCSS "Audio" keeps this thread on time without admin rights.
        DWORD task_index = 0;
        const HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
        if (mmcss == nullptr) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        }
        struct Revert {
            HANDLE handle;
            ~Revert() {
                if (handle != nullptr) {
                    AvRevertMmThreadCharacteristics(handle);
                }
            }
        } revert{mmcss};
        run_loop();
    } catch (const std::exception& e) {
        log::error("Microphone recording stopped: {}", e.what());
        status_ = MicStatus::Failed;
    } catch (...) {
        log::error("Microphone recording stopped: unknown exception");
        status_ = MicStatus::Failed;
    }
    running_ = false;
}

void MicCapture::run_loop() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    BT_CHECK_HR(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));
    ComPtr<DeviceNotifier> notifier;
    notifier.Attach(new DeviceNotifier(&devices_changed_, wake_event_.get()));
    const bool registered = SUCCEEDED(enumerator->RegisterEndpointNotificationCallback(notifier.Get()));
    if (!registered) {
        log::warn("Cannot watch for microphone changes; switching microphones needs a restart");
    }
    // Unregistered before the notifier is released, on every way out (see AudioCapture).
    struct Unregister {
        IMMDeviceEnumerator* enumerator;
        IMMNotificationClient* client;
        bool active;
        ~Unregister() {
            if (active) {
                const HRESULT unregistered = enumerator->UnregisterEndpointNotificationCallback(client);
                if (FAILED(unregistered)) {
                    log::debug("UnregisterEndpointNotificationCallback (microphone): {}", hresult_text(unregistered));
                }
            }
        }
    } unregister{enumerator.Get(), notifier.Get(), registered};
    struct CloseDevice {
        MicCapture* self;
        ~CloseDevice() { self->close_device(); }
    } close_on_exit{this};

    ULONGLONG next_open = 0;
    while (true) {
        if (!open_ && GetTickCount64() >= next_open) {
            try {
                open_device(enumerator.Get());
            } catch (const std::exception& e) {
                close_device();
                const auto* hr_error = dynamic_cast<const HresultError*>(&e);
                const HRESULT hr = hr_error != nullptr ? hr_error->hr() : E_FAIL;
                if (hr == E_ACCESSDENIED) {
                    status_ = MicStatus::Blocked;
                    if (!warned_blocked_) {
                        warned_blocked_ = true;
                        log::warn("Windows does not let Backtrack use the microphone (Settings > Privacy & security > "
                                  "Microphone > Let desktop apps access your microphone); clips have no voice until "
                                  "that is turned on");
                    }
                    next_open = GetTickCount64() + kBlockedRetryMs;
                } else if (hr == E_NOTFOUND) {
                    status_ = MicStatus::NoDevice;
                    if (error_log_.allow()) {
                        log::info("No microphone is connected; it is used as soon as one is");
                    }
                    next_open = GetTickCount64() + kNoDeviceRetryMs;
                } else {
                    status_ = MicStatus::Failed;
                    if (error_log_.allow()) {
                        log::warn("Cannot record the microphone yet ({}), retrying", e.what());
                    }
                    next_open = GetTickCount64() + kReopenDelayMs;
                }
            }
        }
        const HANDLE handles[] = {stop_event_.get(), wake_event_.get()};
        const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, kPollIntervalMs);
        if (wait == WAIT_OBJECT_0) {
            return;
        }
        if (devices_changed_.exchange(false)) {
            if (!open_) {
                next_open = 0;  // a microphone may have arrived: try now
                continue;
            }
            const std::wstring should_record = pick_device_id(enumerator.Get(), wanted_id_);
            if (should_record != device_id_) {
                log::info("The microphone changed, switching to it");
                close_device();
                status_ = MicStatus::Starting;
                next_open = 0;
                continue;
            }
        }
        if (!open_) {
            continue;
        }
        try {
            read_packets();
        } catch (const std::exception& e) {
            const auto* hr_error = dynamic_cast<const HresultError*>(&e);
            const bool invalidated = hr_error != nullptr && hr_error->hr() == AUDCLNT_E_DEVICE_INVALIDATED;
            const bool settled = GetTickCount64() - opened_at_ms_ > kSettleMs;
            if (error_log_.allow()) {
                log::info("Microphone recording interrupted ({}), reopening", e.what());
            }
            close_device();
            status_ = MicStatus::Starting;
            next_open = invalidated && settled ? 0 : GetTickCount64() + kReopenDelayMs;
        }
    }
}

void MicCapture::open_device(IMMDeviceEnumerator* enumerator) {
    bool fallback = false;
    const ComPtr<IMMDevice> device = pick_device(enumerator, wanted_id_, fallback);
    const std::string name = friendly_name(device.Get());
    if (fallback && !warned_missing_) {
        warned_missing_ = true;
        log::warn("The chosen microphone is not connected; recording the Windows default microphone ({}) until it is",
                  name);
    }

    BT_CHECK_HR(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**>(audio_client_.ReleaseAndGetAddressOf())));
    WAVEFORMATEX* raw_format = nullptr;
    BT_CHECK_HR(audio_client_->GetMixFormat(&raw_format));
    const std::unique_ptr<WAVEFORMATEX, CoTaskMemDeleter> format(raw_format);
    BT_CHECK_HR(audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kDeviceBufferDuration, 0, format.get(), nullptr));
    BT_CHECK_HR(audio_client_->GetService(IID_PPV_ARGS(&capture_client_)));

    UINT32 channel_mask = 0;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        channel_mask = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format.get())->dwChannelMask;
    }
    ComPtr<IMFTransform> resampler;
    BT_CHECK_HR(CoCreateInstance(CLSID_CResamplerMediaObject, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&resampler)));
    ComPtr<IMFMediaType> input;
    BT_CHECK_HR(MFCreateMediaType(&input));
    BT_CHECK_HR(MFInitMediaTypeFromWaveFormatEx(input.Get(), format.get(),
                                                static_cast<UINT32>(sizeof(WAVEFORMATEX) + format->cbSize)));
    BT_CHECK_HR(resampler->SetInputType(0, input.Get(), 0));
    BT_CHECK_HR(resampler->SetOutputType(0, float_type(format->nChannels, channel_mask).Get(), 0));
    MFT_OUTPUT_STREAM_INFO info{};
    BT_CHECK_HR(resampler->GetOutputStreamInfo(0, &info));
    const DWORD chunk_bytes = kOutputRate * format->nChannels * kFloatBytes * kChunkMilliseconds / kMillisecondsPerSecond;
    resampler_output_size_ = std::max(info.cbSize, chunk_bytes);
    BT_CHECK_HR(resampler->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    BT_CHECK_HR(resampler->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    resampler_ = resampler;
    block_align_ = format->nBlockAlign;
    device_rate_ = format->nSamplesPerSec;
    channels_ = format->nChannels;
    channel_power_.fill(0.0f);
    suppressor_->clear_audio();  // what was in it is from before a pause or from the old device

    BT_CHECK_HR(audio_client_->Start());
    device_id_ = id_of(device.Get());
    opened_at_ms_ = GetTickCount64();
    open_ = true;
    warned_blocked_ = false;  // a block after this (setting turned off again) is logged again
    {
        std::lock_guard lock(name_mutex_);
        device_name_ = name;
    }
    status_ = MicStatus::Recording;
    ++counters_.device_opens;
    log::info("Recording the microphone {} ({} Hz, {} channels, noise suppression {})", name, format->nSamplesPerSec,
              format->nChannels, noise_suppression_ ? "on" : "off");
}

void MicCapture::close_device() {
    if (audio_client_) {
        const HRESULT stopped = audio_client_->Stop();
        if (FAILED(stopped)) {
            log::debug("Microphone Stop: {}", hresult_text(stopped));  // the device may already be gone
        }
    }
    capture_client_.Reset();
    audio_client_.Reset();
    resampler_.Reset();  // its last few milliseconds are not needed: the FIFO bridges gaps
    device_id_.clear();
    open_ = false;
}

void MicCapture::read_packets() {
    while (true) {
        UINT32 next = 0;
        BT_CHECK_HR(capture_client_->GetNextPacketSize(&next));
        if (next == 0) {
            return;
        }
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        const HRESULT hr = capture_client_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) {
            return;
        }
        BT_CHECK_HR(hr);
        // Resampled and cleaned straight from WASAPI's buffer: one 10 ms packet takes well under
        // a millisecond, so the buffer is never held long.
        struct Release {
            IAudioCaptureClient* client;
            UINT32 frames;
            bool done = false;
            ~Release() {
                if (!done) {
                    client->ReleaseBuffer(frames);
                }
            }
        } release{capture_client_.Get(), frames};
        resample((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 ? nullptr : data, frames);
        release.done = true;
        BT_CHECK_HR(capture_client_->ReleaseBuffer(frames));
        ++counters_.packets;
    }
}

void MicCapture::resample(const BYTE* data, UINT32 frames) {
    if (frames == 0) {
        return;
    }
    const DWORD bytes = frames * block_align_;
    const ComPtr<IMFSample> sample = make_buffer_sample(bytes);
    ComPtr<IMFMediaBuffer> buffer;
    BT_CHECK_HR(sample->GetBufferByIndex(0, &buffer));
    BYTE* target = nullptr;
    BT_CHECK_HR(buffer->Lock(&target, nullptr, nullptr));
    if (data != nullptr) {
        std::memcpy(target, data, bytes);
    } else {
        std::memset(target, 0, bytes);  // zero is silence for the float and signed formats WASAPI uses
    }
    BT_CHECK_HR(buffer->Unlock());
    BT_CHECK_HR(buffer->SetCurrentLength(bytes));
    // The stamps only keep the resampler's input contiguous; the mix is timed by the FIFO.
    const LONGLONG duration = static_cast<LONGLONG>(frames) * kTicksPerSecond / device_rate_;
    BT_CHECK_HR(sample->SetSampleTime(next_sample_time_));
    BT_CHECK_HR(sample->SetSampleDuration(duration));
    next_sample_time_ += duration;

    HRESULT hr = resampler_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        drain_resampler();
        hr = resampler_->ProcessInput(0, sample.Get(), 0);
    }
    BT_CHECK_HR(hr);
    drain_resampler();
}

void MicCapture::drain_resampler() {
    while (true) {
        const ComPtr<IMFSample> output = make_buffer_sample(resampler_output_size_);
        MFT_OUTPUT_DATA_BUFFER out{};
        out.pSample = output.Get();
        DWORD status = 0;
        const HRESULT hr = resampler_->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents != nullptr) {
            out.pEvents->Release();
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return;
        }
        BT_CHECK_HR(hr);
        ComPtr<IMFMediaBuffer> buffer;
        BT_CHECK_HR(output->ConvertToContiguousBuffer(&buffer));
        BYTE* bytes = nullptr;
        DWORD length = 0;
        BT_CHECK_HR(buffer->Lock(&bytes, nullptr, &length));
        struct Unlock {
            IMFMediaBuffer* buffer;
            ~Unlock() { buffer->Unlock(); }
        } unlock{buffer.Get()};
        const std::size_t frames = length / (static_cast<std::size_t>(channels_) * kFloatBytes);
        if (frames == 0) {
            return;
        }
        deliver(bytes, frames);
    }
}

void MicCapture::deliver(const BYTE* interleaved, std::size_t frames) {
    // Mono: the average of the channels that carry sound. A headset mic is mono already, an array
    // mic or a stereo USB mic has the voice on every channel, but an audio interface often has the
    // mic on input 1 only: averaging in the empty input 2 would halve the voice. A channel more
    // than 10 dB below the loudest (over the last ~half second) is left out.
    if (mono_.size() < frames) {
        mono_.resize(frames);
    }
    const UINT32 channels = channels_;
    const auto sample = [&](std::size_t i, UINT32 c) {
        float value = 0.0f;
        // Copied out, not cast: the buffer is raw bytes from Media Foundation.
        std::memcpy(&value, interleaved + (i * channels + c) * kFloatBytes, sizeof(value));
        return value;
    };
    std::array<bool, kMaxWeighedChannels> use{};
    UINT32 used = 0;
    if (channels <= kMaxWeighedChannels) {
        float loudest = 0.0f;
        for (UINT32 c = 0; c < channels; ++c) {
            float power = 0.0f;
            for (std::size_t i = 0; i < frames; ++i) {
                power += sample(i, c) * sample(i, c);
            }
            power /= static_cast<float>(frames);
            channel_power_[c] = kChannelPowerSmoothing * channel_power_[c] + (1.0f - kChannelPowerSmoothing) * power;
            if (channel_power_[c] < 1e-20f) {
                channel_power_[c] = 0.0f;  // digital silence decays to zero, not into denormals
            }
            loudest = std::max(loudest, channel_power_[c]);
        }
        for (UINT32 c = 0; c < channels; ++c) {
            use[c] = channel_power_[c] >= kQuietChannel * loudest;
            used += use[c] ? 1 : 0;
        }
    }
    const bool all = used == 0;
    const float scale = 1.0f / static_cast<float>(all ? channels : used);
    for (std::size_t i = 0; i < frames; ++i) {
        float sum = 0.0f;
        for (UINT32 c = 0; c < channels; ++c) {
            if (all || use[c]) {
                sum += sample(i, c);
            }
        }
        mono_[i] = sum * scale;
    }
    const std::span<float> block(mono_.data(), frames);
    const bool suppress = noise_suppression_;
    if (suppress != suppressor_->enabled()) {
        suppressor_->set_enabled(suppress);
    }
    suppressor_->process(block);
    fifo_.push(block);
    counters_.frames_out += frames;
}

}  // namespace bt
