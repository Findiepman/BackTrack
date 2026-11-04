#pragma once

// Drawing for the Backtrack window: Direct2D for shapes and pictures, DirectWrite for text, both
// part of Windows (no runtime to ship). Everything is in DIPs (1/96 inch): the render target is
// given the window's DPI, so the same layout is sharp at 100 % and 150 % scaling.
//
// Icons come from the font Windows uses for its own icons: "Segoe Fluent Icons" on Windows 11,
// "Segoe MDL2 Assets" on Windows 10 (same code points for the ones used here).
//
// UI thread only.

#include "platform/Com.h"

#include <d2d1.h>
#include <dwrite.h>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace bt::ui {

struct Color {
    float r = 0, g = 0, b = 0, a = 1;
    static constexpr Color hex(std::uint32_t rgb, float alpha = 1.0f) {
        return Color{static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                     static_cast<float>(rgb & 0xFF) / 255.0f, alpha};
    }
    constexpr Color with_alpha(float alpha) const { return Color{r, g, b, alpha}; }
    // CSS color-mix(in srgb, this p, other): the mockup builds its soft and hover colours this way.
    constexpr Color mix(Color other, float p) const {
        return Color{r * p + other.r * (1 - p), g * p + other.g * (1 - p), b * p + other.b * (1 - p),
                     a * p + other.a * (1 - p)};
    }
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    Rect inset(float dx, float dy) const { return Rect{x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    D2D1_RECT_F d2d() const { return D2D1::RectF(x, y, x + w, y + h); }
};

enum class Weight { Regular = 400, Semibold = 600, Bold = 700 };
enum class Align { Start, Center, End };

struct TextStyle {
    float size = 14.0f;
    Weight weight = Weight::Regular;
    bool display = false;  // "Segoe UI Variable Display" for headings (falls back to Segoe UI)
    bool tabular = false;  // equal-width digits for times and sizes that change
};

class Graphics {
public:
    explicit Graphics(HWND window);
    Graphics(const Graphics&) = delete;
    Graphics& operator=(const Graphics&) = delete;

    // Pixel size of the client area changed, or the window moved to a monitor with another DPI.
    void resize(UINT width_px, UINT height_px);
    void set_dpi(UINT dpi);
    float scale() const { return dpi_ / 96.0f; }

    // False when there is nothing to draw on (window minimised to zero size).
    bool begin();
    // Ends the frame; a lost device (driver update, remote desktop) drops everything that lived
    // on it, and the next begin() makes it again.
    void end();

    void clear(Color color);
    void fill(const Rect& rect, Color color);
    void fill_round(const Rect& rect, float radius, Color color);
    void stroke_round(const Rect& rect, float radius, Color color, float width = 1.0f);
    void line(float x0, float y0, float x1, float y1, Color color, float width = 1.0f);
    void fill_circle(float cx, float cy, float radius, Color color);
    // A vertical gradient (thumbnails without a picture yet, shading under text).
    void fill_gradient(const Rect& rect, float radius, Color top, Color bottom);

    // One line, cut with an ellipsis when it does not fit. Returns the width actually used.
    float text(std::wstring_view text, const Rect& rect, const TextStyle& style, Color color,
               Align horizontal = Align::Start, Align vertical = Align::Center);
    // Wrapped text in a box, top aligned (notes, descriptions).
    void paragraph(std::wstring_view text, const Rect& rect, const TextStyle& style, Color color);
    float text_width(std::wstring_view text, const TextStyle& style);
    void icon(wchar_t glyph, const Rect& rect, float size, Color color);

    void push_clip(const Rect& rect);
    // Everything drawn until pop_clip() stays inside the rounded rectangle (card corners).
    void push_round_clip(const Rect& rect, float radius);
    void pop_clip();

    // Pictures (thumbnails) by key; uploaded once, kept until the device is lost or forget().
    bool has_bitmap(const std::wstring& key) const { return bitmaps_.contains(key); }
    void upload_bitmap(const std::wstring& key, const std::uint8_t* bgra, UINT width, UINT height);
    void forget_bitmap(const std::wstring& key);
    // Fills `rect` with the bitmap (cropped to keep its shape), with rounded corners.
    bool draw_bitmap(const std::wstring& key, const Rect& rect, float radius, float opacity = 1.0f);

private:
    void create_target();
    IDWriteTextFormat* format(const TextStyle& style, bool icon_font);
    ID2D1SolidColorBrush* brush(Color color);

    HWND window_;
    UINT dpi_ = 96;
    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> write_factory_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteInlineObject> ellipsis_;
    std::wstring text_family_;
    std::wstring display_family_;
    std::wstring icon_family_;
    std::map<std::tuple<int, int, bool, bool, bool>, ComPtr<IDWriteTextFormat>> formats_;
    std::map<std::wstring, ComPtr<ID2D1Bitmap>> bitmaps_;
    std::vector<bool> clips_;  // true: a layer (rounded clip), false: an axis-aligned clip
};

}  // namespace bt::ui
