#include "platform/AudioCapture.h"

#include "platform/QpcClock.h"

#include <mfapi.h>
#include <mferror.h>
#include <propidl.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>

namespace bt {
namespace {

// What goes into the AAC encoder and the MP4: 48 kHz 16-bit stereo, what every player expects.
constexpr UINT32 kOutputRate = 48000;
constexpr UINT32 kOutputChannels = 2;
constexpr UINT32 kOutputBits = 16;
constexpr UINT32 kOutputBlockAlign = kOutputChannels * kOutputBits / 8;
constexpr UINT32 kAacFrameSamples = 1024;
constexpr UINT32 kAacProfileLevel = 0x29;  // AAC-LC profile level 2: the one every player supports
constexpr UINT32 kAacPayloadRaw = 0;  // raw AAC frames, the MP4 form (no ADTS headers)
constexpr DWORD kAacFallbackOutputBytes = 6144;  // the AAC spec maximum per channel x 8 channels
constexpr UINT32 kBitsPerKilobit = 1000;

// §6: polled every 10 ms; the device buffer holds 100 ms so a late poll never loses data.
constexpr DWORD kPollIntervalMs = 10;
constexpr REFERENCE_TIME kDeviceBufferDuration = 100 * kTicksPerMillisecond;

// Audio may drift this far from the video clock before it is corrected. Far below the ~45 ms at
// which people notice lip sync, and a typical 50 ppm sound card needs a correction every ~3 min.
constexpr Time100ns kDriftTolerance = 10 * kTicksPerMillisecond;

// Largest block handed to the resampler at once (keeps buffers small when filling a gap).
constexpr UINT32 kChunkMilliseconds = 100;
constexpr UINT32 kMillisecondsPerSecond = 1000;
constexpr ULONGLONG kReopenDelayMs = 2000;
// A device that fails again within this time of opening is retried after kReopenDelayMs instead of
// at once, so a flapping USB DAC or a format change still settling cannot cause a reopen loop.
constexpr ULONGLONG kSettleMs = 1000;

// After a flush the first AAC packet may start up to two frames (priming) away from the input.
constexpr Time100ns kAacFlushTolerance = 2 * kTicksPerSecond * kAacFrameSamples / kOutputRate;

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

std::wstring default_device_id(IMMDeviceEnumerator* enumerator) {
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
        return {};
    }
    return id_of(device.Get());
}

ComPtr<IMFMediaType> pcm_type() {
    ComPtr<IMFMediaType> type;
    BT_CHECK_HR(MFCreateMediaType(&type));
    BT_CHECK_HR(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    BT_CHECK_HR(type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
    BT_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, kOutputBits));
    BT_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kOutputRate));
    BT_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kOutputChannels));
    BT_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kOutputBlockAlign));
    BT_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kOutputRate * kOutputBlockAlign));
    BT_CHECK_HR(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    return type;
}

ComPtr<IMFSample> make_buffer_sample(DWORD bytes) {
    ComPtr<IMFMediaBuffer> buffer;
    BT_CHECK_HR(MFCreateMemoryBuffer(bytes, &buffer));
    ComPtr<IMFSample> sample;
    BT_CHECK_HR(MFCreateSample(&sample));
    BT_CHECK_HR(sample->AddBuffer(buffer.Get()));
    return sample;
}

Time100ns frames_to_time(std::uint64_t frames, UINT32 rate) {
    return static_cast<Time100ns>(frames * static_cast<std::uint64_t>(kTicksPerSecond) / rate);
}

std::string device_name(IMMDevice* device) {
    ComPtr<IPropertyStore> properties;
    std::string name = "the default playback device";
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

}  // namespace

// Turns "the default playback device changed" into a flag for the audio thread. The callback
// runs on a Windows thread and must not block or call back into the audio API.
class AudioCapture::DeviceNotifier final : public IMMNotificationClient {
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
        if (flow == eRender && role == eConsole) {
            changed_->store(true);
            SetEvent(wake_);
        }
        return S_OK;
    }
    STDMETHODIMP OnDeviceAdded(LPCWSTR) override { return S_OK; }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    std::atomic<ULONG> refs_{1};
    std::atomic<bool>* changed_;
    HANDLE wake_;
};

