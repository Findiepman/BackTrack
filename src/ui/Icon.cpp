#include "ui/Icon.h"

#include "platform/Com.h"
#include "ui/Theme.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace bt::ui {

HICON make_app_icon(int size_px) {
    if (size_px <= 0) {
        return nullptr;
    }
    const auto size = static_cast<UINT>(size_px);
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1RenderTarget> target;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)) ||
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()))) ||
        FAILED(d2d->CreateWicBitmapRenderTarget(
            bitmap.Get(),
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
            &target))) {
        return nullptr;
    }

    const float s = static_cast<float>(size);
    ComPtr<ID2D1SolidColorBrush> fill;
    ComPtr<ID2D1SolidColorBrush> ink;
    target->CreateSolidColorBrush(D2D1::ColorF(theme::kAccent.r, theme::kAccent.g, theme::kAccent.b), &fill);
    target->CreateSolidColorBrush(D2D1::ColorF(theme::kOnAccent.r, theme::kOnAccent.g, theme::kOnAccent.b), &ink);
    ComPtr<IDWriteTextFormat> glyph_format;
    const wchar_t* family = L"Segoe Fluent Icons";
    {
        ComPtr<IDWriteFontCollection> fonts;
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(dwrite->GetSystemFontCollection(&fonts, FALSE)) &&
            (FAILED(fonts->FindFamilyName(family, &index, &exists)) || !exists)) {
            family = L"Segoe MDL2 Assets";
        }
    }
    if (!fill || !ink ||
        FAILED(dwrite->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, s * 0.62f, L"", &glyph_format))) {
        return nullptr;
    }
    glyph_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    glyph_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0, 0, 0));
    const float radius = s * 0.24f;
    target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, s, s), radius, radius), fill.Get());
    constexpr wchar_t kRewind = theme::glyph::kAppMark;
    target->DrawText(&kRewind, 1, glyph_format.Get(), D2D1::RectF(0, 0, s, s), ink.Get());
    if (FAILED(target->EndDraw())) {
        return nullptr;
    }

    std::vector<BYTE> pixels(static_cast<std::size_t>(size) * size * 4);
    if (FAILED(bitmap->CopyPixels(nullptr, size * 4, static_cast<UINT>(pixels.size()), pixels.data()))) {
        return nullptr;
    }
    // Icons take straight alpha; Direct2D drew premultiplied. Divide it out so the anti-aliased
    // rim is not darkened.
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        const BYTE a = pixels[i + 3];
        if (a != 0 && a != 0xFF) {
            for (int c = 0; c < 3; ++c) {
                pixels[i + c] = static_cast<BYTE>(std::min(255u, pixels[i + c] * 255u / a));
            }
        }
    }
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = size_px;
    header.bV5Height = -size_px;  // top-down
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    const HDC screen = GetDC(nullptr);
    const HBITMAP color =
        CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (color == nullptr || bits == nullptr) {
        if (color != nullptr) DeleteObject(color);
        return nullptr;
    }
    std::memcpy(bits, pixels.data(), pixels.size());
    // The AND mask is ignored for 32-bit icons by most of Windows, but not by all (high contrast):
    // all zero, so it never hides the picture.
    const std::vector<BYTE> mask_bits(static_cast<std::size_t>((size_px + 15) / 16 * 2) * size, 0);
    const HBITMAP mask = CreateBitmap(size_px, size_px, 1, 1, mask_bits.data());
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    const HICON icon = CreateIconIndirect(&info);
    DeleteObject(color);
    if (mask != nullptr) DeleteObject(mask);
    return icon;
}

}  // namespace bt::ui
