#include "platform/VideoPlayer.h"

#include "platform/Log.h"
#include "platform/Text.h"

#include <mfapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace bt {
namespace {

// Hands the engine's events to the window. The engine may call after the player is gone (events
// already on their way): detach() makes those no-ops.
class Notify final : public IMFMediaEngineNotify {
public:
    Notify(HWND window, UINT message, LPARAM tag) : window_(window), message_(message), tag_(tag) {}

    void detach() {
        std::lock_guard lock(mutex_);
        window_ = nullptr;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (out == nullptr) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFMediaEngineNotify)) {
            *out = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --references_;
        if (left == 0) delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE EventNotify(DWORD event, DWORD_PTR, DWORD) override {
        // TIMEUPDATE comes several times a second; the window's own timer covers the position.
        if (event == MF_MEDIA_ENGINE_EVENT_TIMEUPDATE || event == MF_MEDIA_ENGINE_EVENT_PROGRESS) return S_OK;
        std::lock_guard lock(mutex_);
        if (window_ != nullptr) PostMessageW(window_, message_, event, tag_);
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{1};
    std::mutex mutex_;
    HWND window_;
    const UINT message_;
    const LPARAM tag_;
};

std::string media_error_text(IMFMediaEngine* engine) {
    ComPtr<IMFMediaError> error;
    if (FAILED(engine->GetError(&error)) || !error) return "This clip could not be played.";
    const USHORT code = error->GetErrorCode();
    const HRESULT extended = error->GetExtendedErrorCode();
    log::warn("The clip player reported error {} ({})", code, hresult_text(extended));
    switch (code) {
        case MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED: return "This file is not a video Windows can play.";
        case MF_MEDIA_ENGINE_ERR_DECODE: return "This clip is damaged and cannot be played.";
        case MF_MEDIA_ENGINE_ERR_NETWORK: return "The clip could not be read (drive unplugged?).";
        default: return "This clip could not be played.";
    }
}

}  // namespace

VideoPlayer::VideoPlayer(ID3D11Device* device, HWND window, UINT notify_message, LPARAM tag,
                         const std::filesystem::path& file)
    : device_(device) {
    UINT reset_token = 0;
    BT_CHECK_HR(MFCreateDXGIDeviceManager(&reset_token, &device_manager_));
    BT_CHECK_HR(device_manager_->ResetDevice(device_.Get(), reset_token));

    notify_.Attach(new Notify(window, notify_message, tag));
    ComPtr<IMFAttributes> attributes;
    BT_CHECK_HR(MFCreateAttributes(&attributes, 3));
    BT_CHECK_HR(attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, device_manager_.Get()));
    BT_CHECK_HR(attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify_.Get()));
    BT_CHECK_HR(attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM));

    ComPtr<IMFMediaEngineClassFactory> factory;
    BT_CHECK_HR(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    try {
        BT_CHECK_HR(factory->CreateInstance(0, attributes.Get(), &engine_));
        engine_->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
        engine_->SetAutoPlay(FALSE);
        BSTR url = SysAllocString(file.c_str());
        if (url == nullptr) throw std::bad_alloc();
        const HRESULT hr = engine_->SetSource(url);
        SysFreeString(url);
        BT_CHECK_HR(hr);
    } catch (...) {
        // The destructor does not run for a half-made player: stop the engine's threads here.
        static_cast<Notify*>(notify_.Get())->detach();
        if (engine_) engine_->Shutdown();
        throw;
    }
}

VideoPlayer::~VideoPlayer() {
    static_cast<Notify*>(notify_.Get())->detach();
    if (engine_) {
        // Stops its threads and closes the file before the device manager goes.
        const HRESULT hr = engine_->Shutdown();
        if (FAILED(hr)) log::warn("Closing the clip player failed: {}", hresult_text(hr));
    }
}

bool VideoPlayer::on_event(DWORD event) {
    switch (event) {
        case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
        case MF_MEDIA_ENGINE_EVENT_LOADEDDATA:
        case MF_MEDIA_ENGINE_EVENT_CANPLAY:
            ready_ = true;
            frame_pending_ = true;
            return true;
        case MF_MEDIA_ENGINE_EVENT_ERROR:
            error_ = media_error_text(engine_.Get());
            return true;
        case MF_MEDIA_ENGINE_EVENT_SEEKED:
            frame_pending_ = true;
            return true;
        case MF_MEDIA_ENGINE_EVENT_ENDED:
        case MF_MEDIA_ENGINE_EVENT_PLAYING:
        case MF_MEDIA_ENGINE_EVENT_PAUSE:
        case MF_MEDIA_ENGINE_EVENT_RATECHANGE:
        case MF_MEDIA_ENGINE_EVENT_DURATIONCHANGE:
            return true;
        default: return false;
    }
}

void VideoPlayer::play() {
    if (!error_.empty()) return;
    if (engine_->IsEnded()) engine_->SetCurrentTime(0.0);  // play again from the start
    const HRESULT hr = engine_->Play();
    if (FAILED(hr)) log::warn("Playing the clip failed: {}", hresult_text(hr));
}

void VideoPlayer::pause() {
    engine_->Pause();
}

bool VideoPlayer::playing() const {
    return engine_->IsPaused() == FALSE && engine_->IsEnded() == FALSE;
}

bool VideoPlayer::ended() const {
    return engine_->IsEnded() != FALSE;
}

void VideoPlayer::seek(double seconds) {
    const double length = duration();
    if (length > 0) seconds = std::clamp(seconds, 0.0, length);
    if (!std::isfinite(seconds) || seconds < 0) seconds = 0;
    engine_->SetCurrentTime(seconds);
    frame_pending_ = true;
}

double VideoPlayer::position() const {
    const double t = engine_->GetCurrentTime();
    return std::isfinite(t) && t > 0 ? t : 0.0;
}

double VideoPlayer::duration() const {
    const double d = engine_->GetDuration();
    return std::isfinite(d) && d > 0 ? d : 0.0;  // NaN before loading, +inf for live sources
}

void VideoPlayer::set_volume(double volume) {
    engine_->SetVolume(std::clamp(volume, 0.0, 1.0));
}

void VideoPlayer::set_muted(bool muted) {
    engine_->SetMuted(muted ? TRUE : FALSE);
}

void VideoPlayer::set_rate(double rate) {
    rate_ = std::clamp(rate, 0.25, 4.0);
    engine_->SetPlaybackRate(rate_);
    engine_->SetDefaultPlaybackRate(rate_);
}

bool VideoPlayer::update_frame() {
    if (!ready_ || !error_.empty()) return false;
    LONGLONG pts = 0;
    const bool fresh = engine_->OnVideoStreamTick(&pts) == S_OK;
    if (!fresh && !frame_pending_) return false;
    DWORD width = 0;
    DWORD height = 0;
    if (FAILED(engine_->GetNativeVideoSize(&width, &height)) || width == 0 || height == 0) return false;
    if (!texture_ || width != texture_width_ || height != texture_height_) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;
        const HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture);
        if (FAILED(hr)) {
            log::warn("Could not make the video frame texture ({}x{}): {}", width, height, hresult_text(hr));
            return false;
        }
        texture_ = texture;
        texture_width_ = width;
        texture_height_ = height;
    }
    const RECT target{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    const MFARGB black{0, 0, 0, 255};
    const HRESULT hr = engine_->TransferVideoFrame(texture_.Get(), nullptr, &target, &black);
    if (FAILED(hr)) {
        // Right after a seek there may be no frame yet: try again on the next tick.
        return false;
    }
    frame_pending_ = false;
    return true;
}

}  // namespace bt
