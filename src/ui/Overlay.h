#pragma once

// The in-screen overlay (Settings > Notifications & Overlay): a small status chip ("Replay on") in
// a corner of the display Instant Replay records, on top of games in borderless or windowed mode,
// and short messages in it ("Clip saved", "Bookmark added").
//
// It is never part of a recording: the window asks Windows to leave it out of every capture
// (WDA_EXCLUDEFROMCAPTURE, which the desktop duplication honours). On a Windows that cannot do
// that (before Windows 10 2004) the overlay stays off rather than show up in clips. Clicks go
// through it to the game (WS_EX_TRANSPARENT) and it never takes the focus.
//
// Exclusive fullscreen games draw past every window, so it does not show over those.
//
// UI thread only: made, updated and destroyed by the App on the window's thread.

#include "ui/Graphics.h"

#include <windows.h>

#include <chrono>
#include <string>

namespace bt::ui {

struct OverlayState {
    bool visible = false;
    std::wstring text;  // "Replay on"
    Color dot;
    RECT monitor{};  // the display it sits on, in screen pixels
    int corner = 1;  // OverlayCorner: 0 top left, 1 top right, 2 bottom left, 3 bottom right
    float opacity = 0.85f;
    bool reduce_motion = false;  // no fading
    Color chip;  // background
    Color ink;  // text
};

class Overlay {
public:
    Overlay();
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // False when Windows cannot keep it out of recordings: then it never shows.
    bool available() const { return window_ != nullptr && excluded_; }

    void update(const OverlayState& state);
    // A message in place of the status for a moment.
    void flash(std::wstring text, Color dot);

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    void tick();
    void render();
    bool showing_flash() const;

    HWND window_ = nullptr;
    bool excluded_ = false;
    OverlayState state_;
    std::wstring flash_text_;
    Color flash_dot_;
    std::chrono::steady_clock::time_point flash_until_{};
    float fade_ = 0.0f;  // 0 hidden .. 1 shown
    // What the layered window holds, to skip redrawing an unchanged chip (a fade step only
    // changes the opacity).
    struct Picture {
        std::wstring text;
        float dot[4]{}, chip[4]{}, ink[4]{};
        LONG monitor[4]{};
        int corner = -1;
        UINT dpi = 0;
        bool operator==(const Picture&) const = default;
    };
    Picture drawn_;
    float drawn_alpha_ = -1.0f;
    bool timer_ = false;

    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IDWriteTextFormat> format_;
    ComPtr<ID2D1DCRenderTarget> target_;
};

}  // namespace bt::ui
