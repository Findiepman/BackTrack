#include "core/Fft.h"

#include <bit>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace bt {

Fft::Fft(std::size_t size) : size_(size) {
    if (size < 2 || !std::has_single_bit(size)) {
        throw std::invalid_argument("Fft size must be a power of two of at least 2");
    }
    const int bits = std::countr_zero(size);
    bit_reverse_.resize(size);
    for (std::size_t i = 0; i < size; ++i) {
        std::size_t reversed = 0;
        for (int b = 0; b < bits; ++b) {
            reversed |= ((i >> b) & 1u) << (bits - 1 - b);
        }
        bit_reverse_[i] = reversed;
    }
    cos_.resize(size / 2);
    sin_.resize(size / 2);
    for (std::size_t k = 0; k < size / 2; ++k) {
        // Computed in double: float twiddles accumulate visible error at 1024 points.
        const double angle = 2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(size);
        cos_[k] = static_cast<float>(std::cos(angle));
        sin_[k] = static_cast<float>(std::sin(angle));
    }
}

void Fft::transform(std::span<float> re, std::span<float> im, bool inverse) const {
    if (re.size() != size_ || im.size() != size_) {
        throw std::invalid_argument("Fft::transform: wrong buffer size");
    }
    for (std::size_t i = 0; i < size_; ++i) {
        const std::size_t j = bit_reverse_[i];
        if (j > i) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    const float sign = inverse ? 1.0f : -1.0f;
    for (std::size_t length = 2; length <= size_; length *= 2) {
        const std::size_t half = length / 2;
        const std::size_t stride = size_ / length;
        for (std::size_t start = 0; start < size_; start += length) {
            for (std::size_t k = 0; k < half; ++k) {
                const float wr = cos_[k * stride];
                const float wi = sign * sin_[k * stride];
                const std::size_t a = start + k;
                const std::size_t b = a + half;
                const float tr = re[b] * wr - im[b] * wi;
                const float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
    if (inverse) {
        const float scale = 1.0f / static_cast<float>(size_);
        for (std::size_t i = 0; i < size_; ++i) {
            re[i] *= scale;
            im[i] *= scale;
        }
    }
}

}  // namespace bt
