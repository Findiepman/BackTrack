#include "platform/IniFile.h"

#include "platform/Com.h"
#include "platform/Log.h"
#include "platform/Text.h"

#include <fstream>
#include <sstream>

namespace bt {
namespace {

// Notepad saves as UTF-8, "Unicode" (UTF-16 LE), "Unicode big endian" or ANSI. The parser wants
// UTF-8, so the others are converted here instead of failing on every line.
std::string ini_text_as_utf8(const std::string& bytes) {
    const bool utf16le = bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
                         static_cast<unsigned char>(bytes[1]) == 0xFE;
    const bool utf16be = bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE &&
                         static_cast<unsigned char>(bytes[1]) == 0xFF;
    if (utf16le || utf16be) {
        std::wstring wide;
        for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
            const auto lo = static_cast<unsigned char>(bytes[utf16le ? i : i + 1]);
            const auto hi = static_cast<unsigned char>(bytes[utf16le ? i + 1 : i]);
            wide.push_back(static_cast<wchar_t>(lo | (hi << 8)));
        }
        log::info("backtrack.ini is saved as UTF-16, reading it as such");
        return to_utf8(wide);
    }
    const int valid_utf8 = bytes.empty() ? 1
        : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (valid_utf8 == 0) {
        // Not UTF-8: an "ANSI" save with an accented path. Read it in the system code page.
        const int length = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(), length);
        log::info("backtrack.ini is not UTF-8, reading it in the system code page");
        return to_utf8(wide);
    }
    return bytes;
}

}  // namespace

std::optional<std::string> read_ini_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::stringstream text;
    text << file.rdbuf();
    if (file.bad()) {
        return std::nullopt;
    }
    return ini_text_as_utf8(text.str());
}

bool write_ini_file(const std::filesystem::path& path, const std::string& utf8_text) {
    std::filesystem::path temp = path;
    temp += L".tmp";
    {
        const UniqueHandle out(
            CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        DWORD written = 0;
        const bool ok = out && WriteFile(out.get(), utf8_text.data(), static_cast<DWORD>(utf8_text.size()), &written, nullptr) &&
                        written == utf8_text.size() && FlushFileBuffers(out.get());
        if (!ok) {
            log::warn("Could not write {}: {}", to_utf8(temp.wstring()), hresult_text(HRESULT_FROM_WIN32(GetLastError())));
            std::error_code ec;
            std::filesystem::remove(temp, ec);
            return false;
        }
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        log::warn("Could not save the settings to {}: {}", to_utf8(path.wstring()),
                  hresult_text(HRESULT_FROM_WIN32(GetLastError())));
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

}  // namespace bt