AudioCapture::AudioCapture(int bitrate_kbps, Time100ns max_gap_fill, PacketCallback on_packet)
    : on_packet_(std::move(on_packet)), max_gap_fill_(max_gap_fill) {
    stop_event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    BT_CHECK_WIN32(static_cast<bool>(stop_event_));
    wake_event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    BT_CHECK_WIN32(static_cast<bool>(wake_event_));

    BT_CHECK_HR(CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&aac_)));
    const ComPtr<IMFMediaType> input = pcm_type();
    ComPtr<IMFMediaType> output;
    BT_CHECK_HR(MFCreateMediaType(&output));
    BT_CHECK_HR(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    BT_CHECK_HR(output->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC));
    BT_CHECK_HR(output->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, kOutputBits));
    BT_CHECK_HR(output->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kOutputRate));
    BT_CHECK_HR(output->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kOutputChannels));
    BT_CHECK_HR(output->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                  static_cast<UINT32>(bitrate_kbps) * kBitsPerKilobit / 8));
    BT_CHECK_HR(output->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, kAacPayloadRaw));
    BT_CHECK_HR(output->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, kAacProfileLevel));

    // The AAC encoder documents input first; some Windows builds only accept output first.
    HRESULT hr = aac_->SetInputType(0, input.Get(), 0);
    if (SUCCEEDED(hr)) {
        hr = aac_->SetOutputType(0, output.Get(), 0);
    }
    if (FAILED(hr)) {
        BT_CHECK_HR(aac_->SetOutputType(0, output.Get(), 0));
        BT_CHECK_HR(aac_->SetInputType(0, input.Get(), 0));
    }
    BT_CHECK_HR(aac_->GetOutputCurrentType(0, &output_type_));
    UINT32 config_size = 0;
    if (FAILED(output_type_->GetBlobSize(MF_MT_USER_DATA, &config_size)) || config_size == 0) {
        throw std::runtime_error("the AAC encoder gave no AudioSpecificConfig (MF_MT_USER_DATA)");
    }
    MFT_OUTPUT_STREAM_INFO info{};
    BT_CHECK_HR(aac_->GetOutputStreamInfo(0, &info));
    aac_output_size_ = info.cbSize != 0 ? info.cbSize : kAacFallbackOutputBytes;
    BT_CHECK_HR(aac_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    BT_CHECK_HR(aac_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    log::info("Audio encoder: AAC-LC {} kbit/s, {} Hz stereo", bitrate_kbps, kOutputRate);
}

AudioCapture::~AudioCapture() {
    stop();
}

void AudioCapture::start(Time100ns clock_offset) {
    if (thread_.joinable()) {
        if (running_) {
            return;
        }
        thread_.join();  // it ended on its own (failed); start a fresh one
    }
    clock_offset_ = clock_offset;
    failed_ = false;
    BT_CHECK_WIN32(ResetEvent(stop_event_.get()));
    running_ = true;
    thread_ = std::thread(&AudioCapture::thread_main, this);
}

void AudioCapture::stop() {
    if (!thread_.joinable()) {
        return;
    }
    SetEvent(stop_event_.get());
    thread_.join();
}
AudioStats AudioCapture::stats() const {
    AudioStats s;
    s.packets_out = counters_.packets_out;
    s.silence_inserted_ms = counters_.silence_inserted_ms;
    s.trimmed_ms = counters_.trimmed_ms;
    s.device_opens = counters_.device_opens;
    s.silent_packets = counters_.silent_packets;
    return s;
}

void AudioCapture::thread_main() {
    // §9: the audio thread never dies quietly. Device errors close the device and retry; only a
    // failure to reach the Windows audio system at all ends the thread, and failed() then tells
    // the owner. Video keeps recording either way.
    try {
        ComApartment com(COINIT_MULTITHREADED);
        run_loop();
    } catch (const std::exception& e) {
        log::error("Sound recording stopped: {}", e.what());
        failed_ = true;
    } catch (...) {
        log::error("Sound recording stopped: unknown exception");
        failed_ = true;
    }
    running_ = false;
}

void AudioCapture::run_loop() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    BT_CHECK_HR(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));
    ComPtr<DeviceNotifier> notifier;
    notifier.Attach(new DeviceNotifier(&device_changed_, wake_event_.get()));
    // §9: re-open the loopback when the default device changes (headset plugged in, etc.).
    const bool registered = SUCCEEDED(enumerator->RegisterEndpointNotificationCallback(notifier.Get()));
    if (!registered) {
        log::warn("Cannot watch for playback device changes; switching devices needs a restart");
    }
    // Windows keeps the notifier without taking a reference, so it is unregistered before it is
    // released on every way out of here, exceptions included. Declared after the notifier and
    // destroyed before it; the device is closed first, while COM is still initialised.
    struct Unregister {
        IMMDeviceEnumerator* enumerator;
        IMMNotificationClient* client;
        bool active;
        ~Unregister() {
            if (active) {
                const HRESULT unregistered = enumerator->UnregisterEndpointNotificationCallback(client);
                if (FAILED(unregistered)) {
                    log::debug("UnregisterEndpointNotificationCallback: {}", hresult_text(unregistered));
                }
            }
        }
    } unregister{enumerator.Get(), notifier.Get(), registered};
    struct CloseDevice {
        AudioCapture* self;
        ~CloseDevice() { self->close_device(); }
    } close_on_exit{this};

    ULONGLONG next_open = 0;
    while (true) {
        if (!open_ && GetTickCount64() >= next_open) {
            try {
                open_device(enumerator.Get());
            } catch (const std::exception& e) {
                if (error_log_.allow()) {
                    log::warn("Cannot record sound yet ({}), retrying", e.what());
                }
                close_device();
                next_open = GetTickCount64() + kReopenDelayMs;
            }
        }
        const HANDLE handles[] = {stop_event_.get(), wake_event_.get()};
        const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, kPollIntervalMs);
        if (wait == WAIT_OBJECT_0) {
            return;
        }
        if (device_changed_.exchange(false)) {
            // When the old device disappeared, the read below already failed and reopened on the
            // new default; Windows announces that same switch a moment later.
            if (open_ && !device_id_.empty() && default_device_id(enumerator.Get()) == device_id_) {
                continue;
            }
            log::info("Default playback device changed, switching sound recording to it");
            close_device();
            next_open = 0;
            continue;
        }
        if (!open_) {
            continue;
        }
        try {
            keep_render_alive();
        } catch (const std::exception& e) {
            // The silent stream only helps; loopback works without it (gaps are filled from the
            // timestamps), so a failure here never stops the recording.
            log::info("Stopped the silent keep-alive stream ({}); sound recording continues without it", e.what());
            drop_render_stream();
        }
        try {
            read_packets();
        } catch (const std::exception& e) {
            // AUDCLNT_E_DEVICE_INVALIDATED (unplugged, format changed in Sound settings) reopens
            // whatever is now the default device at once, unless the device was only just opened:
            // one that keeps failing right after opening is retried after a pause, not in a loop.
            const auto* hr_error = dynamic_cast<const HresultError*>(&e);
            const bool invalidated = hr_error != nullptr && hr_error->hr() == AUDCLNT_E_DEVICE_INVALIDATED;
            const bool settled = GetTickCount64() - opened_at_ms_ > kSettleMs;
            if (reopen_log_.allow()) {
                log::info("Sound recording interrupted ({}), reopening the device", e.what());
            }
            close_device();
            next_open = invalidated && settled ? 0 : GetTickCount64() + kReopenDelayMs;
        }
    }
}
void AudioCapture::open_device(IMMDeviceEnumerator* enumerator) {
    ComPtr<IMMDevice> device;
    BT_CHECK_HR(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device));
    const std::string name = device_name(device.Get());
    device_id_ = id_of(device.Get());

    BT_CHECK_HR(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**>(capture_client_audio_.ReleaseAndGetAddressOf())));
    WAVEFORMATEX* raw_format = nullptr;
    BT_CHECK_HR(capture_client_audio_->GetMixFormat(&raw_format));
    const std::unique_ptr<WAVEFORMATEX, CoTaskMemDeleter> format(raw_format);
    BT_CHECK_HR(capture_client_audio_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                                  kDeviceBufferDuration, 0, format.get(), nullptr));
    BT_CHECK_HR(capture_client_audio_->GetService(IID_PPV_ARGS(&capture_client_)));

    // Loopback delivers nothing at all while nothing plays, so a quiet moment would be missing
    // from the recording instead of silent. Playing silence on the same device keeps it flowing.
    // Best effort: without it the gaps are filled from the timestamps, slightly less exactly.
    try {
        BT_CHECK_HR(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(render_client_audio_.ReleaseAndGetAddressOf())));
        BT_CHECK_HR(render_client_audio_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kDeviceBufferDuration, 0,
                                                     format.get(), nullptr));
        BT_CHECK_HR(render_client_audio_->GetBufferSize(&render_buffer_frames_));
        BT_CHECK_HR(render_client_audio_->GetService(IID_PPV_ARGS(&render_client_)));
        keep_render_alive();
    } catch (const std::exception& e) {
        log::info("No silent keep-alive stream on this device ({}); recording sound without it", e.what());
        drop_render_stream();
    }

    // §6: the device format (usually 32-bit float, any rate and channel count) becomes 16-bit
    // 48 kHz stereo for the AAC encoder. Rebuilt on every open: a new device can differ in sample
    // type or channel layout even when rate and frame size match, and the old resampler would
    // turn that into noise.
    {
        ComPtr<IMFTransform> resampler;
        BT_CHECK_HR(CoCreateInstance(CLSID_CResamplerMediaObject, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&resampler)));
        ComPtr<IMFMediaType> input;
        BT_CHECK_HR(MFCreateMediaType(&input));
        BT_CHECK_HR(MFInitMediaTypeFromWaveFormatEx(input.Get(), format.get(),
                                                    static_cast<UINT32>(sizeof(WAVEFORMATEX) + format->cbSize)));
        BT_CHECK_HR(resampler->SetInputType(0, input.Get(), 0));
        BT_CHECK_HR(resampler->SetOutputType(0, pcm_type().Get(), 0));
        MFT_OUTPUT_STREAM_INFO info{};
        BT_CHECK_HR(resampler->GetOutputStreamInfo(0, &info));
        const DWORD chunk_bytes = kOutputRate * kOutputBlockAlign * kChunkMilliseconds / kMillisecondsPerSecond;
        resampler_output_size_ = std::max(info.cbSize, chunk_bytes);
        BT_CHECK_HR(resampler->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
        BT_CHECK_HR(resampler->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
        resampler_ = resampler;
        // A new sample rate starts a new segment of the timeline at the current position.
        if (device_rate_ != 0 && format->nSamplesPerSec != device_rate_) {
            segment_base_ += frames_to_time(segment_frames_, device_rate_);
            segment_frames_ = 0;
        }
        device_rate_ = format->nSamplesPerSec;
        block_align_ = format->nBlockAlign;
    }

    if (render_client_audio_) {
        const HRESULT started = render_client_audio_->Start();
        if (FAILED(started)) {
            log::info("The silent keep-alive stream did not start ({})", hresult_text(started));
            drop_render_stream();
        }
    }
    device_latency_.reset();  // a new device has its own latency
    BT_CHECK_HR(capture_client_audio_->Start());
    opened_at_ms_ = GetTickCount64();
    open_ = true;
    ++counters_.device_opens;
    log::info("Recording sound from {} ({} Hz, {} channels)", name, format->nSamplesPerSec, format->nChannels);
}

