#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace bt {

// In-place complex radix-2 FFT of one fixed size (a power of two). Tables are made once in the
// constructor; transform() allocates nothing.
class Fft {
public:
    explicit Fft(std::size_t size);

    std::size_t size() const { return size_; }

    // Forward: X[k] = sum x[n] e^(-2 pi i k n / N). Inverse: the same with +i, divided by N, so
    // inverse(forward(x)) == x.
    void transform(std::span<float> re, std::span<float> im, bool inverse) const;

private:
    std::size_t size_;
    std::vector<std::size_t> bit_reverse_;
    std::vector<float> cos_;  // cos(2 pi k / N), k < N/2
    std::vector<float> sin_;
};

}  // namespace bt
