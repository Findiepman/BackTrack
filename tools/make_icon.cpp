// Build step: writes backtrack.ico (16 to 256 px) from the logo drawing in src/ui/Logo.cpp, so the
// exe's icon in Explorer, the Start menu and the taskbar is the same picture as the window's.
// Usage: make_icon <out.ico>

#include "platform/Com.h"
#include "ui/Icon.h"

#include <wincodec.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

namespace {

void put16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

// A classic icon image: BITMAPINFOHEADER (double height), bottom-up BGRA rows, an all-zero AND mask.
std::vector<std::uint8_t> bmp_entry(const std::vector<std::uint8_t>& pixels, int size) {
    std::vector<std::uint8_t> out;
    put32(out, 40);
    put32(out, static_cast<std::uint32_t>(size));
    put32(out, static_cast<std::uint32_t>(size * 2));
    put16(out, 1);
    put16(out, 32);
    for (int i = 0; i < 6; ++i) put32(out, 0);
    for (int y = size - 1; y >= 0; --y) {
        const auto row = pixels.begin() + static_cast<std::ptrdiff_t>(y) * size * 4;
        out.insert(out.end(), row, row + static_cast<std::ptrdiff_t>(size) * 4);
    }
    const std::size_t mask_row = static_cast<std::size_t>((size + 31) / 32 * 4);
    out.insert(out.end(), mask_row * static_cast<std::size_t>(size), 0);
    return out;
}

// The 256 px image as PNG (what Windows expects at that size).
std::vector<std::uint8_t> png_entry(const std::vector<std::uint8_t>& pixels, int size) {
    bt::ComPtr<IWICImagingFactory> wic;
    bt::ComPtr<IStream> stream;
    bt::ComPtr<IWICBitmapEncoder> encoder;
    bt::ComPtr<IWICBitmapFrameEncode> frame;
    BT_CHECK_HR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
    BT_CHECK_HR(CreateStreamOnHGlobal(nullptr, TRUE, &stream));
    BT_CHECK_HR(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    BT_CHECK_HR(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    BT_CHECK_HR(encoder->CreateNewFrame(&frame, nullptr));
    BT_CHECK_HR(frame->Initialize(nullptr));
    BT_CHECK_HR(frame->SetSize(static_cast<UINT>(size), static_cast<UINT>(size)));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    BT_CHECK_HR(frame->SetPixelFormat(&format));
    BT_CHECK_HR(frame->WritePixels(static_cast<UINT>(size), static_cast<UINT>(size) * 4,
                                   static_cast<UINT>(pixels.size()), const_cast<BYTE*>(pixels.data())));
    BT_CHECK_HR(frame->Commit());
    BT_CHECK_HR(encoder->Commit());
    STATSTG stat{};
    BT_CHECK_HR(stream->Stat(&stat, STATFLAG_NONAME));
    std::vector<std::uint8_t> out(static_cast<std::size_t>(stat.cbSize.QuadPart));
    const LARGE_INTEGER zero{};
    BT_CHECK_HR(stream->Seek(zero, STREAM_SEEK_SET, nullptr));
    ULONG read = 0;
    BT_CHECK_HR(stream->Read(out.data(), static_cast<ULONG>(out.size()), &read));
    out.resize(read);
    return out;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fputs("usage: make_icon <out.ico>\n", stderr);
        return 2;
    }
    try {
        bt::ComApartment com(COINIT_APARTMENTTHREADED);
        constexpr int kSizes[] = {16, 20, 24, 32, 40, 48, 64, 256};
        std::vector<std::vector<std::uint8_t>> images;
        for (const int size : kSizes) {
            const std::vector<std::uint8_t> pixels = bt::ui::render_logo_pixels(size);
            if (pixels.empty()) {
                std::fprintf(stderr, "make_icon: could not draw the %d px logo\n", size);
                return 1;
            }
            images.push_back(size >= 256 ? png_entry(pixels, size) : bmp_entry(pixels, size));
        }
        std::vector<std::uint8_t> file;
        put16(file, 0);
        put16(file, 1);  // icon
        put16(file, static_cast<std::uint16_t>(images.size()));
        std::uint32_t offset = 6 + 16 * static_cast<std::uint32_t>(images.size());
        for (std::size_t i = 0; i < images.size(); ++i) {
            const int size = kSizes[i];
            file.push_back(static_cast<std::uint8_t>(size >= 256 ? 0 : size));
            file.push_back(static_cast<std::uint8_t>(size >= 256 ? 0 : size));
            file.push_back(0);
            file.push_back(0);
            put16(file, 1);
            put16(file, 32);
            put32(file, static_cast<std::uint32_t>(images[i].size()));
            put32(file, offset);
            offset += static_cast<std::uint32_t>(images[i].size());
        }
        for (const auto& image : images) file.insert(file.end(), image.begin(), image.end());
        std::ofstream out(argv[1], std::ios::binary);
        out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
        if (!out) {
            std::fputs("make_icon: could not write the file\n", stderr);
            return 1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "make_icon: %s\n", e.what());
        return 1;
    }
    return 0;
}