void AudioCapture::close_device() {
    if (capture_client_audio_) {
        const HRESULT stopped = capture_client_audio_->Stop();
        if (FAILED(stopped)) {
            log::debug("Loopback Stop: {}", hresult_text(stopped));  // the device may already be gone
        }
    }
    drop_render_stream();
    capture_client_.Reset();
    capture_client_audio_.Reset();
    // What the resampler still holds was already counted on the timeline; push it through to the
    // AAC encoder, otherwise every device switch or pause would lose a few milliseconds of sound
    // and the stamps would slowly fall behind.
    if (resampler_ && open_) {
        try {
            BT_CHECK_HR(resampler_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0));
            drain_resampler();
        } catch (const std::exception& e) {
            log::debug("Draining the resampler: {}", e.what());
        }
    }
    open_ = false;
}

void AudioCapture::drop_render_stream() {
    if (render_client_audio_) {
        const HRESULT stopped = render_client_audio_->Stop();
        if (FAILED(stopped)) {
            log::debug("Keep-alive stream Stop: {}", hresult_text(stopped));
        }
    }
    render_client_.Reset();
    render_client_audio_.Reset();
}

void AudioCapture::keep_render_alive() {
    if (!render_client_audio_) {
        return;
    }
    UINT32 padding = 0;
    BT_CHECK_HR(render_client_audio_->GetCurrentPadding(&padding));
    const UINT32 free_frames = render_buffer_frames_ - padding;
    if (free_frames == 0) {
        return;
    }
    BYTE* data = nullptr;
    BT_CHECK_HR(render_client_->GetBuffer(free_frames, &data));
    BT_CHECK_HR(render_client_->ReleaseBuffer(free_frames, AUDCLNT_BUFFERFLAGS_SILENT));
}

