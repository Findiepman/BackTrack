#include "core/CaptureMath.h"

#include "Check.h"

#include <cstdint>
#include <vector>

using namespace bt;

namespace {

void scaled_sizes() {
    // Native, and never scaled up.
    CHECK(scaled_output_size({1920, 1080}, 0) == (PixelSize{1920, 1080}));
    CHECK(scaled_output_size({1920, 1080}, 1080) == (PixelSize{1920, 1080}));
    CHECK(scaled_output_size({1920, 1080}, 1440) == (PixelSize{1920, 1080}));
    CHECK(scaled_output_size({1280, 720}, 1080) == (PixelSize{1280, 720}));
    // Down to the short side, width from the shape.
    CHECK(scaled_output_size({2560, 1440}, 1080) == (PixelSize{1920, 1080}));
    CHECK(scaled_output_size({2560, 1440}, 720) == (PixelSize{1280, 720}));
    CHECK(scaled_output_size({3840, 2160}, 1440) == (PixelSize{2560, 1440}));
    CHECK(scaled_output_size({1920, 1080}, 720) == (PixelSize{1280, 720}));
    // Ultrawide and odd shapes: the long side is the nearest even number.
    CHECK(scaled_output_size({3440, 1440}, 1080) == (PixelSize{2580, 1080}));
    CHECK(scaled_output_size({3440, 1440}, 720) == (PixelSize{1720, 720}));
    CHECK(scaled_output_size({1366, 768}, 720) == (PixelSize{1280, 720}));  // 1280.6 -> 1280
    CHECK(scaled_output_size({2560, 1600}, 1080) == (PixelSize{1728, 1080}));
    CHECK(scaled_output_size({1680, 1050}, 720) == (PixelSize{1152, 720}));
    CHECK(scaled_output_size({5120, 1440}, 720) == (PixelSize{2560, 720}));
    // Portrait: the setting is the width.
    CHECK(scaled_output_size({1080, 1920}, 720) == (PixelSize{720, 1280}));
    CHECK(scaled_output_size({1440, 2560}, 1080) == (PixelSize{1080, 1920}));
    CHECK(scaled_output_size({1080, 1920}, 1080) == (PixelSize{1080, 1920}));
    // Every result is even.
    for (int w = 1000; w < 4000; w += 37) {
        for (int h = 700; h < 2200; h += 41) {
            const PixelSize natural{w & ~1, h & ~1};
            for (int r : {720, 1080, 1440}) {
                const PixelSize s = scaled_output_size(natural, r);
                CHECK(s.width % 2 == 0 && s.height % 2 == 0);
                CHECK(s.width <= natural.width && s.height <= natural.height);
            }
        }
    }
    // Nonsense in, the input back out.
    CHECK(scaled_output_size({0, 0}, 720) == (PixelSize{0, 0}));
}

void rotation_mapping() {
    const PixelSize desktop{1080, 1920};  // portrait desktop; the texture is 1920x1080
    // Clockwise90: the texture turned clockwise is the desktop, so the desktop's top-left corner is
    // the texture's bottom-left corner.
    CHECK(desktop_to_texture({0, 0}, desktop, Rotation::Clockwise90) == (PixelPoint{0, 1079}));
    CHECK(desktop_to_texture({1079, 0}, desktop, Rotation::Clockwise90) == (PixelPoint{0, 0}));
    CHECK(desktop_to_texture({1079, 1919}, desktop, Rotation::Clockwise90) == (PixelPoint{1919, 0}));
    // Clockwise270: the desktop's top-left corner is the texture's top-right corner.
    CHECK(desktop_to_texture({0, 0}, desktop, Rotation::Clockwise270) == (PixelPoint{1919, 0}));
    CHECK(desktop_to_texture({0, 1919}, desktop, Rotation::Clockwise270) == (PixelPoint{0, 0}));
    const PixelSize landscape{1920, 1080};
    CHECK(desktop_to_texture({0, 0}, landscape, Rotation::Clockwise180) == (PixelPoint{1919, 1079}));
    CHECK(desktop_to_texture({5, 7}, landscape, Rotation::None) == (PixelPoint{5, 7}));
}

// A 3x2 image whose pixels are numbered, to see where each one ends up.
CursorImage numbered() {
    CursorImage image;
    image.width = 3;
    image.height = 2;
    for (std::uint32_t i = 0; i < 6; ++i) {
        image.pixels.push_back(CursorPixel{i + 1, 0});
    }
    return image;
}

std::uint32_t at(const CursorImage& image, int x, int y) {
    return image.pixels[static_cast<std::size_t>(y * image.width + x)].colour;
}

void placement() {
    const PixelSize landscape{1920, 1080};
    CursorPlacement p = place_cursor(numbered(), {100, 50}, landscape, Rotation::None);
    CHECK(p.rect == (PixelRect{100, 50, 103, 52}));
    CHECK(p.visible == p.rect);
    CHECK_EQ(at(p.image, 0, 0), 1u);
    CHECK_EQ(at(p.image, 2, 1), 6u);

    // Partly off the left and top edges, and past the right/bottom edges.
    p = place_cursor(numbered(), {-1, -1}, landscape, Rotation::None);
    CHECK(p.rect == (PixelRect{-1, -1, 2, 1}));
    CHECK(p.visible == (PixelRect{0, 0, 2, 1}));
    p = place_cursor(numbered(), {1918, 1079}, landscape, Rotation::None);
    CHECK(p.visible == (PixelRect{1918, 1079, 1920, 1080}));
    // Entirely off the monitor.
    p = place_cursor(numbered(), {1920, 0}, landscape, Rotation::None);
    CHECK(p.visible.empty());
    p = place_cursor(numbered(), {-3, 0}, landscape, Rotation::None);
    CHECK(p.visible.empty());

    // Portrait monitor, Clockwise90: the 3x2 image becomes 2x3 in the texture, turned back
    // (counter-clockwise), so its top-right pixel (3) is the texture's top-left one.
    const PixelSize portrait{1080, 1920};
    p = place_cursor(numbered(), {0, 0}, portrait, Rotation::Clockwise90);
    CHECK(p.rect == (PixelRect{0, 1077, 2, 1080}));
    CHECK(p.visible == p.rect);
    CHECK_EQ(p.image.width, 2);
    CHECK_EQ(p.image.height, 3);
    CHECK_EQ(at(p.image, 0, 0), 3u);
    CHECK_EQ(at(p.image, 1, 0), 6u);
    CHECK_EQ(at(p.image, 0, 2), 1u);
    CHECK_EQ(at(p.image, 1, 2), 4u);
    // Clockwise270: turned clockwise, so the bottom-left pixel (4) is the top-left one.
    p = place_cursor(numbered(), {0, 0}, portrait, Rotation::Clockwise270);
    CHECK(p.rect == (PixelRect{1918, 0, 1920, 3}));
    CHECK_EQ(at(p.image, 0, 0), 4u);
    CHECK_EQ(at(p.image, 1, 0), 1u);
    CHECK_EQ(at(p.image, 0, 2), 6u);
    // Clockwise180: upside down.
    p = place_cursor(numbered(), {0, 0}, landscape, Rotation::Clockwise180);
    CHECK(p.rect == (PixelRect{1917, 1078, 1920, 1080}));
    CHECK_EQ(at(p.image, 0, 0), 6u);
    CHECK_EQ(at(p.image, 2, 1), 1u);
    // Off the rotated monitor's far edge.
    p = place_cursor(numbered(), {1079, 1919}, portrait, Rotation::Clockwise90);
    CHECK(p.visible == (PixelRect{1919, 0, 1920, 1}));

    CHECK(place_cursor(CursorImage{}, {0, 0}, landscape, Rotation::None).visible.empty());

    // The turned image does not depend on where the pointer is, and cursor_rect is place_cursor's
    // rectangle: the overlay turns once per shape and only moves the rectangle afterwards.
    for (Rotation r : {Rotation::None, Rotation::Clockwise90, Rotation::Clockwise180, Rotation::Clockwise270}) {
        const PixelSize desktop = (r == Rotation::Clockwise90 || r == Rotation::Clockwise270) ? portrait : landscape;
        CHECK(texture_size(desktop, r) == landscape);
        const CursorPlacement origin = place_cursor(numbered(), {0, 0}, desktop, r);
        for (PixelPoint at_point : {PixelPoint{37, 91}, PixelPoint{-2, 5}, PixelPoint{1000, 1070}}) {
            const CursorPlacement moved = place_cursor(numbered(), at_point, desktop, r);
            CHECK(moved.rect == cursor_rect({3, 2}, at_point, desktop, r));
            CHECK(moved.image.pixels == origin.image.pixels);
            // cursor_rect takes the size the right way up and gives the turned image's size.
            CHECK_EQ(moved.rect.width(), origin.image.width);
            CHECK_EQ(moved.rect.height(), origin.image.height);
        }
    }
    CHECK(cursor_rect({0, 2}, {0, 0}, landscape, Rotation::None).empty());
}

std::vector<std::uint8_t> bgra(std::initializer_list<std::uint32_t> pixels) {
    std::vector<std::uint8_t> bytes;
    for (std::uint32_t p : pixels) {
        for (int i = 0; i < 4; ++i) {
            bytes.push_back(static_cast<std::uint8_t>(p >> (8 * i)));
        }
    }
    return bytes;
}

void colour_shape() {
    // Opaque red, transparent, half-transparent white (premultiplied, as Windows hands it over),
    // and padding at the end of the row.
    std::vector<std::uint8_t> data = bgra({0xFFFF0000u, 0x00000000u, 0x80808080u, 0xDEADBEEFu});
    const CursorImage image = convert_cursor_shape(CursorShape{CursorShapeType::Color, 3, 1, 16, data});
    CHECK_EQ(image.width, 3);
    CHECK_EQ(image.height, 1);
    CHECK(image.pixels[0] == (CursorPixel{0xFFFF0000u, 0}));
    CHECK(image.pixels[1] == (CursorPixel{0, 0}));
    CHECK(image.pixels[2] == (CursorPixel{0x80808080u, 0}));

    const std::uint32_t grey = 0xFF404040u;
    CHECK_EQ(composite_pixel(grey, image.pixels[0]), 0xFFFF0000u);
    CHECK_EQ(composite_pixel(grey, image.pixels[1]), grey);
    // 0x80 + 0x40 * 127 / 255 = 128 + 32 = 0xA0
    CHECK_EQ(composite_pixel(grey, image.pixels[2]), 0xFFA0A0A0u);
    // The desktop's alpha byte stays what it was.
    CHECK_EQ(composite_pixel(0x00404040u, image.pixels[0]), 0x00FF0000u);
    // A real Windows 11 arrow outline pixel (214 at alpha 237) over black and over white.
    CHECK_EQ(composite_pixel(0xFF000000u, CursorPixel{0xEDD6D6D6u, 0}), 0xFFD6D6D6u);
    CHECK_EQ(composite_pixel(0xFFFFFFFFu, CursorPixel{0xEDD6D6D6u, 0}), 0xFFE8E8E8u);  // 214 + 18
    // A colour above its alpha (not premultiplied after all) saturates instead of wrapping.
    CHECK_EQ(composite_pixel(0xFF808080u, CursorPixel{0x40FFFFFFu, 0}), 0xFFFFFFFFu);
}

void masked_colour_shape() {
    // Alpha 0: replace with the colour. Alpha 0xFF: XOR the screen with it.
    std::vector<std::uint8_t> data = bgra({0x00000000u, 0x0012F0A0u, 0xFFFFFFFFu, 0xFF000000u});
    const CursorImage image = convert_cursor_shape(CursorShape{CursorShapeType::MaskedColor, 4, 1, 16, data});
    CHECK_EQ(image.width, 4);
    const std::uint32_t screen = 0xFF336699u;
    CHECK_EQ(composite_pixel(screen, image.pixels[0]), 0xFF000000u);  // black
    CHECK_EQ(composite_pixel(screen, image.pixels[1]), 0xFF12F0A0u);  // the colour, whatever was under it
    CHECK_EQ(composite_pixel(screen, image.pixels[2]), 0xFFCC9966u);  // inverted
    CHECK_EQ(composite_pixel(screen, image.pixels[3]), screen);  // XOR with 0: unchanged
}

void monochrome_shape() {
    // 4 pixels wide, 2 tall (DXGI says height 4: AND rows then XOR rows), pitch 2 with padding.
    // Row 0: AND 0011, XOR 0101 -> black, white, transparent, inverted.
    // Row 1: AND 1111, XOR 0000 -> all transparent.
    const std::vector<std::uint8_t> data = {
        0b0011'0000, 0xFF,  // AND row 0
        0b1111'0000, 0xFF,  // AND row 1
        0b0101'0000, 0xFF,  // XOR row 0
        0b0000'0000, 0xFF,  // XOR row 1
    };
    const CursorImage image = convert_cursor_shape(CursorShape{CursorShapeType::Monochrome, 4, 4, 2, data});
    CHECK_EQ(image.width, 4);
    CHECK_EQ(image.height, 2);
    const std::uint32_t screen = 0xFF102030u;
    CHECK_EQ(composite_pixel(screen, image.pixels[0]), 0xFF000000u);
    CHECK_EQ(composite_pixel(screen, image.pixels[1]), 0xFFFFFFFFu);
    CHECK_EQ(composite_pixel(screen, image.pixels[2]), screen);
    CHECK_EQ(composite_pixel(screen, image.pixels[3]), 0xFFEFDFCFu);
    for (int x = 0; x < 4; ++x) {
        CHECK_EQ(composite_pixel(screen, image.pixels[static_cast<std::size_t>(4 + x)]), screen);
    }

    // Wider than one byte: pixel 9 is bit 6 of the second byte.
    std::vector<std::uint8_t> wide(2 * 2, 0xFF);  // 10 wide, 1 tall: AND row, XOR row
    wide[2] = 0x00;
    wide[3] = 0b0100'0000;  // XOR: pixel 9 set
    const CursorImage w = convert_cursor_shape(CursorShape{CursorShapeType::Monochrome, 10, 2, 2, wide});
    CHECK_EQ(w.width, 10);
    CHECK_EQ(w.height, 1);
    CHECK(w.pixels[9] == (CursorPixel{0, 0x00FFFFFFu}));  // AND 1 XOR 1: inverts
    CHECK(w.pixels[8] == (CursorPixel{0, 0}));
}

void invalid_shapes() {
    const std::vector<std::uint8_t> data(64, 0);
    // Pitch too small for the width, data too short, zero sizes, odd monochrome height of 1.
    CHECK(convert_cursor_shape(CursorShape{CursorShapeType::Color, 4, 1, 8, data}).empty());
    CHECK(convert_cursor_shape(CursorShape{CursorShapeType::Color, 4, 5, 16, data}).empty());
    CHECK(!convert_cursor_shape(CursorShape{CursorShapeType::Color, 4, 4, 16, data}).empty());
    CHECK(convert_cursor_shape(CursorShape{CursorShapeType::Color, 0, 4, 16, data}).empty());
    CHECK(convert_cursor_shape(CursorShape{CursorShapeType::Color, 4, 0, 16, data}).empty());
    CHECK(convert_cursor_shape(CursorShape{CursorShapeType::Color, 4, 4, 0, data}).empty());
    CHECK(convert_cursor_shape(CursorShape{CursorShapeType::Monochrome, 8, 1, 1, data}).empty());
    CHECK(convert_cursor_shape(CursorShape{CursorShapeType::Monochrome, 9, 2, 1, data}).empty());
    CHECK(!convert_cursor_shape(CursorShape{CursorShapeType::Monochrome, 9, 2, 2, data}).empty());
    // The last row does not need its padding: exactly pitch * (rows - 1) + one row of pixels.
    const std::vector<std::uint8_t> tight(16 + 8, 0);
    CHECK(!convert_cursor_shape(CursorShape{CursorShapeType::Color, 2, 2, 16, tight}).empty());
    CHECK(convert_cursor_shape(CursorShape{CursorShapeType::Color, 2, 2, 16, std::span(tight).first(23)}).empty());
}

}  // namespace

int main() {
    scaled_sizes();
    rotation_mapping();
    placement();
    colour_shape();
    masked_colour_shape();
    monochrome_shape();
    invalid_shapes();
    return bt_test::finish("CaptureMathTests");
}
