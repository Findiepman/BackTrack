#include "ui/Graphics.h"

#include "platform/Log.h"
#include "ui/Logo.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace bt::ui {
namespace {

// The first family that exists on this PC.
std::wstring first_installed(IDWriteFactory* factory, std::initializer_list<const wchar_t*> families) {
    ComPtr<IDWriteFontCollection> fonts;
    if (SUCCEEDED(factory->GetSystemFontCollection(&fonts, FALSE))) {
        for (const wchar_t* family : families) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists) {
                return family;
            }
        }
    }
    return *(families.end() - 1);
}

D2D1_COLOR_F d2d(Color c) {
    return D2D1::ColorF(c.r, c.g, c.b, c.a);
}

}  // namespace

Graphics::Graphics(HWND window) : window_(window) {
    D2D1_FACTORY_OPTIONS options{};
    // Multithreaded: the clip player's decoder uses the same Direct3D device from its own threads,
    // and this factory type makes Direct2D take the device's lock while it draws.
    BT_CHECK_HR(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &options,
                                  reinterpret_cast<void**>(factory_.GetAddressOf())));
    BT_CHECK_HR(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                    reinterpret_cast<IUnknown**>(write_factory_.GetAddressOf())));
    text_family_ = first_installed(write_factory_.Get(), {L"Segoe UI Variable Text", L"Segoe UI"});
    display_family_ = first_installed(write_factory_.Get(), {L"Segoe UI Variable Display", L"Segoe UI"});
    icon_family_ = first_installed(write_factory_.Get(), {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"});
    dpi_ = GetDpiForWindow(window_);
    if (dpi_ == 0) dpi_ = 96;
}

void Graphics::create_device() {
    // BGRA for Direct2D, VIDEO_SUPPORT so Media Foundation can decode the clip player's video on
    // it. The player uses the device from its own threads: multithread protection on.
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    // The graphics card with video support first; without it only the clip player is missing;
    // software drawing when there is no usable driver (basic display adapter, some remote sessions).
    const struct {
        D3D_DRIVER_TYPE type;
        UINT flags;
    } attempts[] = {
        {D3D_DRIVER_TYPE_HARDWARE, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT},
        {D3D_DRIVER_TYPE_HARDWARE, D3D11_CREATE_DEVICE_BGRA_SUPPORT},
        {D3D_DRIVER_TYPE_WARP, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT},
        {D3D_DRIVER_TYPE_WARP, D3D11_CREATE_DEVICE_BGRA_SUPPORT},
    };
    HRESULT hr = E_FAIL;
    for (const auto& attempt : attempts) {
        hr = D3D11CreateDevice(nullptr, attempt.type, nullptr, attempt.flags, levels, static_cast<UINT>(std::size(levels)),
                               D3D11_SDK_VERSION, d3d_.ReleaseAndGetAddressOf(), nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            if (&attempt != &attempts[0]) {
                log::warn("The window draws with a {} device{}", attempt.type == D3D_DRIVER_TYPE_WARP ? "software" : "hardware",
                          (attempt.flags & D3D11_CREATE_DEVICE_VIDEO_SUPPORT) != 0 ? "" : " without video support");
            }
            break;
        }
    }
    BT_CHECK_HR(hr);
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(d3d_.As(&multithread))) multithread->SetMultithreadProtected(TRUE);

    ComPtr<IDXGIDevice> dxgi_device;
    BT_CHECK_HR(d3d_.As(&dxgi_device));
    BT_CHECK_HR(factory_->CreateDevice(dxgi_device.Get(), d2d_device_.ReleaseAndGetAddressOf()));
    BT_CHECK_HR(d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, target_.ReleaseAndGetAddressOf()));
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
    BT_CHECK_HR(target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), brush_.ReleaseAndGetAddressOf()));

    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> dxgi_factory;
    BT_CHECK_HR(dxgi_device->GetAdapter(&adapter));
    BT_CHECK_HR(adapter->GetParent(IID_PPV_ARGS(&dxgi_factory)));
    RECT client{};
    GetClientRect(window_, &client);
    width_px_ = static_cast<UINT>(std::max<LONG>(client.right, 1));
    height_px_ = static_cast<UINT>(std::max<LONG>(client.bottom, 1));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_px_;
    desc.Height = height_px_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_NONE;  // while resizing, the old frame stays put instead of stretching
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = dxgi_factory->CreateSwapChainForHwnd(d3d_.Get(), window_, &desc, nullptr, nullptr,
                                              swap_chain_.ReleaseAndGetAddressOf());
    if (hr == DXGI_ERROR_INVALID_CALL) {
        desc.Scaling = DXGI_SCALING_STRETCH;  // NONE is refused by some older drivers
        hr = dxgi_factory->CreateSwapChainForHwnd(d3d_.Get(), window_, &desc, nullptr, nullptr,
                                                  swap_chain_.ReleaseAndGetAddressOf());
    }
    BT_CHECK_HR(hr);
    dxgi_factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);
    ++generation_;
}