void AudioCapture::read_packets() {
    // Everything waiting is read first and handed back at once, then handled. Copied out because
    // filling a long gap with silence can take a while and WASAPI's buffer must not stay locked;
    // read as a batch because a poll usually finds two or three packets, and the latency estimate
    // must see the freshest of them before any is stamped (see handle_packet).
    std::size_t count = 0;
    while (true) {
        UINT32 next = 0;
        BT_CHECK_HR(capture_client_->GetNextPacketSize(&next));
        if (next == 0) {
            break;
        }
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 device_position = 0;
        UINT64 qpc_position = 0;
        const HRESULT hr = capture_client_->GetBuffer(&data, &frames, &flags, &device_position, &qpc_position);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) {
            break;
        }
        BT_CHECK_HR(hr);
        if (count == batch_.size()) {
            batch_.emplace_back();
        }
        BatchPacket& packet = batch_[count++];
        packet.frames = frames;
        packet.flags = flags;
        packet.qpc_position = qpc_position;
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0) {
            packet.bytes.assign(data, data + static_cast<std::size_t>(frames) * block_align_);
        }
        BT_CHECK_HR(capture_client_->ReleaseBuffer(frames));
    }
    if (count == 0) {
        return;
    }

    const Time100ns arrival = qpc_now();
    std::uint64_t frames_after = 0;  // frames in the batch after packet i
    for (std::size_t i = count; i-- > 0;) {
        batch_[i].frames_after = frames_after;
        frames_after += batch_[i].frames;
        if (batch_[i].qpc_position != 0 && (batch_[i].flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) == 0) {
            // Packet i was complete (readable) no later than the newer packets' duration before now.
            const Time100ns available = arrival - frames_to_time(batch_[i].frames_after, device_rate_);
            add_latency_sample(available, available - static_cast<Time100ns>(batch_[i].qpc_position));
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        const BatchPacket& packet = batch_[i];
        const bool silent = (packet.flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        const Time100ns available = arrival - frames_to_time(packet.frames_after, device_rate_);
        handle_packet(silent ? nullptr : packet.bytes.data(), packet.frames, packet.flags, packet.qpc_position,
                      available);
    }
}

void AudioCapture::add_latency_sample(Time100ns at, Time100ns value) {
    // Only the two-second minimum. A shorter window to notice a latency drop sooner (a Bluetooth
    // codec switch) was tried: Bluetooth stamps jitter enough that it fired again and again, and
    // every firing shifted the stamps and cut the sound (measured: 26 tone onsets for 8 tones).
    // After a real drop the old minimum lingers for at most the window, then corrects itself.
    device_latency_.add(at, value);
}

Time100ns AudioCapture::timeline() const {
    return anchor_ + segment_base_ + frames_to_time(segment_frames_, device_rate_);
}

void AudioCapture::handle_packet(const BYTE* data, UINT32 frames, DWORD flags, UINT64 qpc_position,
                                 Time100ns arrival) {
    // §3.3: a silent packet becomes real zeros. Dropping it would make the audio shorter than
    // the video and the muxer would stretch it.
    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
    if (silent) {
        ++counters_.silent_packets;
    }
    // §3.3: one clock. WASAPI stamps each packet with a QPC time in 100 ns units, but for loopback
    // that is when the device will *play* it: a Bluetooth headset reports ~170 ms in the future
    // (measured; it put the sound 155 ms behind the picture in every clip). The game made the
    // sound, and showed the matching frame, when it was mixed. A packet becomes readable as soon
    // as it is mixed, and reads come up to one poll late but never early, so the smallest
    // (arrival - stamp) over the last seconds is the device's latency without the poll jitter.
    // A stamp the driver flags as wrong (or does not give) is not used for the estimate: one bad
    // value would be the minimum for two seconds and shift every packet in that time. The
    // estimate itself is updated per batch in read_packets. `arrival` is when this packet was
    // complete at the latest (the batch's read time minus the newer packets after it).
    const Time100ns duration = frames_to_time(frames, device_rate_);
    const bool stamp_valid = qpc_position != 0 && (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) == 0;
    const std::optional<Time100ns> latency = device_latency_.value();
    const Time100ns mixed = stamp_valid && latency ? static_cast<Time100ns>(qpc_position) + *latency - duration
                                                   : arrival - duration;
    const Time100ns captured = mixed - clock_offset_;
    if (!anchored_) {
        anchored_ = true;
        anchor_ = captured;
        segment_base_ = 0;
        segment_frames_ = 0;
    }

    // §3.3: the timeline counts samples and is re-anchored against QPC. A discontinuity
    // (AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) shows up here as a gap and is filled like any other.
    const Time100ns drift = captured - timeline();
    if (drift > max_gap_fill_) {
        // The sound device was gone longer than any clip reaches back. Only the last
        // max_gap_fill_ of the gap is filled with silence, so no clip ever contains a hole, and
        // the timeline jumps over the rest. The resampler and AAC encoder are flushed first so the
        // stamps after the jump come from the new input times, not from samples they still hold.
        log::info("Sound was missing for {:.1f} s", static_cast<double>(drift) / kTicksPerSecond);
        BT_CHECK_HR(resampler_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0));
        BT_CHECK_HR(aac_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0));
        segment_base_ += drift - max_gap_fill_;
        // Whether the AAC encoder takes its stamps from the input after a flush is not
        // documented; the first packet after it is checked against where the timeline is.
        aac_expected_after_flush_ = timeline();
        check_aac_after_flush_ = true;
        encode_pcm(nullptr, static_cast<UINT32>(static_cast<std::uint64_t>(max_gap_fill_) * device_rate_ / kTicksPerSecond));
        counters_.silence_inserted_ms += static_cast<std::uint64_t>(max_gap_fill_ / kTicksPerMillisecond);
    } else if (drift > kDriftTolerance) {
        const auto missing = static_cast<UINT32>(static_cast<std::uint64_t>(drift) * device_rate_ / kTicksPerSecond);
        encode_pcm(nullptr, missing);
        counters_.silence_inserted_ms += static_cast<std::uint64_t>(drift / kTicksPerMillisecond);
    } else if (drift < -kDriftTolerance) {
        const auto ahead = static_cast<UINT32>(static_cast<std::uint64_t>(-drift) * device_rate_ / kTicksPerSecond);
        const UINT32 trim = std::min(ahead, frames);
        if (!silent) {
            data += static_cast<std::size_t>(trim) * block_align_;
        }
        frames -= trim;
        counters_.trimmed_ms += static_cast<std::uint64_t>(frames_to_time(trim, device_rate_) / kTicksPerMillisecond);
    }
    if (frames > 0) {
        encode_pcm(silent ? nullptr : data, frames);
    }
}

