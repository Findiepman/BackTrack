#include "core/Blur.h"

#include "Check.h"

#include <cstdint>
#include <vector>

using namespace bt;

namespace {

std::vector<std::uint8_t> solid(int w, int h, std::uint8_t b, std::uint8_t g, std::uint8_t r) {
    std::vector<std::uint8_t> p(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < p.size(); i += 4) {
        p[i] = b;
        p[i + 1] = g;
        p[i + 2] = r;
        p[i + 3] = 255;
    }
    return p;
}

void a_solid_picture_stays_the_same() {
    // Edges repeat the outermost pixel: no dark rim around a blurred picture.
    std::vector<std::uint8_t> p = solid(7, 5, 10, 200, 90);
    const std::vector<std::uint8_t> before = p;
    box_blur_bgra(p, 7, 5, 3);
    CHECK(p == before);
}

void radius_zero_and_bad_sizes_do_nothing() {
    std::vector<std::uint8_t> p = solid(4, 4, 1, 2, 3);
    p[0] = 255;
    const std::vector<std::uint8_t> before = p;
    box_blur_bgra(p, 4, 4, 0);
    CHECK(p == before);
    box_blur_bgra(p, 0, 4, 3);
    box_blur_bgra(p, 4, -1, 3);
    box_blur_bgra(p, 100, 100, 3);  // the buffer is smaller than that: untouched, no overrun
    CHECK(p == before);
    std::vector<std::uint8_t> empty;
    box_blur_bgra(empty, 1, 1, 2);
    CHECK(empty.empty());
}

void a_bright_dot_spreads_and_keeps_its_energy() {
    const int w = 21;
    const int h = 21;
    std::vector<std::uint8_t> p = solid(w, h, 0, 0, 0);
    const std::size_t centre = (static_cast<std::size_t>(10) * w + 10) * 4;
    p[centre] = 255;
    box_blur_bgra(p, w, h, 2);
    CHECK(p[centre] < 255);  // the dot got softer
    CHECK(p[centre] > 0);
    CHECK(p[centre + 4] > 0);  // and its neighbours lighter
    CHECK(p[(static_cast<std::size_t>(10) * w + 13) * 4] > 0);
    CHECK(p[0] == 0);  // far away stays dark
    CHECK(p[centre + 3] == 255);  // alpha of an opaque picture stays opaque
    // Symmetric: left and right of the dot match.
    CHECK(p[centre + 4] == p[centre - 4]);
}

void a_huge_radius_on_a_tiny_picture_is_safe() {
    std::vector<std::uint8_t> p = solid(2, 1, 0, 0, 0);
    p[0] = 200;
    box_blur_bgra(p, 2, 1, 50);
    CHECK(p[0] > 0 && p[0] < 200);
    CHECK(p[4] > 0);
}

void box_radius_follows_sigma() {
    CHECK(box_radius_for(0) == 0);
    CHECK(box_radius_for(-3) == 0);
    CHECK(box_radius_for(0.1) == 1);
    CHECK(box_radius_for(4) >= 3 && box_radius_for(4) <= 5);
    CHECK(box_radius_for(24) > box_radius_for(4));
}

}  // namespace

int main() {
    a_solid_picture_stays_the_same();
    radius_zero_and_bad_sizes_do_nothing();
    a_bright_dot_spreads_and_keeps_its_energy();
    a_huge_radius_on_a_tiny_picture_is_safe();
    box_radius_follows_sigma();
    return bt_test::finish("BlurTests");
}
