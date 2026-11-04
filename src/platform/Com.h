#pragma once

#include "platform/Text.h"

#include <windows.h>
#include <wrl/client.h>

#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace bt {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// Codes whose system text is missing or wrong. FormatMessage maps the DXGI facility onto
// unrelated Win32 messages, e.g. DXGI_ERROR_ACCESS_LOST reads "The keyed mutex was abandoned".
inline const char* known_hresult_name(HRESULT hr) {
    switch (hr) {
        case DXGI_ERROR_ACCESS_LOST: return "DXGI_ERROR_ACCESS_LOST: the desktop switched (UAC, lock screen, mode change)";
        case DXGI_ERROR_WAIT_TIMEOUT: return "DXGI_ERROR_WAIT_TIMEOUT";
        case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED: the GPU was removed or its driver restarted";
        case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET";
        case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG";
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
        case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
        case DXGI_ERROR_UNSUPPORTED: return "DXGI_ERROR_UNSUPPORTED";
        case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE: return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE: too many programs are capturing the screen";
        case DXGI_ERROR_SESSION_DISCONNECTED: return "DXGI_ERROR_SESSION_DISCONNECTED";
        case DXGI_ERROR_NOT_FOUND: return "DXGI_ERROR_NOT_FOUND";
        case E_ACCESSDENIED: return "E_ACCESSDENIED: secure desktop (UAC prompt or lock screen) is showing";
        default: return nullptr;
    }
}

// "0x887A0026 (DXGI_ERROR_ACCESS_LOST: ...)"
inline std::string hresult_text(HRESULT hr) {
    if (const char* name = known_hresult_name(hr)) {
        return std::format("0x{:08X} ({})", static_cast<unsigned long>(hr), name);
    }
    constexpr DWORD kFlags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    wchar_t* buffer = nullptr;
    DWORD length = FormatMessageW(kFlags, nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<wchar_t*>(&buffer), 0,
                                  nullptr);
    if (length == 0) {
        // Media Foundation keeps its error texts in mfplat.dll, not in the system message table.
        if (const HMODULE mfplat = GetModuleHandleW(L"mfplat.dll")) {
            length = FormatMessageW(kFlags | FORMAT_MESSAGE_FROM_HMODULE, mfplat, static_cast<DWORD>(hr), 0,
                                    reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
        }
    }
    std::string text = length != 0 ? to_utf8(std::wstring_view(buffer, length)) : std::string("no description");
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return std::format("0x{:08X} ({})", static_cast<unsigned long>(hr), text);
}

class HresultError : public std::runtime_error {
public:
    HresultError(HRESULT hr, const std::string& message) : std::runtime_error(message), hr_(hr) {}
    HRESULT hr() const noexcept { return hr_; }

private:
    HRESULT hr_;
};

inline std::string_view file_name_only(std::string_view path) {
    const auto slash = path.find_last_of("\\/");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// §9: every HRESULT is checked. Setup code throws so the failure is logged with the exact call,
// location and system text, and the owning thread shuts the pipeline down cleanly.
inline void check_hr(HRESULT hr, const char* expression, const char* file, int line) {
    if (FAILED(hr)) {
        throw HresultError(hr, std::format("{} failed at {}:{}: {}", expression, file_name_only(file), line,
                                           hresult_text(hr)));
    }
}

inline void check_win32(BOOL ok, const char* expression, const char* file, int line) {
    if (!ok) {
        check_hr(HRESULT_FROM_WIN32(GetLastError()), expression, file, line);
    }
}

#define BT_CHECK_HR(expr) ::bt::check_hr((expr), #expr, __FILE__, __LINE__)
#define BT_CHECK_WIN32(expr) ::bt::check_win32((expr) ? TRUE : FALSE, #expr, __FILE__, __LINE__)

// Owns a kernel HANDLE. Treats both null and INVALID_HANDLE_VALUE as empty.
class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.handle_, nullptr));
        }
        return *this;
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

    void reset(HANDLE handle = nullptr) noexcept {
        if (*this) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

// COM must be initialised on every thread Backtrack creates (§6).
class ComApartment {
public:
    explicit ComApartment(DWORD model) { BT_CHECK_HR(CoInitializeEx(nullptr, model)); }
    ~ComApartment() { CoUninitialize(); }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
};

}  // namespace bt