void AudioCapture::encode_pcm(const BYTE* data, UINT32 frames) {
    const UINT32 max_chunk = std::max<UINT32>(1, device_rate_ * kChunkMilliseconds / kMillisecondsPerSecond);
    while (frames > 0) {
        if (WaitForSingleObject(stop_event_.get(), 0) == WAIT_OBJECT_0) {
            return;  // a long silence fill must not hold up pause or quit
        }
        const UINT32 chunk = std::min(frames, max_chunk);
        const DWORD bytes = chunk * block_align_;
        const ComPtr<IMFSample> sample = make_buffer_sample(bytes);
        ComPtr<IMFMediaBuffer> buffer;
        BT_CHECK_HR(sample->GetBufferByIndex(0, &buffer));
        BYTE* target = nullptr;
        BT_CHECK_HR(buffer->Lock(&target, nullptr, nullptr));
        if (data != nullptr) {
            std::memcpy(target, data, bytes);
        } else {
            std::memset(target, 0, bytes);  // zero is silence for the float and signed formats WASAPI mixes in
        }
        BT_CHECK_HR(buffer->Unlock());
        BT_CHECK_HR(buffer->SetCurrentLength(bytes));
        BT_CHECK_HR(sample->SetSampleTime(timeline()));
        BT_CHECK_HR(sample->SetSampleDuration(frames_to_time(chunk, device_rate_)));

        HRESULT hr = resampler_->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            drain_resampler();
            hr = resampler_->ProcessInput(0, sample.Get(), 0);
        }
        BT_CHECK_HR(hr);
        segment_frames_ += chunk;  // only what the resampler accepted moves the timeline
        drain_resampler();
        if (data != nullptr) {
            data += bytes;
        }
        frames -= chunk;
    }
}

