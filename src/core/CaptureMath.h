#pragma once

// The arithmetic of the screen capture that does not need a GPU: the recording size for the
// resolution setting, where the mouse pointer goes on a rotated monitor and what its pixels do to
// the desktop under them. ScreenCapture draws exactly what composite_pixel() computes, with a
// pixel shader (src/platform/shaders/CursorPS.hlsl).

#include <cstdint>
#include <span>
#include <vector>

namespace bt {

struct PixelSize {
    int width = 0;
    int height = 0;
    bool operator==(const PixelSize&) const = default;
};

// The recording size for a display of `natural` pixels and the resolution setting (0 = the
// display's own size, else 720, 1080 or 1440). The setting is the short side, so "1080" on a
// portrait monitor is 1080 wide, as phone video calls 1080x1920 "1080p". The other side keeps the
// display's shape, rounded to an even number (NV12 has one colour sample per 2x2 pixels). Never
// scaled up: a 1080p display with 1440 records 1080p.
PixelSize scaled_output_size(PixelSize natural, int resolution);

// How the captured texture has to be turned clockwise to show the desktop the right way up
// (DXGI_MODE_ROTATION: ROTATE90 = Clockwise90, the same mapping the video processor is given).
enum class Rotation { None, Clockwise90, Clockwise180, Clockwise270 };

struct PixelPoint {
    int x = 0;
    int y = 0;
    bool operator==(const PixelPoint&) const = default;
};

struct PixelRect {
    int left = 0;
    int top = 0;
    int right = 0;  // exclusive
    int bottom = 0;  // exclusive
    int width() const { return right - left; }
    int height() const { return bottom - top; }
    bool empty() const { return right <= left || bottom <= top; }
    bool operator==(const PixelRect&) const = default;
};

PixelRect intersect(const PixelRect& a, const PixelRect& b);

// The texture pixel that shows desktop pixel `p` of a desktop `desktop` pixels in size (desktop
// orientation, e.g. 1080x1920 for a portrait monitor whose texture is 1920x1080).
PixelPoint desktop_to_texture(PixelPoint p, PixelSize desktop, Rotation rotation);

// One pointer pixel: out = (desktop * (255 - alpha) / 255 + colour) XOR xor_mask, per colour
// channel. Both are packed like a little-endian BGRA pixel: B | G << 8 | R << 16 | A << 24. The
// colour is premultiplied by its alpha. This one form covers all three DXGI pointer types: an
// alpha-blended pixel has xor_mask 0, a pixel that inverts the screen has colour 0 and xor_mask
// 0xFFFFFF, a transparent one has both 0.
struct CursorPixel {
    std::uint32_t colour = 0;
    std::uint32_t xor_mask = 0;
    bool operator==(const CursorPixel&) const = default;
};

struct CursorImage {
    int width = 0;
    int height = 0;
    std::vector<CursorPixel> pixels;  // row by row, width * height
    bool empty() const { return pixels.empty(); }
};

// DXGI_OUTDUPL_POINTER_SHAPE_TYPE, under names that need no Windows header.
enum class CursorShapeType { Monochrome, Color, MaskedColor };

struct CursorShape {
    CursorShapeType type = CursorShapeType::Color;
    int width = 0;
    int height = 0;  // as DXGI reports it: twice the pointer's height for Monochrome (AND then XOR mask)
    int pitch = 0;  // bytes per row
    std::span<const std::uint8_t> data;
};

// The pointer image in desktop orientation. Empty when the shape is not a valid image (sizes that
// do not fit the data): then nothing is drawn rather than garbage.
//   Color: BGRA, alpha-blended. The colour is already premultiplied by alpha (measured on Windows
//     11: in every semi-transparent pixel of the arrow and the I-beam no channel exceeds alpha, and
//     the arrow's light outline is 214 at alpha 237), so it is used as it is. A channel above its
//     alpha (a pointer that does not follow that) saturates at white instead of wrapping.
//   MaskedColor: BGRA; alpha 0 replaces the screen pixel with the colour, alpha 0xFF XORs it.
//   Monochrome: 1 bit per pixel, the leftmost pixel in the highest bit. AND 0 XOR 0 black,
//     AND 0 XOR 1 white, AND 1 XOR 0 transparent, AND 1 XOR 1 inverts the screen.
CursorImage convert_cursor_shape(const CursorShape& shape);

// Where the pointer lands in the captured texture: `image` (desktop orientation) with its top-left
// corner at desktop pixel `position` (relative to the monitor, may be negative or past the edge).
struct CursorPlacement {
    CursorImage image;  // turned into texture orientation
    PixelRect rect;  // in texture pixels, unclipped: the size of `image`
    PixelRect visible;  // rect clipped to the texture; empty when the pointer is off the monitor
};

CursorPlacement place_cursor(const CursorImage& image, PixelPoint position, PixelSize desktop, Rotation rotation);

// Only the texture rectangle of place_cursor, without turning the image: a turned image is the
// same wherever the pointer is, so it is made once per shape and this is all a move costs.
PixelRect cursor_rect(PixelSize image, PixelPoint position, PixelSize desktop, Rotation rotation);

// The size of the captured texture for a desktop of this size (desktop orientation).
PixelSize texture_size(PixelSize desktop, Rotation rotation);

// Reference for the pixel shader and the tests: one BGRA desktop pixel (packed as above) with the
// pointer pixel over it. The desktop's alpha byte is kept.
std::uint32_t composite_pixel(std::uint32_t desktop, CursorPixel cursor);

}  // namespace bt
