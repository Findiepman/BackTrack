#pragma once

// Development check for M1: decodes an NV12 frame the way a video player would and compares it
// with the BGRA desktop image it was made from. Tries BT.709/BT.601 and limited/full range, so a
// wrong colour space shows up as "best match: BT.601 full" instead of just "looks a bit off".
// Pure C++, no Windows headers.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace bt::tools {

struct YuvInterpretation {
    const char* name;
    double kr;
    double kb;
    bool limited_range;
};

inline constexpr std::array<YuvInterpretation, 4> kInterpretations = {{
    {"BT.709 limited", 0.2126, 0.0722, true},
    {"BT.709 full", 0.2126, 0.0722, false},
    {"BT.601 limited", 0.299, 0.114, true},
    {"BT.601 full", 0.299, 0.114, false},
}};

struct Fit {
    const YuvInterpretation* interpretation = nullptr;
    std::array<double, 3> mean_abs_error{};  // R, G, B in 0..255 units
    double overall = 0.0;
};

struct ComparisonReport {
    std::vector<Fit> fits;  // best first
    int luma_min = 255;
    int luma_max = 0;
};

inline std::array<std::uint8_t, 3> yuv_to_rgb(int y, int u, int v, const YuvInterpretation& in) {
    const double yn = in.limited_range ? (y - 16) / 219.0 : y / 255.0;
    const double cb = in.limited_range ? (u - 128) / 224.0 : (u - 128) / 255.0;
    const double cr = in.limited_range ? (v - 128) / 224.0 : (v - 128) / 255.0;
    const double kg = 1.0 - in.kr - in.kb;
    const double r = yn + 2.0 * (1.0 - in.kr) * cr;
    const double b = yn + 2.0 * (1.0 - in.kb) * cb;
    const double g = (yn - in.kr * r - in.kb * b) / kg;
    auto to_byte = [](double c) { return static_cast<std::uint8_t>(std::clamp(std::lround(c * 255.0), 0L, 255L)); };
    return {to_byte(r), to_byte(g), to_byte(b)};
}

// NV12 needs even dimensions and exactly width * height * 3 / 2 bytes.
inline bool nv12_size_ok(const std::vector<std::uint8_t>& nv12, int width, int height) {
    return width > 0 && height > 0 && width % 2 == 0 && height % 2 == 0 &&
           nv12.size() == static_cast<std::size_t>(width) * height * 3 / 2;
}

// nv12: width * height * 3 / 2 bytes. bgra: width * height * 4 bytes, both tightly packed.
inline ComparisonReport compare_nv12_with_bgra(const std::vector<std::uint8_t>& nv12,
                                               const std::vector<std::uint8_t>& bgra, int width, int height) {
    ComparisonReport report;
    if (!nv12_size_ok(nv12, width, height) || bgra.size() != static_cast<std::size_t>(width) * height * 4) {
        return report;  // no fits: caller reports the mismatch
    }
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    for (std::size_t i = 0; i < w * h; ++i) {
        report.luma_min = std::min<int>(report.luma_min, nv12[i]);
        report.luma_max = std::max<int>(report.luma_max, nv12[i]);
    }
    for (const YuvInterpretation& in : kInterpretations) {
        std::array<double, 3> sum{};
        for (std::size_t y = 0; y < h; ++y) {
            const std::uint8_t* uv_row = nv12.data() + w * h + (y / 2) * w;
            for (std::size_t x = 0; x < w; ++x) {
                const std::size_t uv = (x / 2) * 2;
                const auto rgb = yuv_to_rgb(nv12[y * w + x], uv_row[uv], uv_row[uv + 1], in);
                const std::uint8_t* ref = bgra.data() + (y * w + x) * 4;
                sum[0] += std::abs(rgb[0] - ref[2]);
                sum[1] += std::abs(rgb[1] - ref[1]);
                sum[2] += std::abs(rgb[2] - ref[0]);
            }
        }
        Fit fit;
        fit.interpretation = &in;
        for (int c = 0; c < 3; ++c) {
            fit.mean_abs_error[static_cast<std::size_t>(c)] = sum[static_cast<std::size_t>(c)] / static_cast<double>(w * h);
        }
        fit.overall = (fit.mean_abs_error[0] + fit.mean_abs_error[1] + fit.mean_abs_error[2]) / 3.0;
        report.fits.push_back(fit);
    }
    std::sort(report.fits.begin(), report.fits.end(), [](const Fit& a, const Fit& b) { return a.overall < b.overall; });
    return report;
}

// 24-bit bottom-up BMP: opens in every image viewer. bgr is width * height * 3, top row first.
inline bool write_bmp24(const std::filesystem::path& path, const std::vector<std::uint8_t>& bgr, int width, int height) {
    if (width <= 0 || height <= 0 || bgr.size() != static_cast<std::size_t>(width) * height * 3) {
        return false;
    }
    const std::size_t row = static_cast<std::size_t>(width) * 3;
    const std::size_t padded = (row + 3) & ~std::size_t{3};
    const std::size_t image_bytes = padded * static_cast<std::size_t>(height);
    const std::uint32_t file_size = static_cast<std::uint32_t>(54 + image_bytes);
    std::uint8_t header[54] = {'B', 'M'};
    auto put32 = [&](int offset, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) header[offset + i] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    put32(2, file_size);
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<std::uint32_t>(width));
    put32(22, static_cast<std::uint32_t>(height));
    header[26] = 1;   // planes
    header[28] = 24;  // bits per pixel
    put32(34, static_cast<std::uint32_t>(image_bytes));

    std::ofstream file(path, std::ios::binary);  // wide path: works for any folder name
    file.write(reinterpret_cast<const char*>(header), sizeof(header));
    const std::vector<char> padding(padded - row, 0);
    for (int y = height - 1; file && y >= 0; --y) {
        file.write(reinterpret_cast<const char*>(bgr.data() + static_cast<std::size_t>(y) * row),
                   static_cast<std::streamsize>(row));
        file.write(padding.data(), static_cast<std::streamsize>(padding.size()));
    }
    file.close();
    return !file.fail();
}

inline std::vector<std::uint8_t> bgra_to_bgr(const std::vector<std::uint8_t>& bgra) {
    std::vector<std::uint8_t> bgr(bgra.size() / 4 * 3);
    for (std::size_t i = 0, j = 0; i + 3 < bgra.size(); i += 4, j += 3) {
        bgr[j] = bgra[i];
        bgr[j + 1] = bgra[i + 1];
        bgr[j + 2] = bgra[i + 2];
    }
    return bgr;
}

inline std::vector<std::uint8_t> nv12_to_bgr(const std::vector<std::uint8_t>& nv12, int width, int height) {
    if (!nv12_size_ok(nv12, width, height)) {
        return {};
    }
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    std::vector<std::uint8_t> bgr(w * h * 3);
    for (std::size_t y = 0; y < h; ++y) {
        const std::uint8_t* uv_row = nv12.data() + w * h + (y / 2) * w;
        for (std::size_t x = 0; x < w; ++x) {
            const std::size_t uv = (x / 2) * 2;
            const auto rgb = yuv_to_rgb(nv12[y * w + x], uv_row[uv], uv_row[uv + 1], kInterpretations[0]);
            std::uint8_t* out = bgr.data() + (y * w + x) * 3;
            out[0] = rgb[2];
            out[1] = rgb[1];
            out[2] = rgb[0];
        }
    }
    return bgr;
}

}  // namespace bt::tools
