#pragma once

// Plays one clip on the clip page: Media Foundation's media engine (part of Windows) decodes the
// video on the window's own Direct3D device and plays the sound; the window pulls each new frame
// into a texture and draws it with the rest of the page (frame-server mode), so the controls can
// sit on top of the picture.
//
// UI thread only. The engine calls back on its own threads; those callbacks only post
// `notify_message` to the window, which then calls on_event().

#include "platform/Com.h"

#include <d3d11.h>
#include <mfmediaengine.h>

#include <filesystem>
#include <string>

namespace bt {

class VideoPlayer {
public:
    // `device` must have been made with D3D11_CREATE_DEVICE_VIDEO_SUPPORT and multithread
    // protection on. Opening starts at once; the first frame arrives a moment later.
    // Events arrive as `notify_message` with wparam = the event and lparam = `tag`.
    VideoPlayer(ID3D11Device* device, HWND window, UINT notify_message, LPARAM tag, const std::filesystem::path& file);
    ~VideoPlayer();
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    // After `notify_message` arrived (wparam = the MF_MEDIA_ENGINE_EVENT). True when the page
    // should be drawn again (loaded, ended, an error, a seek finished).
    bool on_event(DWORD event);

    void play();
    void pause();
    bool playing() const;
    bool ended() const;
    void seek(double seconds);
    double position() const;  // seconds
    double duration() const;  // 0 until loaded
    void set_volume(double volume);  // 0..1
    void set_muted(bool muted);
    void set_rate(double rate);  // 0.25..4
    double rate() const { return rate_; }

    bool ready() const { return ready_; }
    // Empty while all is well; one line for the user after the clip could not be played.
    const std::string& error() const { return error_; }

    // When the engine has a frame the window has not drawn yet, copies it into texture() and
    // returns true. Call on every timer tick while the page is shown.
    bool update_frame();
    // The last frame, or null before the first one.
    ID3D11Texture2D* texture() const { return texture_.Get(); }

private:
    ComPtr<ID3D11Device> device_;
    ComPtr<IMFDXGIDeviceManager> device_manager_;
    ComPtr<IMFMediaEngineNotify> notify_;
    ComPtr<IMFMediaEngine> engine_;
    ComPtr<ID3D11Texture2D> texture_;
    UINT texture_width_ = 0;
    UINT texture_height_ = 0;
    bool ready_ = false;
    bool frame_pending_ = true;  // a seek while paused: draw the frame it lands on
    double rate_ = 1.0;
    std::string error_;
};

}  // namespace bt
