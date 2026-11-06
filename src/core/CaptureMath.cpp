#include "core/CaptureMath.h"

#include <algorithm>

namespace bt {
namespace {

constexpr std::uint32_t kOpaque = 0xFF000000u;
constexpr std::uint32_t kRgb = 0x00FFFFFFu;
constexpr std::uint32_t kWhite = 0x00FFFFFFu;
constexpr int kBitsPerByte = 8;
constexpr int kBytesPerPixel = 4;
// MaskedColor only allows 0 and 0xFF; anything else is taken as the nearer of the two.
constexpr std::uint32_t kMaskThreshold = 0x80;

std::uint32_t read_bgra(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

std::uint32_t channel(std::uint32_t pixel, int index) {
    return (pixel >> (index * kBitsPerByte)) & 0xFFu;
}

bool bit(const std::uint8_t* row, int x) {
    return (row[x / kBitsPerByte] >> (7 - x % kBitsPerByte)) & 1;
}

bool sideways(Rotation rotation) {
    return rotation == Rotation::Clockwise90 || rotation == Rotation::Clockwise270;
}

}  // namespace

PixelSize scaled_output_size(PixelSize natural, int resolution) {
    if (natural.width <= 0 || natural.height <= 0) {
        return natural;
    }
    const bool portrait = natural.height > natural.width;
    const int short_side = portrait ? natural.width : natural.height;
    const int long_side = portrait ? natural.height : natural.width;
    if (resolution <= 0 || resolution >= short_side) {
        return natural;
    }
    const int target = resolution & ~1;
    // The nearest even number to long_side * target / short_side: 2 * round(x / 2), in integers.
    const long long product = static_cast<long long>(long_side) * target;
    const int scaled_long = std::max(2, static_cast<int>(2 * ((product + short_side) / (2LL * short_side))));
    return portrait ? PixelSize{target, scaled_long} : PixelSize{scaled_long, target};
}

PixelRect intersect(const PixelRect& a, const PixelRect& b) {
    PixelRect r{std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right),
                std::min(a.bottom, b.bottom)};
    if (r.empty()) {
        return PixelRect{};
    }
    return r;
}

PixelPoint desktop_to_texture(PixelPoint p, PixelSize desktop, Rotation rotation) {
    // The texture is the desktop turned back: Clockwise90 means texture turned 90 degrees clockwise
    // is the desktop, so a desktop pixel comes from the texture turned the other way.
    switch (rotation) {
        case Rotation::Clockwise90: return PixelPoint{p.y, desktop.width - 1 - p.x};
        case Rotation::Clockwise180: return PixelPoint{desktop.width - 1 - p.x, desktop.height - 1 - p.y};
        case Rotation::Clockwise270: return PixelPoint{desktop.height - 1 - p.y, p.x};
        case Rotation::None: break;
    }
    return p;
}

CursorImage convert_cursor_shape(const CursorShape& shape) {
    CursorImage image;
    if (shape.width <= 0 || shape.height <= 0 || shape.pitch <= 0) {
        return image;
    }
    const bool monochrome = shape.type == CursorShapeType::Monochrome;
    const int height = monochrome ? shape.height / 2 : shape.height;
    const int rows = monochrome ? height * 2 : height;
    const std::size_t needed_pitch = monochrome
        ? (static_cast<std::size_t>(shape.width) + kBitsPerByte - 1) / kBitsPerByte
        : static_cast<std::size_t>(shape.width) * kBytesPerPixel;
    if (height <= 0 || static_cast<std::size_t>(shape.pitch) < needed_pitch ||
        shape.data.size() < static_cast<std::size_t>(shape.pitch) * static_cast<std::size_t>(rows - 1) + needed_pitch) {
        return image;
    }

    image.width = shape.width;
    image.height = height;
    image.pixels.resize(static_cast<std::size_t>(shape.width) * static_cast<std::size_t>(height));
    const auto pitch = static_cast<std::size_t>(shape.pitch);
    for (int y = 0; y < height; ++y) {
        CursorPixel* out = image.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(shape.width);
        if (monochrome) {
            const std::uint8_t* and_row = shape.data.data() + static_cast<std::size_t>(y) * pitch;
            const std::uint8_t* xor_row = shape.data.data() + static_cast<std::size_t>(y + height) * pitch;
            for (int x = 0; x < shape.width; ++x) {
                const std::uint32_t xor_value = bit(xor_row, x) ? kWhite : 0;
                out[x] = bit(and_row, x) ? CursorPixel{0, xor_value} : CursorPixel{kOpaque | xor_value, 0};
            }
            continue;
        }
        const std::uint8_t* row = shape.data.data() + static_cast<std::size_t>(y) * pitch;
        for (int x = 0; x < shape.width; ++x) {
            const std::uint32_t pixel = read_bgra(row + static_cast<std::size_t>(x) * kBytesPerPixel);
            if (shape.type == CursorShapeType::MaskedColor) {
                out[x] = channel(pixel, 3) >= kMaskThreshold ? CursorPixel{0, pixel & kRgb}
                                                             : CursorPixel{kOpaque | (pixel & kRgb), 0};
            } else {
                out[x] = CursorPixel{pixel, 0};
            }
        }
    }
    return image;
}

CursorPlacement place_cursor(const CursorImage& image, PixelPoint position, PixelSize desktop, Rotation rotation) {
    CursorPlacement placement;
    if (image.empty()) {
        return placement;
    }
    placement.rect = cursor_rect(PixelSize{image.width, image.height}, position, desktop, rotation);
    const PixelSize texture = texture_size(desktop, rotation);
    placement.visible = intersect(placement.rect, PixelRect{0, 0, texture.width, texture.height});

    // Every pixel goes through the same mapping as the position, so the image and its place can
    // never disagree about which way is up.
    CursorImage& turned = placement.image;
    turned.width = placement.rect.width();
    turned.height = placement.rect.height();
    turned.pixels.resize(image.pixels.size());
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const PixelPoint t = desktop_to_texture(PixelPoint{position.x + x, position.y + y}, desktop, rotation);
            const auto index = static_cast<std::size_t>(t.y - placement.rect.top) * static_cast<std::size_t>(turned.width) +
                               static_cast<std::size_t>(t.x - placement.rect.left);
            turned.pixels[index] =
                image.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)];
        }
    }
    return placement;
}

PixelRect cursor_rect(PixelSize image, PixelPoint position, PixelSize desktop, Rotation rotation) {
    if (image.width <= 0 || image.height <= 0) {
        return PixelRect{};
    }
    const PixelPoint a = desktop_to_texture(position, desktop, rotation);
    const PixelPoint b =
        desktop_to_texture(PixelPoint{position.x + image.width - 1, position.y + image.height - 1}, desktop, rotation);
    return PixelRect{std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x) + 1, std::max(a.y, b.y) + 1};
}

PixelSize texture_size(PixelSize desktop, Rotation rotation) {
    return sideways(rotation) ? PixelSize{desktop.height, desktop.width} : desktop;
}

std::uint32_t composite_pixel(std::uint32_t desktop, CursorPixel cursor) {
    const std::uint32_t alpha = channel(cursor.colour, 3);
    std::uint32_t out = desktop & kOpaque;
    for (int c = 0; c < 3; ++c) {
        const std::uint32_t blended =
            std::min<std::uint32_t>(255, channel(cursor.colour, c) + (channel(desktop, c) * (255 - alpha) + 127) / 255);
        out |= (blended ^ channel(cursor.xor_mask, c)) << (c * kBitsPerByte);
    }
    return out;
}

}  // namespace bt
