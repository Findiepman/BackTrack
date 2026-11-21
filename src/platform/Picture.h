#pragma once

// Reads a picture file (JPEG, PNG, BMP, GIF, TIFF and whatever else Windows has a decoder for:
// WebP and HEIC when their extensions are installed) into BGRA pixels with the Windows Imaging
// Component. Needs COM on the calling thread; meant for the background worker.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace bt {

struct Picture {
    std::vector<std::uint8_t> bgra;  // opaque: transparent parts are put on black
    int width = 0;
    int height = 0;
};

// Scaled down (never up) so the longer side is at most `max_side`. Empty with `error` set when
// the file cannot be read or is not a picture Windows can decode.
std::optional<Picture> load_picture(const std::filesystem::path& file, int max_side, std::string& error);

}  // namespace bt
