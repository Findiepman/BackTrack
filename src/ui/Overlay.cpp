#include "ui/Overlay.h"

#include "platform/Log.h"

#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>

namespace bt::ui {
namespace {

constexpr wchar_t kClass[] = L"Backtrack.Overlay";
constexpr UINT_PTR kAnimationTimer = 1;
constexpr UINT kAnimationMs = 16;
constexpr auto kFlashLength = std::chrono::milliseconds(2500);
constexpr float kFadeSeconds = 0.15f;

// The chip, in DIPs (the mockup's .chipv: 30 high, 12 padding, a 7 px dot).
constexpr float kHeight = 30;
constexpr float kPad = 12;
constexpr float kDot = 7;
constexpr float kGap = 8;
constexpr float kMargin = 16;  // from the screen's edges
constexpr float kFontSize = 12.5f;

// GetVersionEx lies to programs without a manifest entry for each Windows; ntdll does not.
DWORD windows_build() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto get = ntdll != nullptr ? reinterpret_cast<RtlGetVersionFn>(
                                            reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))
                                      : nullptr;
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    return get != nullptr && get(&info) == 0 ? info.dwBuildNumber : 0;
}

D2D1_COLOR_F d2d(Color c) {
    return D2D1::ColorF(c.r, c.g, c.b, c.a);
}

}  // namespace

Overlay::Overlay() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kClass;
    if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        log::warn("The overlay is off: its window class could not be registered");
        return;
    }
    // Layered + transparent: drawn with per-pixel alpha and clicked through. Tool window: no
    // taskbar button, not in Alt+Tab. No-activate: never steals the game's focus.
    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              kClass, L"Backtrack overlay", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    if (window_ == nullptr) {
        log::warn("The overlay is off: its window could not be made");
        return;
    }
    // Before anything is drawn: from its first frame on it is left out of screen captures.
    // Before Windows 10 2004 (build 19041) the call "succeeds" but acts as WDA_MONITOR: a black
    // box in every recording. So the build decides, not the return value.
    excluded_ = windows_build() >= 19041 && SetWindowDisplayAffinity(window_, WDA_EXCLUDEFROMCAPTURE) != FALSE;
    if (!excluded_) {
        log::info("The overlay is off: this Windows cannot keep it out of recordings (needs Windows 10 2004 or later)");
        return;
    }
    try {
        BT_CHECK_HR(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.ReleaseAndGetAddressOf()));
        BT_CHECK_HR(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                        reinterpret_cast<IUnknown**>(dwrite_.ReleaseAndGetAddressOf())));
        // Premultiplied alpha into a 32-bit DIB is what UpdateLayeredWindow wants.
        const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
            // Software: a 30 px chip costs nothing to draw on the CPU, and a CPU target is never
            // lost with the graphics driver (sleep, driver update), which a hardware one is.
            D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        BT_CHECK_HR(d2d_->CreateDCRenderTarget(&properties, target_.ReleaseAndGetAddressOf()));
        // ClearType needs an opaque background to blend against; this one is see-through.
        target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    } catch (const std::exception& e) {
        log::warn("The overlay is off: {}", e.what());
        excluded_ = false;
    }
}

Overlay::~Overlay() {
    if (window_ != nullptr) {
        SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
        DestroyWindow(window_);
    }
}

