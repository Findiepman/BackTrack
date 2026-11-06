#include "platform/Uploader.h"

#include "platform/Log.h"
#include "platform/Text.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <format>
#include <vector>

namespace bt {
namespace {

// A piece small enough that a cancel is noticed quickly on a slow upload (64 KB at 1 Mbit/s is
// half a second), big enough not to cost speed.
constexpr DWORD kPiece = 64 * 1024;
// Uploading 200 MB on a slow line takes long; a send only fails when nothing moves for this long.
constexpr int kResolveTimeoutMs = 15'000;
constexpr int kConnectTimeoutMs = 15'000;
constexpr int kSendTimeoutMs = 60'000;
// catbox answers once it has stored the file, which can take a while for a big one.
constexpr int kReceiveTimeoutMs = 180'000;
constexpr DWORD kMaxReplyBytes = 4096;

class Internet {
public:
    explicit Internet(HINTERNET handle = nullptr) : handle_(handle) {}
    ~Internet() {
        if (handle_ != nullptr) WinHttpCloseHandle(handle_);
    }
    Internet(const Internet&) = delete;
    Internet& operator=(const Internet&) = delete;
    HINTERNET get() const { return handle_; }

private:
    HINTERNET handle_;
};

std::string connection_problem(DWORD error, std::wstring_view host) {
    const std::string name = to_utf8(std::wstring(host));
    switch (error) {
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        case ERROR_WINHTTP_CANNOT_CONNECT:
        case ERROR_WINHTTP_CONNECTION_ERROR:
        case ERROR_WINHTTP_TIMEOUT:
            return std::format("Could not reach {}. Check the internet connection and try again.", name);
        case ERROR_WINHTTP_SECURE_FAILURE:
        case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
        case ERROR_WINHTTP_SECURE_INVALID_CA:
        case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
        case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
        case ERROR_WINHTTP_SECURE_INVALID_CERT:
        case ERROR_WINHTTP_SECURE_CERT_REVOKED:
            return std::format("The secure connection to {} could not be trusted (a proxy or security program may be "
                               "in the way). Nothing was uploaded.",
                               name);
        default: return std::format("The upload to {} failed (error {}). Try again in a moment.", name, error);
    }
}

std::string cancelled_text() {
    return "The upload was cancelled.";
}

std::string random_boundary() {
    unsigned char bytes[16] = {};
    BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::string out = "----BacktrackBoundary";
    for (const unsigned char b : bytes) out += std::format("{:02x}", b);
    return out;
}

}  // namespace

void UploadAbort::abort() {
    std::lock_guard lock(mutex_);
    aborted_ = true;
    if (request_ != nullptr) {
        WinHttpCloseHandle(static_cast<HINTERNET>(request_));  // the blocked call on the worker returns now
        request_ = nullptr;
    }
}

bool UploadAbort::aborted() const {
    std::lock_guard lock(mutex_);
    return aborted_;
}

// Owns the request handle together with UploadAbort: whichever comes first closes it, once.
class UploadRequestGuard {
public:
    UploadRequestGuard(UploadAbort& abort, HINTERNET request) : abort_(abort) {
        std::lock_guard lock(abort_.mutex_);
        if (abort_.aborted_) {
            WinHttpCloseHandle(request);
        } else {
            abort_.request_ = request;
        }
    }
    ~UploadRequestGuard() {
        std::lock_guard lock(abort_.mutex_);
        if (abort_.request_ != nullptr) WinHttpCloseHandle(static_cast<HINTERNET>(abort_.request_));
        abort_.request_ = nullptr;
    }
    UploadRequestGuard(const UploadRequestGuard&) = delete;
    UploadRequestGuard& operator=(const UploadRequestGuard&) = delete;
    // The handle for the next call, or null once abort() closed it: a call on null fails at once,
    // where a call on the closed value could reach another handle that WinHTTP gave the same value
    // since (the updater's). Only the moment between this and the call itself stays open.
    HINTERNET get() const {
        std::lock_guard lock(abort_.mutex_);
        return static_cast<HINTERNET>(abort_.request_);
    }

private:
    UploadAbort& abort_;
};

UploadResult upload_clip(const std::filesystem::path& file, ShareHost host, const UploadProgress& progress,
                         UploadAbort& abort) {
    UploadResult result;
    const ShareEndpoint endpoint = share_endpoint(host);
    try {
        const HANDLE opened = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                          FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (opened == INVALID_HANDLE_VALUE) {
            result.message = "The share copy could not be opened for the upload.";
            return result;
        }
        struct Closer {
            HANDLE h;
            ~Closer() { CloseHandle(h); }
        } closer{opened};
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(opened, &size)) {
            result.message = "The share copy could not be read for the upload.";
            return result;
        }
        const auto file_bytes = static_cast<std::uint64_t>(size.QuadPart);
        if (file_bytes == 0 || file_bytes > endpoint.max_bytes) {
            result.message = std::format("The clip is {:.0f} MB; {} takes at most {} MB.", file_bytes / 1e6,
                                         to_utf8(std::wstring(endpoint.host)), endpoint.max_bytes / 1'000'000);
            return result;
        }
        const UploadForm form = make_upload_form(host, to_utf8(file.filename().wstring()), random_boundary());
        const std::uint64_t total = form.head.size() + file_bytes + form.tail.size();

        const Internet session(WinHttpOpen(L"Backtrack", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                           WINHTTP_NO_PROXY_BYPASS, 0));
        if (session.get() == nullptr) {
            result.message = connection_problem(GetLastError(), endpoint.host);
            return result;
        }
        WinHttpSetTimeouts(session.get(), kResolveTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs, kReceiveTimeoutMs);
        const Internet connect(WinHttpConnect(session.get(), std::wstring(endpoint.host).c_str(),
                                              INTERNET_DEFAULT_HTTPS_PORT, 0));
        const HINTERNET opened_request =
            connect.get() == nullptr ? nullptr
                                     : WinHttpOpenRequest(connect.get(), L"POST", std::wstring(endpoint.path).c_str(),
                                                          nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                          WINHTTP_FLAG_SECURE);
        if (opened_request == nullptr) {
            result.message = connection_problem(GetLastError(), endpoint.host);
            return result;
        }
        const UploadRequestGuard request(abort, opened_request);
        // Every failure below may be the abort closing the handle under a blocked call.
        const auto failed = [&](DWORD error) {
            result.cancelled = abort.aborted();
            result.message = result.cancelled ? cancelled_text() : connection_problem(error, endpoint.host);
            return result;
        };
        if (request.get() == nullptr) return failed(ERROR_WINHTTP_OPERATION_CANCELLED);
        // The reply must come from the host itself: a redirect could hand back any address.
        DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));
        DWORD cookies = WINHTTP_DISABLE_COOKIES;
        WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &cookies, sizeof(cookies));
        const std::wstring headers = L"Content-Type: " + to_wide(form.content_type);
        // Over 4 GB cannot be announced here, but no host accepts that much anyway (checked above).
        if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0,
                                static_cast<DWORD>(total), 0)) {
            return failed(GetLastError());
        }
        std::uint64_t sent = 0;
        const auto write = [&](const void* data, DWORD bytes) {
            DWORD written = 0;
            if (!WinHttpWriteData(request.get(), data, bytes, &written) || written != bytes) return false;
            sent += bytes;
            if (progress) progress(static_cast<double>(sent) / static_cast<double>(total));
            return true;
        };
        bool ok = write(form.head.data(), static_cast<DWORD>(form.head.size()));
        std::vector<char> piece(kPiece);
        for (std::uint64_t left = file_bytes; ok && left > 0;) {
            if (abort.aborted()) return failed(ERROR_WINHTTP_OPERATION_CANCELLED);  // a half-sent upload is not kept
            DWORD read = 0;
            if (!ReadFile(opened, piece.data(), static_cast<DWORD>(std::min<std::uint64_t>(left, kPiece)), &read,
                          nullptr) ||
                read == 0) {
                result.message = "The share copy could not be read during the upload.";
                return result;
            }
            ok = write(piece.data(), read);
            left -= read;
        }
        if (ok) ok = write(form.tail.data(), static_cast<DWORD>(form.tail.size()));
        if (!ok || !WinHttpReceiveResponse(request.get(), nullptr)) return failed(GetLastError());
        DWORD status = 0;
        DWORD status_size = sizeof(status);
        if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX)) {
            return failed(GetLastError());
        }
        std::string body;
        char buffer[1024];
        DWORD got = 0;
        while (body.size() < kMaxReplyBytes) {
            if (!WinHttpReadData(request.get(), buffer, sizeof(buffer), &got)) return failed(GetLastError());
            if (got == 0) break;
            body.append(buffer, got);
        }
        // Cancelled while the reply came in: whatever arrived is not trusted as an answer.
        if (abort.aborted()) return failed(ERROR_WINHTTP_OPERATION_CANCELLED);
        if (status == 407) {
            result.message = "A proxy on this network wants a password before anything can be uploaded.";
            return result;
        }
        if (status == 413 || status == 412) {
            result.message = std::format("{} says the clip is too big.", to_utf8(std::wstring(endpoint.host)));
            return result;
        }
        if (status != 200) {
            result.message = std::format("{} refused the upload (error {}). Try again later.",
                                         to_utf8(std::wstring(endpoint.host)), status);
            log::warn("Upload refused: HTTP {}", status);
            return result;
        }
        if (const auto url = parse_upload_reply(host, body)) {
            result.ok = true;
            result.url = *url;
            return result;
        }
        // Their message in the log (printable characters only), a plain line for the user.
        std::string shown;
        for (const char c : body.substr(0, 160)) shown += (c >= 0x20 && c < 0x7f) ? c : ' ';
        log::warn("Upload answer without a link: {}", shown);
        result.message = std::format("{} did not send back a link. Try again later.", to_utf8(std::wstring(endpoint.host)));
    } catch (const std::exception& e) {
        result.message = std::string("The upload failed: ") + e.what();
    }
    return result;
}

}  // namespace bt