void Graphics::create_target() {
    ComPtr<IDXGISurface> surface;
    BT_CHECK_HR(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&surface)));
    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), static_cast<float>(dpi_),
        static_cast<float>(dpi_));
    BT_CHECK_HR(target_->CreateBitmapFromDxgiSurface(surface.Get(), &properties, back_buffer_.ReleaseAndGetAddressOf()));
    target_->SetTarget(back_buffer_.Get());
}

void Graphics::drop_device() {
    bitmaps_.clear();  // they lived on the old device; the owner uploads them again
    video_bitmap_.Reset();
    video_texture_.Reset();
    brush_.Reset();
    if (target_) target_->SetTarget(nullptr);
    back_buffer_.Reset();
    target_.Reset();
    d2d_device_.Reset();
    swap_chain_.Reset();
    d3d_.Reset();
}

bool Graphics::ensure_device() {
    if (d3d_) return true;
    try {
        create_device();
        return true;
    } catch (const std::exception& e) {
        log::warn("Could not set up drawing: {}", e.what());
        drop_device();
        return false;
    }
}

void Graphics::resize(UINT width_px, UINT height_px) {
    width_px = std::max(width_px, 1u);
    height_px = std::max(height_px, 1u);
    if (!swap_chain_ || (width_px == width_px_ && height_px == height_px_)) {
        return;
    }
    // Every reference to the old buffers has to go before they can be resized.
    target_->SetTarget(nullptr);
    back_buffer_.Reset();
    const HRESULT hr = swap_chain_->ResizeBuffers(0, width_px, height_px, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        log::warn("Resizing the window's drawing surface failed ({}); starting over", hresult_text(hr));
        drop_device();
        return;
    }
    width_px_ = width_px;
    height_px_ = height_px;
}

void Graphics::set_dpi(UINT dpi) {
    dpi_ = dpi == 0 ? 96 : dpi;
    if (target_) {
        target_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
        // The target bitmap carries the DPI too: made again at the next begin().
        target_->SetTarget(nullptr);
        back_buffer_.Reset();
    }
}

bool Graphics::begin() {
    RECT client{};
    GetClientRect(window_, &client);
    if (client.right <= 0 || client.bottom <= 0) {
        return false;
    }
    if (!ensure_device()) {
        return false;
    }
    resize(static_cast<UINT>(client.right), static_cast<UINT>(client.bottom));
    if (!d3d_ && !ensure_device()) {
        return false;  // resizing failed and the device could not be made again
    }
    if (!back_buffer_) {
        create_target();
    }
    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    return true;
}

void Graphics::end() {
    while (!clips_.empty()) {
        pop_clip();
    }
    HRESULT hr = target_->EndDraw();
    if (SUCCEEDED(hr)) {
        // Waits for the next vertical blank at most, so a playing video repaints at the screen's rate.
        hr = swap_chain_->Present(1, 0);
        if (hr == DXGI_STATUS_OCCLUDED) hr = S_OK;
    }
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        log::info("The graphics device was lost; redrawing the window on a new one");
        drop_device();
        InvalidateRect(window_, nullptr, FALSE);
    } else if (FAILED(hr)) {
        log::warn("Drawing the window failed: {}", hresult_text(hr));
    }
}

ID2D1SolidColorBrush* Graphics::brush(Color color) {
    brush_->SetColor(d2d(color));
    return brush_.Get();
}

void Graphics::clear(Color color) {
    target_->Clear(d2d(color));
}

void Graphics::fill(const Rect& rect, Color color) {
    target_->FillRectangle(rect.d2d(), brush(color));
}

void Graphics::fill_round(const Rect& rect, float radius, Color color) {
    target_->FillRoundedRectangle(D2D1::RoundedRect(rect.d2d(), radius, radius), brush(color));
}