LRESULT CALLBACK Overlay::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self != nullptr) {
        if (message == WM_TIMER && wparam == kAnimationTimer) {
            self->tick();
            return 0;
        }
        if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool Overlay::showing_flash() const {
    return !flash_text_.empty() && std::chrono::steady_clock::now() < flash_until_;
}

void Overlay::update(const OverlayState& state) {
    if (!available()) return;
    state_ = state;
    render();
}

void Overlay::flash(std::wstring text, Color dot) {
    if (!available()) return;
    flash_text_ = std::move(text);
    flash_dot_ = dot;
    flash_until_ = std::chrono::steady_clock::now() + kFlashLength;
    render();
}

void Overlay::tick() {
    const float step = static_cast<float>(kAnimationMs) / 1000.0f / kFadeSeconds;
    const float target = state_.visible ? 1.0f : 0.0f;
    fade_ = fade_ < target ? std::min(target, fade_ + step) : std::max(target, fade_ - step);
    if (!flash_text_.empty() && !showing_flash()) flash_text_.clear();  // back to the status
    render();
}

void Overlay::render() {
    const float target = state_.visible ? 1.0f : 0.0f;
    if (state_.reduce_motion) fade_ = target;
    const bool flashing = showing_flash();
    // The timer runs only while something moves: a fade, or a message waiting to expire.
    const bool animate = fade_ != target || flashing;
    if (animate != timer_) {
        if (animate) SetTimer(window_, kAnimationTimer, kAnimationMs, nullptr);
        else KillTimer(window_, kAnimationTimer);
        timer_ = animate;
    }
    if (fade_ <= 0.0f) {
        if (IsWindowVisible(window_)) ShowWindow(window_, SW_HIDE);
        drawn_alpha_ = -1.0f;
        drawn_ = Picture{};
        return;
    }

    const std::wstring& text = flashing ? flash_text_ : state_.text;
    const Color dot = flashing ? flash_dot_ : state_.dot;
    const float alpha = std::clamp(state_.opacity, 0.0f, 1.0f) * fade_;
    UINT dpi_x = 96;
    UINT dpi_y = 96;
    const HMONITOR monitor = MonitorFromRect(&state_.monitor, MONITOR_DEFAULTTONEAREST);
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) dpi_x = 96;
    const float s = static_cast<float>(dpi_x) / 96.0f;
    const RECT& m = state_.monitor;
    const Picture picture{text,
                          {dot.r, dot.g, dot.b, dot.a},
                          {state_.chip.r, state_.chip.g, state_.chip.b, state_.chip.a},
                          {state_.ink.r, state_.ink.g, state_.ink.b, state_.ink.a},
                          {m.left, m.top, m.right, m.bottom},
                          state_.corner,
                          dpi_x};
    const bool same_picture = picture == drawn_;

    BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(alpha * 255.0f)), AC_SRC_ALPHA};
    if (same_picture) {
        // Only the opacity changes (a fade step): no redraw.
        if (alpha != drawn_alpha_) {
            UpdateLayeredWindow(window_, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &blend, ULW_ALPHA);
            drawn_alpha_ = alpha;
        }
    } else {
        if (!format_ || std::abs(format_->GetFontSize() - kFontSize * s) > 0.01f) {
            format_.Reset();
            if (FAILED(dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, kFontSize * s,
                                                 L"en-us", format_.ReleaseAndGetAddressOf()))) {
                return;
            }
            format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format_.Get(), 2000.0f,
                                             kHeight * s, layout.GetAddressOf()))) {
            return;
        }
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        const int width = static_cast<int>(std::ceil((kPad + kDot + kGap + kPad) * s + metrics.widthIncludingTrailingWhitespace));
        const int height = static_cast<int>(std::ceil(kHeight * s));

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;  // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        const HDC screen = GetDC(nullptr);
        const HDC memory = CreateCompatibleDC(screen);
        const HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (memory == nullptr || bitmap == nullptr) {
            if (bitmap != nullptr) DeleteObject(bitmap);
            if (memory != nullptr) DeleteDC(memory);
            return;
        }
        const HGDIOBJ previous = SelectObject(memory, bitmap);
        const RECT area{0, 0, width, height};
        bool drawn = SUCCEEDED(target_->BindDC(memory, &area));
        if (drawn) {
            target_->BeginDraw();
            target_->Clear(D2D1::ColorF(0, 0, 0, 0));
            ComPtr<ID2D1SolidColorBrush> brush;
            target_->CreateSolidColorBrush(d2d(state_.chip), brush.GetAddressOf());
            if (brush) {
                const float w = static_cast<float>(width);
                const float h = static_cast<float>(height);
                const D2D1_ROUNDED_RECT chip{D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), h / 2, h / 2};
                target_->FillRoundedRectangle(chip, brush.Get());
                brush->SetColor(d2d(state_.ink.with_alpha(0.12f)));
                target_->DrawRoundedRectangle(chip, brush.Get(), 1.0f);
                brush->SetColor(d2d(dot));
                const float r = kDot * s / 2;
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kPad * s + r, h / 2), r, r), brush.Get());
                brush->SetColor(d2d(state_.ink));
                target_->DrawTextLayout(D2D1::Point2F((kPad + kDot + kGap) * s, 0), layout.Get(), brush.Get(),
                                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            drawn = SUCCEEDED(target_->EndDraw());
        }
        if (drawn) {
            const LONG margin = std::lround(kMargin * s);
            const bool left = state_.corner == 0 || state_.corner == 2;
            const bool top = state_.corner == 0 || state_.corner == 1;
            POINT position{left ? m.left + margin : m.right - margin - width, top ? m.top + margin : m.bottom - margin - height};
            SIZE size{width, height};
            POINT origin{0, 0};
            UpdateLayeredWindow(window_, nullptr, &position, &size, memory, &origin, 0, &blend, ULW_ALPHA);
            drawn_ = picture;
            drawn_alpha_ = alpha;
        }
        SelectObject(memory, previous);
        DeleteObject(bitmap);
        DeleteDC(memory);
        if (!drawn) {
            // Never leave an old message standing on the screen.
            ShowWindow(window_, SW_HIDE);
            drawn_ = Picture{};
            return;
        }
    }
    if (!IsWindowVisible(window_)) ShowWindow(window_, SW_SHOWNOACTIVATE);
    // A game that makes itself topmost later would cover it; keep it above.
    SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

}  // namespace bt::ui
