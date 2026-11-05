#include "ui/Icon.h"

#include "platform/Com.h"
#include "ui/Logo.h"

#include <d2d1.h>
#include <wincodec.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace bt::ui {

std::vector<std::uint8_t> render_logo_pixels(int size_px) {
    if (size_px <= 0 || size_px > 1024) {
        return {};
    }
    const auto size = static_cast<UINT>(size_px);
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<ID2D1RenderTarget> target;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)) ||
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())) ||
        FAILED(d2d->CreateWicBitmapRenderTarget(
            bitmap.Get(),
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
            &target))) {
        return {};
    }
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0, 0, 0));
    draw_logo(target.Get(), d2d.Get(), D2D1::RectF(0, 0, static_cast<float>(size), static_cast<float>(size)));
    if (FAILED(target->EndDraw())) {
        return {};
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size) * size * 4);
    if (FAILED(bitmap->CopyPixels(nullptr, size * 4, static_cast<UINT>(pixels.size()), pixels.data()))) {
        return {};
    }
    // Icons take straight alpha; Direct2D drew premultiplied. Divide it out so the anti-aliased
    // rim is not darkened.
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        const std::uint8_t a = pixels[i + 3];
        if (a != 0 && a != 0xFF) {
            for (int c = 0; c < 3; ++c) {
                pixels[i + c] = static_cast<std::uint8_t>(std::min(255u, pixels[i + c] * 255u / a));
            }
        }
    }
    return pixels;
}

HICON make_app_icon(int size_px) {
    const std::vector<std::uint8_t> pixels = render_logo_pixels(size_px);
    if (pixels.empty()) {
        return nullptr;
    }
    const auto size = static_cast<UINT>(size_px);
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
