// Development tool for the trim and filmstrip code. Not shipped.
//
//   media_check trim <source.mp4> <destination.mp4> <in seconds> <out seconds>
//   media_check filmstrip <video> <count> <width> <height> <out.bmp>
//
// trim runs trim_clip exactly as the app does; check the result with ffprobe / ffmpeg.
// filmstrip runs extract_frames and lays the pictures side by side in one BMP to look at.

#include "platform/Com.h"
#include "platform/Filmstrip.h"
#include "platform/Log.h"
#include "platform/Trim.h"

#include <mfapi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {

bool write_bmp(const std::filesystem::path& path, const std::vector<std::vector<std::uint8_t>>& frames, int width,
               int height) {
    const int total_width = width * static_cast<int>(frames.size());
    const std::uint32_t pixel_bytes = static_cast<std::uint32_t>(total_width) * static_cast<std::uint32_t>(height) * 4;
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    file.bfType = 0x4D42;  // "BM"
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + pixel_bytes;
    info.biSize = sizeof(info);
    info.biWidth = total_width;
    info.biHeight = -height;  // top-down rows
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = pixel_bytes;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    out.write(reinterpret_cast<const char*>(&info), sizeof(info));
    for (int y = 0; y < height; ++y) {
        for (const auto& frame : frames) {
            out.write(reinterpret_cast<const char*>(frame.data() + static_cast<std::size_t>(y) * width * 4),
                      static_cast<std::streamsize>(width) * 4);
        }
    }
    return static_cast<bool>(out);
}

int usage() {
    std::puts("usage: media_check trim <source> <destination> <in s> <out s>\n"
              "       media_check filmstrip <video> <count> <width> <height> <out.bmp>");
    return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return usage();
    bt::log::set_console_echo(true);
    bt::log::set_level(bt::LogLevel::Debug);
    bt::ComApartment com(COINIT_MULTITHREADED);
    BT_CHECK_HR(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    const std::wstring command = argv[1];
    int code = 0;
    const auto started = std::chrono::steady_clock::now();
    if (command == L"trim" && argc == 6) {
        const bt::TrimResult r = bt::trim_clip(bt::TrimRequest{argv[2], argv[3], _wtof(argv[4]), _wtof(argv[5])});
        if (r.ok) {
            std::printf("ok: start %.6f s, %.6f s long, %llu bytes\n", r.start_s, r.seconds,
                        static_cast<unsigned long long>(r.bytes));
        } else {
            std::printf("failed: %s\n", r.error.c_str());
            code = 1;
        }
    } else if (command == L"filmstrip" && argc == 7) {
        const int count = _wtoi(argv[3]);
        const int width = _wtoi(argv[4]);
        const int height = _wtoi(argv[5]);
        const auto frames = bt::extract_frames(argv[2], count, width, height);
        if (frames.empty()) {
            std::puts("failed: no frames");
            code = 1;
        } else if (!write_bmp(argv[6], frames, width, height)) {
            std::puts("failed: cannot write the BMP");
            code = 1;
        } else {
            std::printf("ok: %zu frames\n", frames.size());
        }
    } else {
        code = usage();
    }
    std::printf("took %lld ms\n", static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                              std::chrono::steady_clock::now() - started)
                                                              .count()));
    MFShutdown();
    return code;
}