void Graphics::stroke_round(const Rect& rect, float radius, Color color, float width) {
    // Half a stroke inside, so a 1 px border sits on whole pixels and stays crisp.
    const Rect r = rect.inset(width / 2, width / 2);
    target_->DrawRoundedRectangle(D2D1::RoundedRect(r.d2d(), radius, radius), brush(color), width);
}

void Graphics::line(float x0, float y0, float x1, float y1, Color color, float width) {
    target_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush(color), width);
}

void Graphics::fill_circle(float cx, float cy, float radius, Color color) {
    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush(color));
}

void Graphics::fill_gradient(const Rect& rect, float radius, Color top, Color bottom) {
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, d2d(top)}, {1.0f, d2d(bottom)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> gradient;
    if (FAILED(target_->CreateGradientStopCollection(stops, 2, &collection)) ||
        FAILED(target_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(rect.x, rect.y), D2D1::Point2F(rect.x, rect.bottom())),
            collection.Get(), &gradient))) {
        fill_round(rect, radius, top);
        return;
    }
    target_->FillRoundedRectangle(D2D1::RoundedRect(rect.d2d(), radius, radius), gradient.Get());
}

IDWriteTextFormat* Graphics::format(const TextStyle& style, bool icon_font) {
    const auto key = std::make_tuple(static_cast<int>(std::lround(style.size * 10)), static_cast<int>(style.weight),
                                     style.display, style.tabular, icon_font);
    if (const auto it = formats_.find(key); it != formats_.end()) {
        return it->second.Get();
    }
    const std::wstring& family = icon_font ? icon_family_ : style.display ? display_family_ : text_family_;
    ComPtr<IDWriteTextFormat> created;
    BT_CHECK_HR(write_factory_->CreateTextFormat(family.c_str(), nullptr, static_cast<DWRITE_FONT_WEIGHT>(style.weight),
                                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, style.size,
                                                 L"", &created));
    created->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!ellipsis_) {
        write_factory_->CreateEllipsisTrimmingSign(created.Get(), &ellipsis_);
    }
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    created->SetTrimming(&trimming, ellipsis_.Get());
    formats_[key] = created;
    return created.Get();
}

float Graphics::text(std::wstring_view text, const Rect& rect, const TextStyle& style, Color color, Align horizontal,
                     Align vertical) {
    if (text.empty() || rect.w <= 0 || rect.h <= 0) {
        return 0.0f;
    }
    IDWriteTextFormat* fmt = format(style, false);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(write_factory_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), fmt, rect.w, rect.h,
                                                &layout))) {
        return 0.0f;
    }
    layout->SetTextAlignment(horizontal == Align::Start    ? DWRITE_TEXT_ALIGNMENT_LEADING
                             : horizontal == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                           : DWRITE_TEXT_ALIGNMENT_TRAILING);
    layout->SetParagraphAlignment(vertical == Align::Start    ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR
                                  : vertical == Align::Center ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER
                                                              : DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    if (style.tabular) {
        ComPtr<IDWriteTypography> typography;
        if (SUCCEEDED(write_factory_->CreateTypography(&typography))) {
            typography->AddFontFeature(DWRITE_FONT_FEATURE{DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
            layout->SetTypography(typography.Get(), DWRITE_TEXT_RANGE{0, static_cast<UINT32>(text.size())});
        }
    }
    target_->DrawTextLayout(D2D1::Point2F(rect.x, rect.y), layout.Get(), brush(color), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return std::min(metrics.widthIncludingTrailingWhitespace, rect.w);
}

void Graphics::paragraph(std::wstring_view text, const Rect& rect, const TextStyle& style, Color color) {
    if (text.empty() || rect.w <= 0 || rect.h <= 0) {
        return;
    }
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(write_factory_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format(style, false),
                                                rect.w, rect.h, &layout))) {
        return;
    }
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    // Text longer than the box ends in "..." on its last visible line instead of being cut through.
    if (ellipsis_) {
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_WORD, 0, 0};
        layout->SetTrimming(&trimming, ellipsis_.Get());
    }
    target_->DrawTextLayout(D2D1::Point2F(rect.x, rect.y), layout.Get(), brush(color), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Graphics::text_width(std::wstring_view text, const TextStyle& style) {
    if (text.empty()) {
        return 0.0f;
    }
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(write_factory_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format(style, false),
                                                100000.0f, 1000.0f, &layout))) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

void Graphics::icon(wchar_t glyph, const Rect& rect, float size, Color color) {
    IDWriteTextFormat* fmt = format(TextStyle{size, Weight::Regular, false, false}, true);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(write_factory_->CreateTextLayout(&glyph, 1, fmt, rect.w, rect.h, &layout))) {
        return;
    }
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    target_->DrawTextLayout(D2D1::Point2F(rect.x, rect.y), layout.Get(), brush(color));
}

