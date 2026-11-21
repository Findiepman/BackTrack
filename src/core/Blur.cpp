#include "core/Blur.h"

#include <algorithm>
#include <cmath>

namespace bt {
namespace {

// One box pass along a line of `count` pixels, `stride` bytes apart, from `src` into `dst`.
void box_line(const std::uint8_t* src, std::uint8_t* dst, int count, int stride, int radius) {
    const int window = 2 * radius + 1;
    for (int channel = 0; channel < 4; ++channel) {
        const auto at = [&](int i) {
            return static_cast<int>(src[static_cast<std::ptrdiff_t>(std::clamp(i, 0, count - 1)) * stride + channel]);
        };
        int sum = 0;
        for (int i = -radius; i <= radius; ++i) sum += at(i);
        for (int i = 0; i < count; ++i) {
            dst[static_cast<std::ptrdiff_t>(i) * stride + channel] = static_cast<std::uint8_t>((sum + window / 2) / window);
            sum += at(i + radius + 1) - at(i - radius);
        }
    }
}

}  // namespace

int box_radius_for(double sigma) {
    if (!(sigma > 0)) return 0;
    // Three passes of width w have variance 3 * (w * w - 1) / 12 = sigma^2.
    const double w = std::sqrt(4.0 * sigma * sigma + 1.0);
    return std::max(1, static_cast<int>(std::lround((w - 1.0) / 2.0)));
}

void box_blur_bgra(std::vector<std::uint8_t>& pixels, int width, int height, int radius) {
    if (radius <= 0 || width <= 0 || height <= 0) return;
    const std::size_t bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    if (pixels.size() < bytes) return;
    std::vector<std::uint8_t> scratch(bytes);
    const int row_bytes = width * 4;
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < height; ++y) {
            const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(row_bytes);
            box_line(pixels.data() + row, scratch.data() + row, width, 4, radius);
        }
        for (int x = 0; x < width; ++x) {
            const std::size_t column = static_cast<std::size_t>(x) * 4;
            box_line(scratch.data() + column, pixels.data() + column, height, row_bytes, radius);
        }
    }
}

}  // namespace bt