void AudioCapture::drain_resampler() {
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
        BT_CHECK_HR(output->GetBufferByIndex(0, &buffer));
        DWORD length = 0;
        BT_CHECK_HR(buffer->GetCurrentLength(&length));
        if (length == 0) {
            return;
        }
        encode_aac(output.Get());
    }
}

void AudioCapture::encode_aac(IMFSample* pcm) {
    HRESULT hr = aac_->ProcessInput(0, pcm, 0);
    if (hr == MF_E_NOTACCEPTING) {
        drain_aac();
        hr = aac_->ProcessInput(0, pcm, 0);
    }
    BT_CHECK_HR(hr);
    drain_aac();
}

void AudioCapture::drain_aac() {
    while (true) {
        const ComPtr<IMFSample> output = make_buffer_sample(aac_output_size_);
        MFT_OUTPUT_DATA_BUFFER out{};
        out.pSample = output.Get();
        DWORD status = 0;
        const HRESULT hr = aac_->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents != nullptr) {
            out.pEvents->Release();
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return;
        }
        BT_CHECK_HR(hr);
        LONGLONG pts = 0;
        if (FAILED(output->GetSampleTime(&pts))) {
            if (error_log_.allow()) {
                log::warn("AAC encoder produced a frame without a timestamp, dropped");
            }
            continue;
        }
        LONGLONG duration = 0;
        if (FAILED(output->GetSampleDuration(&duration)) || duration <= 0) {
            duration = frames_to_time(kAacFrameSamples, kOutputRate);
        }
        pts += aac_offset_;
        if (check_aac_after_flush_) {
            check_aac_after_flush_ = false;
            const Time100ns difference = aac_expected_after_flush_ - pts;
            if (difference > kAacFlushTolerance || difference < -kAacFlushTolerance) {
                log::warn("AAC encoder kept its own timing after a flush ({} ms off), correcting it",
                          difference / kTicksPerMillisecond);
                aac_offset_ += difference;
                pts += difference;
            }
        }
        ComPtr<IMFMediaBuffer> buffer;
        BT_CHECK_HR(output->ConvertToContiguousBuffer(&buffer));
        BYTE* data = nullptr;
        DWORD length = 0;
        BT_CHECK_HR(buffer->Lock(&data, nullptr, &length));
        struct Unlock {
            IMFMediaBuffer* buffer;
            ~Unlock() { buffer->Unlock(); }
        } unlock{buffer.Get()};
        if (length == 0) {
            continue;
        }
        Packet packet{pts, duration, true, make_packet_bytes(std::vector<std::uint8_t>(data, data + length))};
        ++counters_.packets_out;
        on_packet_(std::move(packet));
    }
}

}  // namespace bt