void Graphics::logo(const Rect& rect) {
    draw_logo(target_.Get(), factory_.Get(), rect.d2d());
}

void Graphics::push_clip(const Rect& rect) {
    target_->PushAxisAlignedClip(rect.d2d(), D2D1_ANTIALIAS_MODE_ALIASED);
    clips_.push_back(false);
}

void Graphics::push_round_clip(const Rect& rect, float radius) {
    ComPtr<ID2D1RoundedRectangleGeometry> shape;
    if (FAILED(factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rect.d2d(), radius, radius), &shape))) {
        push_clip(rect);  // square corners are better than drawing outside the card
        return;
    }
    // A null layer: Direct2D allocates and reuses the layer surface itself (Windows 8 and later).
    target_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), shape.Get()), nullptr);
    clips_.push_back(true);
}

void Graphics::pop_clip() {
    if (clips_.empty()) {
        return;
    }
    if (clips_.back()) {
        target_->PopLayer();
    } else {
        target_->PopAxisAlignedClip();
    }
    clips_.pop_back();
}

void Graphics::upload_bitmap(const std::wstring& key, const std::uint8_t* bgra, UINT width, UINT height) {
    if (!target_ || bgra == nullptr || width == 0 || height == 0) {
        return;
    }
    ComPtr<ID2D1Bitmap> bitmap;
    const D2D1_BITMAP_PROPERTIES properties =
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    if (SUCCEEDED(target_->CreateBitmap(D2D1::SizeU(width, height), bgra, width * 4, properties, &bitmap))) {
        bitmaps_[key] = bitmap;
    }
}

void Graphics::forget_bitmap(const std::wstring& key) {
    bitmaps_.erase(key);
}

bool Graphics::draw_bitmap(const std::wstring& key, const Rect& rect, float radius, float opacity) {
    const auto it = bitmaps_.find(key);
    if (it == bitmaps_.end() || rect.w <= 0 || rect.h <= 0) {
        return false;
    }
    ID2D1Bitmap* bitmap = it->second.Get();
    const D2D1_SIZE_F size = bitmap->GetSize();
    // Cover: scale so the bitmap fills the box, centred, cropping the overflow.
    const float scale = std::max(rect.w / size.width, rect.h / size.height);
    const float w = size.width * scale;
    const float h = size.height * scale;
    ComPtr<ID2D1BitmapBrush> pattern;
    if (FAILED(target_->CreateBitmapBrush(bitmap, &pattern))) {
        return false;
    }
    pattern->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    pattern->SetOpacity(opacity);
    pattern->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                          D2D1::Matrix3x2F::Translation(rect.x + (rect.w - w) / 2, rect.y + (rect.h - h) / 2));
    target_->FillRoundedRectangle(D2D1::RoundedRect(rect.d2d(), radius, radius), pattern.Get());
    return true;
}

bool Graphics::draw_texture(ID3D11Texture2D* texture, const Rect& rect) {
    if (texture == nullptr || !target_ || rect.w <= 0 || rect.h <= 0) {
        return false;
    }
    if (texture != video_texture_.Get()) {
        ComPtr<IDXGISurface> surface;
        if (FAILED(texture->QueryInterface(IID_PPV_ARGS(&surface)))) return false;
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        ComPtr<ID2D1Bitmap1> bitmap;
        if (FAILED(target_->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &bitmap))) return false;
        video_bitmap_ = bitmap;
        video_texture_ = texture;
    }
    const D2D1_SIZE_U pixels = video_bitmap_->GetPixelSize();
    if (pixels.width == 0 || pixels.height == 0) return false;
    // Contain: the whole frame, centred, black bars where the shapes differ.
    const float scale = std::min(rect.w / static_cast<float>(pixels.width), rect.h / static_cast<float>(pixels.height));
    const float w = static_cast<float>(pixels.width) * scale;
    const float h = static_cast<float>(pixels.height) * scale;
    const D2D1_RECT_F destination = D2D1::RectF(rect.x + (rect.w - w) / 2, rect.y + (rect.h - h) / 2,
                                                rect.x + (rect.w + w) / 2, rect.y + (rect.h + h) / 2);
    target_->DrawBitmap(video_bitmap_.Get(), destination, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
    return true;
}

}  // namespace bt::ui
