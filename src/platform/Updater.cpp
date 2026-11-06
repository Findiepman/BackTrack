#include "platform/Updater.h"

#include "platform/Com.h"
#include "platform/Log.h"
#include "platform/Text.h"

#include <bcrypt.h>
#include <shlobj.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace bt {
namespace {

namespace fs = std::filesystem;

// Inno Setup's uninstall entry for our AppId (installer/backtrack.iss); per-user, so under HKCU.
constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{6E3B1C2A-8F4D-4C55-9A7E-2B5D9C1F0A71}_is1";
constexpr wchar_t kStateFile[] = L"state.json";
constexpr wchar_t kInstallLog[] = L"install.log";
// Long enough for a slow connection. The receive timeout is also the longest a quit waits for a
// download whose connection went silent (the worker cannot be interrupted inside a WinHTTP call).
constexpr int kResolveTimeoutMs = 10000;
constexpr int kConnectTimeoutMs = 10000;
constexpr int kSendTimeoutMs = 10000;
constexpr int kReceiveTimeoutMs = 10000;
constexpr int kMaxRedirects = 5;
constexpr std::size_t kMaxApiBody = 4 * 1024 * 1024;  // a release with many assets is ~100 KB
constexpr std::int64_t kMaxInstallerSize = 512LL * 1024 * 1024;  // ours is a few MB; this only stops nonsense
constexpr std::size_t kChunk = 64 * 1024;
constexpr auto kProgressInterval = std::chrono::milliseconds(250);
// Wakes up at least this often to look at the wall clock again: steady time does not count a
// PC that slept, so a "due in 23 h" wait would otherwise overshoot by the night.
constexpr auto kMaxAutoWait = std::chrono::hours(1);
constexpr int kHttpOk = 200;

std::string path_text(const fs::path& path) {
    return to_utf8(path.wstring());
}

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// What went wrong: `plain` for the window, what() for the log.
class UpdateError : public std::runtime_error {
public:
    UpdateError(std::string plain, const std::string& detail) : std::runtime_error(detail), plain_(std::move(plain)) {}
    const std::string& plain() const { return plain_; }

private:
    std::string plain_;
};

// The worker was asked to stop (quit, or an install_* call).
struct Cancelled {};

std::string winhttp_plain(DWORD error) {
    switch (error) {
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        case ERROR_WINHTTP_CANNOT_CONNECT:
        case ERROR_WINHTTP_CONNECTION_ERROR:
        case ERROR_WINHTTP_TIMEOUT:
        case ERROR_WINHTTP_AUTO_PROXY_SERVICE_ERROR:
        case ERROR_WINHTTP_BAD_AUTO_PROXY_SCRIPT:
        case ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT:
            return "Could not reach GitHub. Check the internet connection; Backtrack tries again later.";
        case ERROR_WINHTTP_SECURE_FAILURE:
        case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
        case ERROR_WINHTTP_SECURE_INVALID_CA:
        case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
        case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
        case ERROR_WINHTTP_SECURE_INVALID_CERT:
        case ERROR_WINHTTP_SECURE_CERT_REVOKED:
            return "The secure connection to GitHub could not be trusted (a proxy or security program may be in the "
                   "way). Nothing was downloaded.";
        default:
            return std::format("The connection to GitHub failed (error {}). Backtrack tries again later.", error);
    }
}

// Owns a WinHTTP session or connection handle.
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

class Sha256 {
public:
    Sha256() {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) ||
            !BCRYPT_SUCCESS(BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0))) {
            close();
            throw std::runtime_error("SHA-256 is not available (BCrypt)");
        }
    }
    ~Sha256() { close(); }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void add(const void* data, std::size_t size) {
        if (!BCRYPT_SUCCESS(BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), 0))) {
            throw std::runtime_error("BCryptHashData failed");
        }
    }

    std::string hex() {
        std::array<UCHAR, 32> digest{};
        if (!BCRYPT_SUCCESS(BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
            throw std::runtime_error("BCryptFinishHash failed");
        }
        std::string out;
        for (const UCHAR byte : digest) out += std::format("{:02x}", byte);
        return out;
    }

private:
    void close() {
        if (hash_ != nullptr) BCryptDestroyHash(hash_);
        if (algorithm_ != nullptr) BCryptCloseAlgorithmProvider(algorithm_, 0);
        hash_ = nullptr;
        algorithm_ = nullptr;
    }

    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

std::optional<std::string> hash_open_file(HANDLE file) {
    LARGE_INTEGER start{};
    if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) return std::nullopt;
    Sha256 sha;
    std::vector<char> buffer(kChunk);
    while (true) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) return std::nullopt;
        if (read == 0) break;
        sha.add(buffer.data(), read);
    }
    return sha.hex();
}

fs::path default_folder() {
    PWSTR local = nullptr;
    fs::path folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local))) {
        folder = fs::path(local) / L"Backtrack" / L"updates";
    }
    CoTaskMemFree(local);
    return folder;
}

}  // namespace

std::optional<fs::path> install_location() {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation", RRF_RT_REG_SZ, nullptr, nullptr, &bytes) !=
            ERROR_SUCCESS ||
        bytes < sizeof(wchar_t)) {
        return std::nullopt;
    }
    std::wstring location(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation", RRF_RT_REG_SZ, nullptr, location.data(),
                     &bytes) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    location.resize(wcsnlen(location.c_str(), location.size()));
    std::error_code ec;
    if (location.empty() || !fs::exists(fs::path(location) / L"backtrack.exe", ec)) return std::nullopt;
    return fs::path(location);
}

namespace {

// Whether `exe` sits in the folder Inno Setup installed Backtrack into for this Windows user.
bool runs_from_install_folder(const fs::path& exe) {
    const std::optional<fs::path> location = install_location();
    if (!location) return false;
    std::error_code ec;
    // equivalent() compares the folders themselves: case, a trailing backslash and 8.3 names do not matter.
    return fs::equivalent(exe.parent_path(), *location, ec) && !ec;
}

bool write_file_atomically(const fs::path& path, const std::string& text) {
    const fs::path temp = fs::path(path).concat(L".tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out.flush()) return false;
    }
    return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

std::optional<std::string> read_small_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string size_text(std::int64_t bytes) {
    return std::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

// One HTTPS GET on the worker thread. Redirects are followed here, not by WinHTTP, so every hop's
// address is checked before anything is sent to it.
//
// Cancelling: WinHTTP does not allow closing a synchronous request that another thread is using,
// so only this thread ever touches its handles. Another thread sets `cancelled`, which is looked at
// before every call; a call already waiting returns within one timeout at the most.
class HttpGet {
public:
    using HostRule = bool (*)(std::string_view host);

    HttpGet(const std::atomic<bool>& cancelled, const std::string& user_agent) : cancelled_(cancelled) {
        // AUTOMATIC_PROXY: the proxy Windows uses (settings, PAC script or WPAD), like a browser.
        session_ = std::make_unique<Internet>(WinHttpOpen(to_wide(user_agent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (session_->get() == nullptr) fail("WinHttpOpen");
        WinHttpSetTimeouts(session_->get(), kResolveTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs, kReceiveTimeoutMs);
    }
    HttpGet(const HttpGet&) = delete;
    HttpGet& operator=(const HttpGet&) = delete;

    // Sends the GET and waits for the answer's headers; returns the status code.
    int open(std::string url, const wchar_t* headers, HostRule allowed) {
        for (int hop = 0;; ++hop) {
            const std::optional<HttpsUrl> parsed = parse_https_url(url);
            if (!parsed || !allowed(parsed->host)) {
                throw UpdateError("The update server sent Backtrack somewhere it does not trust. Nothing was downloaded.",
                                  "refused address " + url.substr(0, 200));
            }
            send(*parsed, headers);
            const int status = status_code();
            const bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
            if (!redirect) return status;
            if (hop >= kMaxRedirects) throw UpdateError("The download was redirected too often.", "too many redirects");
            url = to_utf8(header(WINHTTP_QUERY_LOCATION));
        }
    }

    std::optional<std::int64_t> content_length() {
        const std::wstring text = header(WINHTTP_QUERY_CONTENT_LENGTH);
        if (text.empty() || !std::all_of(text.begin(), text.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }) ||
            text.size() > 18) {
            return std::nullopt;
        }
        return std::stoll(text);
    }

    // 0 at the end of the body.
    std::size_t read(char* buffer, std::size_t size) {
        throw_if_cancelled();
        DWORD got = 0;
        if (!WinHttpReadData(request_->get(), buffer, static_cast<DWORD>(size), &got)) fail("WinHttpReadData");
        return got;
    }

private:
    void send(const HttpsUrl& url, const wchar_t* headers) {
        throw_if_cancelled();
        request_.reset();
        connect_.reset();
        connect_ = std::make_unique<Internet>(WinHttpConnect(session_->get(), to_wide(url.host).c_str(),
                                                             INTERNET_DEFAULT_HTTPS_PORT, 0));
        if (connect_->get() == nullptr) fail("WinHttpConnect");
        request_ = std::make_unique<Internet>(WinHttpOpenRequest(connect_->get(), L"GET", to_wide(url.path).c_str(),
                                                                 nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                                 WINHTTP_FLAG_SECURE));
        if (request_->get() == nullptr) fail("WinHttpOpenRequest");
        DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request_->get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));
        DWORD cookies = WINHTTP_DISABLE_COOKIES;
        WinHttpSetOption(request_->get(), WINHTTP_OPTION_DISABLE_FEATURE, &cookies, sizeof(cookies));
        if (!WinHttpSendRequest(request_->get(), headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            fail("WinHttpSendRequest");
        }
        throw_if_cancelled();
        if (!WinHttpReceiveResponse(request_->get(), nullptr)) fail("WinHttpReceiveResponse");
    }

    int status_code() {
        DWORD status = 0;
        DWORD size = sizeof(status);
        if (!WinHttpQueryHeaders(request_->get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
            fail("WinHttpQueryHeaders");
        }
        return static_cast<int>(status);
    }

    std::wstring header(DWORD query) {
        DWORD bytes = 0;
        if (WinHttpQueryHeaders(request_->get(), query, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &bytes,
                                WINHTTP_NO_HEADER_INDEX) ||
            GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
            return {};
        }
        std::wstring text(bytes / sizeof(wchar_t) + 1, L'\0');
        if (!WinHttpQueryHeaders(request_->get(), query, WINHTTP_HEADER_NAME_BY_INDEX, text.data(), &bytes,
                                 WINHTTP_NO_HEADER_INDEX)) {
            return {};
        }
        text.resize(bytes / sizeof(wchar_t));
        return text;
    }

    void throw_if_cancelled() const {
        if (cancelled_.load()) throw Cancelled{};
    }

    [[noreturn]] void fail(const char* call) const {
        const DWORD error = GetLastError();
        throw_if_cancelled();
        throw UpdateError(winhttp_plain(error), std::format("{} failed with error {}", call, error));
    }

    const std::atomic<bool>& cancelled_;
    // Closed in reverse order: request, connection, session.
    std::unique_ptr<Internet> session_;
    std::unique_ptr<Internet> connect_;
    std::unique_ptr<Internet> request_;
};

bool api_host(std::string_view host) {
    return host == "api.github.com";
}

// CreateProcess, not ShellExecute: the per-user installer never needs a UAC prompt, and one that
// suddenly wanted it must fail rather than ask. Out of Backtrack's job object when it is in one
// (a launcher that ends its children with it would end the installer half way).
bool start_process(const fs::path& exe, const std::wstring& parameters, const fs::path& directory) {
    std::wstring command = L"\"" + exe.wstring() + L"\" " + parameters;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    BOOL ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB, nullptr,
                             directory.c_str(), &startup, &process);
    if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
        ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, directory.c_str(), &startup,
                            &process);
    }
    if (!ok) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

}  // namespace

Updater::Updater(UpdaterOptions options)
    : options_(std::move(options)),
      current_(parse_version(options_.current_version).value_or(Version{})),
      folder_(options_.folder.empty() ? default_folder() : options_.folder),
      installed_(runs_from_install_folder(options_.exe_path)),
      started_(std::chrono::steady_clock::now()) {
    load();
    log::info("Updater: Backtrack {}, {}", current_.text(),
              installed_ ? "installed copy (updates itself)" : "not the installed copy (only reports new versions)");
    start_worker();
}

void Updater::load() {
    std::lock_guard lock(mutex_);
    if (!folder_.empty()) {
        if (const auto text = read_small_file(folder_ / kStateFile)) record_ = parse_updater_record(*text, options_.source);
    }
    if (record_.ready) {
        std::error_code ec;
        const fs::path file = folder_ / to_wide(record_.ready->file);
        const auto size = fs::file_size(file, ec);
        if (record_.ready->version <= current_) {
            // The update was installed (or is older than this copy): the installer is not needed.
            forget_ready();
        } else if (ec || static_cast<std::int64_t>(size) != record_.ready->size) {
            log::warn("The downloaded update {} is missing or damaged; it is downloaded again",
                      record_.ready->version.text());
            forget_ready();
        } else {
            state_ = ready_state();
            if (record_.ready->install_attempts > 0) {
                log::warn("Update {} was started {} time(s) but Backtrack is still {}; see {}",
                          record_.ready->version.text(), record_.ready->install_attempts, current_.text(),
                          path_text(folder_ / kInstallLog));
            }
        }
        save_record();
    }
    clean_folder();
    state_.last_check = record_.last_check.value_or(0);
    if (state_.page_url.empty()) state_.page_url = options_.source.releases_page();
}

Updater::~Updater() {
    stop_worker();
}

void Updater::start_worker() {
    {
        std::lock_guard lock(mutex_);
        stop_ = false;
    }
    thread_ = std::thread([this] { run(); });
}

void Updater::stop_worker() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cancelled_ = true;  // the worker stops before its next WinHTTP call or file write
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Updater::notify(HWND window, UINT message) {
    std::lock_guard lock(mutex_);
    window_ = window;
    message_ = message;
}

void Updater::set_automatic(bool on) {
    {
        std::lock_guard lock(mutex_);
        if (automatic_ == on) return;
        automatic_ = on;
        schedule_changed_ = true;
    }
    wake_.notify_all();
}

UpdateState Updater::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

bool Updater::check() {
    std::lock_guard lock(mutex_);
    if (busy_ || job_ != Job::None || stop_) return false;
    job_ = Job::Check;
    UpdateState checking = state_;
    checking.stage = UpdateStage::Checking;
    checking.error.clear();
    set_state(std::move(checking));
    wake_.notify_all();
    return true;
}

bool Updater::download() {
    std::lock_guard lock(mutex_);
    if (busy_ || job_ != Job::None || stop_ || state_.stage != UpdateStage::Available || !state_.can_install ||
        !available_) {
        return false;
    }
    job_ = Job::Download;
    UpdateState downloading = state_;
    downloading.stage = UpdateStage::Downloading;
    downloading.fraction = 0.0;
    set_state(std::move(downloading));
    wake_.notify_all();
    return true;
}

void Updater::set_state(UpdateState state) {
    state.last_check = record_.last_check.value_or(0);
    if (state.page_url.empty()) state.page_url = options_.source.releases_page();
    state_ = std::move(state);
    if (window_ != nullptr) PostMessageW(window_, message_, 0, 0);
}

UpdateState Updater::ready_state() const {
    UpdateState ready;
    ready.stage = UpdateStage::Ready;
    ready.version = record_.ready->version.text();
    ready.notes = record_.ready->notes;
    ready.page_url = record_.ready->page_url;
    ready.can_install = installed_;
    return ready;
}

std::optional<std::int64_t> Updater::seconds_until_auto_check() const {
    AutoCheckInput input;
    input.enabled = automatic_;
    input.seconds_running =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_).count();
    input.now = unix_now();
    input.last_check = record_.last_check;
    input.last_check_failed = record_.last_check_failed;
    return bt::seconds_until_auto_check(input);
}

void Updater::save_record() {
    if (folder_.empty()) return;
    std::error_code ec;
    fs::create_directories(folder_, ec);
    if (!write_file_atomically(folder_ / kStateFile, dump_updater_record(record_))) {
        log::warn("Could not save {}", path_text(folder_ / kStateFile));
    }
}

void Updater::forget_ready() {
    if (!record_.ready) return;
    std::error_code ec;
    fs::remove(folder_ / to_wide(record_.ready->file), ec);  // may still run (the installer just ended): next start
    record_.ready.reset();
}

// Old installers, partial downloads (Backtrack quit or crashed during one) and stray temp files.
void Updater::clean_folder() {
    if (folder_.empty()) return;
    std::error_code ec;
    const std::wstring keep = record_.ready ? to_wide(record_.ready->file) : std::wstring();
    const std::wstring prefix = to_wide(options_.source.asset_prefix);
    for (fs::directory_iterator it(folder_, ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring name = it->path().filename().wstring();
        const bool ours = name.ends_with(L".part") || name.ends_with(L".tmp") ||
                          (name.size() > prefix.size() && _wcsnicmp(name.c_str(), prefix.c_str(), prefix.size()) == 0);
        if (!ours || _wcsicmp(name.c_str(), keep.c_str()) == 0) continue;
        std::error_code remove_ec;
        if (fs::remove(it->path(), remove_ec)) log::info("Removed old update file {}", to_utf8(name));
    }
}

void Updater::run() {
    std::unique_lock lock(mutex_);
    while (!stop_) {
        if (job_ == Job::None) {
            const std::optional<std::int64_t> due = seconds_until_auto_check();
            if (due && *due == 0) {
                job_ = Job::AutoCheck;
            } else {
                const auto wait = due ? std::min<std::chrono::steady_clock::duration>(std::chrono::seconds(*due), kMaxAutoWait)
                                      : std::chrono::steady_clock::duration(kMaxAutoWait);
                wake_.wait_for(lock, wait, [this] { return stop_ || job_ != Job::None || schedule_changed_; });
                schedule_changed_ = false;
                continue;  // also after set_automatic(): the schedule is worked out again
            }
        }
        const Job job = std::exchange(job_, Job::None);
        std::optional<Release> release = available_;
        busy_ = true;
        // Only stop_worker() sets it, and it sets stop_ first under this lock: a stop that came
        // before this point ended the loop above, so this never wipes out a cancel.
        cancelled_ = false;
        lock.unlock();
        try {
            if (job == Job::Download && release) {
                do_download(*release);
            } else if (job == Job::Check || job == Job::AutoCheck) {
                do_check(job == Job::AutoCheck);
            }
        } catch (const Cancelled&) {
            log::info("Update {} cancelled", job == Job::Download ? "download" : "check");
        } catch (const std::exception& e) {
            log::error("Updater: {}", e.what());  // never ends the worker: the next check tries again
        }
        lock.lock();
        busy_ = false;
        if (stop_) break;
    }
}

void Updater::do_check(bool automatic) {
    UpdateState before;
    {
        std::lock_guard lock(mutex_);
        before = state_;
        if (automatic) {
            UpdateState checking = state_;
            checking.stage = UpdateStage::Checking;
            set_state(std::move(checking));
        }
    }
    const std::string user_agent = "Backtrack-Updater/" + current_.text();
    CheckResult result;
    try {
        HttpGet http(cancelled_, user_agent);
        const int status = http.open("https://api.github.com" + options_.source.api_path(),
                                     L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n",
                                     api_host);
        std::string body;
        std::vector<char> buffer(kChunk);
        while (const std::size_t got = http.read(buffer.data(), buffer.size())) {
            body.append(buffer.data(), got);
            if (body.size() > kMaxApiBody) throw UpdateError("GitHub's answer to the update check was too large.", "body too large");
        }
        result = read_latest_release(status, body, current_, options_.source);
        if (result.outcome == CheckOutcome::Failed) log::warn("Update check: HTTP {}: {}", status, body.substr(0, 300));
    } catch (const UpdateError& e) {
        log::warn("Update check failed: {}", e.what());
        result = CheckResult{};
        result.outcome = CheckOutcome::Failed;
        result.error = e.plain();
    } catch (const std::exception& e) {
        // Out of memory, BCrypt or the file system: still a failed check, never a stuck "Checking".
        log::error("Update check failed: {}", e.what());
        result = CheckResult{};
        result.outcome = CheckOutcome::Failed;
        result.error = "The update check failed. Backtrack tries again later.";
    }

    bool start_download = false;
    std::lock_guard lock(mutex_);
    record_.last_check = unix_now();
    record_.last_check_failed = result.outcome == CheckOutcome::Failed;
    switch (result.outcome) {
        case CheckOutcome::UpToDate:
            log::info("Update check: Backtrack {} is up to date{}", current_.text(),
                      result.no_releases ? " (no releases published yet)" : "");
            // A waiting installer for a release GitHub no longer offers (every release deleted, or
            // the newest one older than it) is not installed. A latest release whose tag cannot be
            // read (a typo, a pre-release) says nothing about it, so it stays.
            if (record_.ready && (result.no_releases || (result.latest && *result.latest < record_.ready->version))) {
                log::info("Update {} was withdrawn on GitHub; its installer is deleted", record_.ready->version.text());
                forget_ready();
            }
            available_.reset();
            set_state(UpdateState{UpdateStage::UpToDate});
            break;
        case CheckOutcome::Available: {
            const Release& release = result.release;
            log::info("Update check: {} is available{}", release.version.text(),
                      release.installer ? "" : " (no installer attached to the release)");
            if (record_.ready && record_.ready->version == release.version) {
                set_state(ready_state());
                break;
            }
            if (record_.ready && record_.ready->version > release.version) {
                log::info("Update {} was withdrawn on GitHub; its installer is deleted", record_.ready->version.text());
                forget_ready();
            }
            available_ = release;
            UpdateState available;
            available.stage = UpdateStage::Available;
            available.version = release.version.text();
            available.notes = release.notes;
            available.page_url = release.page_url;
            available.can_install = release.installer.has_value() && (installed_ || options_.download_anywhere);
            start_download = automatic_ && available.can_install;
            if (start_download) {
                available.stage = UpdateStage::Downloading;
                available.fraction = 0.0;
            }
            set_state(std::move(available));
            break;
        }
        case CheckOutcome::Failed:
            if (record_.ready) {
                set_state(ready_state());  // what is downloaded stays installable
            } else if (automatic) {
                set_state(before);  // nobody asked: the Settings page keeps what it showed
            } else {
                UpdateState failed;
                failed.stage = UpdateStage::Failed;
                failed.error = result.error;
                set_state(std::move(failed));
            }
            break;
    }
    save_record();
    if (start_download) job_ = Job::Download;  // the worker loop picks it up next, without waiting
}

void Updater::do_download(const Release& release) {
    const ReleaseAsset& asset = *release.installer;
    const fs::path final_path = folder_ / to_wide(asset.name);
    const fs::path part_path = fs::path(final_path).concat(std::format(L".{}.part", GetCurrentProcessId()));
    const auto fail_state = [this, &release](const std::string& plain) {
        std::lock_guard lock(mutex_);
        record_.last_check_failed = true;  // the automatic mode tries again in an hour, not a day
        save_record();
        if (record_.ready) {
            set_state(ready_state());  // an older update already downloaded still installs
            return;
        }
        UpdateState failed;
        failed.stage = UpdateStage::Failed;
        failed.version = release.version.text();
        failed.page_url = release.page_url;
        failed.error = plain;
        set_state(std::move(failed));
    };

    struct PartFile {
        fs::path path;
        UniqueHandle handle;
        ~PartFile() {
            handle.reset();
            std::error_code ec;
            if (!path.empty()) fs::remove(path, ec);  // never leaves a partial file behind
        }
    } part;

    try {
        if (folder_.empty()) throw UpdateError("Backtrack cannot find its folder for updates.", "no LOCALAPPDATA");
        if (asset.size > kMaxInstallerSize) throw UpdateError("The update is far larger than expected; it was not downloaded.", "asset too large");
        std::error_code ec;
        fs::create_directories(folder_, ec);
        part.path = part_path;
        part.handle.reset(CreateFileW(part_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!part.handle) {
            const DWORD error = GetLastError();
            part.path.clear();
            throw UpdateError("Backtrack could not save the update in its folder.",
                              std::format("CreateFileW {} failed: {}", path_text(part_path), error));
        }

        HttpGet http(cancelled_, "Backtrack-Updater/" + current_.text());
        const int status = http.open(asset.url, L"Accept: application/octet-stream\r\n", is_trusted_download_host);
        if (status != kHttpOk) {
            throw UpdateError(std::format("GitHub answered the download with an error (HTTP {}). Backtrack tries again later.", status),
                              std::format("download HTTP {}", status));
        }
        if (const auto length = http.content_length(); length && *length != asset.size) {
            throw UpdateError("The update on GitHub is not the size GitHub says it is; it was not installed.",
                              std::format("Content-Length {} but the asset size is {}", *length, asset.size));
        }
        Sha256 sha;
        std::vector<char> buffer(kChunk);
        std::int64_t written = 0;
        auto last_progress = std::chrono::steady_clock::now();
        while (const std::size_t got = http.read(buffer.data(), buffer.size())) {
            written += static_cast<std::int64_t>(got);
            if (written > asset.size) throw UpdateError("The download was larger than expected; it was thrown away.", "too many bytes");
            DWORD wrote = 0;
            if (!WriteFile(part.handle.get(), buffer.data(), static_cast<DWORD>(got), &wrote, nullptr) || wrote != got) {
                const DWORD error = GetLastError();
                throw UpdateError(error == ERROR_DISK_FULL || error == ERROR_HANDLE_DISK_FULL
                                      ? "There is not enough free space on the drive for the update."
                                      : "Backtrack could not save the update in its folder.",
                                  std::format("WriteFile failed: {}", error));
            }
            sha.add(buffer.data(), got);
            const auto now = std::chrono::steady_clock::now();
            if (now - last_progress >= kProgressInterval) {
                last_progress = now;
                std::lock_guard lock(mutex_);
                UpdateState progress = state_;
                progress.stage = UpdateStage::Downloading;
                progress.fraction = static_cast<double>(written) / static_cast<double>(asset.size);
                set_state(std::move(progress));
            }
        }
        if (written != asset.size) {
            throw UpdateError("The download stopped before the end. Backtrack tries again later.",
                              std::format("got {} of {} bytes", written, asset.size));
        }
        const std::string hash = sha.hex();
        if (!asset.sha256.empty() && hash != asset.sha256) {
            throw UpdateError("The downloaded update did not match GitHub's checksum and was deleted.",
                              std::format("SHA-256 {} but GitHub says {}", hash, asset.sha256));
        }
        if (!FlushFileBuffers(part.handle.get())) {
            throw UpdateError("Backtrack could not save the update in its folder.", std::format("FlushFileBuffers: {}", GetLastError()));
        }
        part.handle.reset();
        if (!MoveFileExW(part_path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw UpdateError("Backtrack could not save the update in its folder.",
                              std::format("MoveFileExW to {} failed: {}", path_text(final_path), GetLastError()));
        }
        part.path.clear();  // it is the final file now

        std::lock_guard lock(mutex_);
        UpdaterRecord::Ready ready;
        ready.version = release.version;
        ready.file = asset.name;
        ready.size = asset.size;
        ready.sha256 = hash;
        ready.notes = release.notes;
        ready.page_url = release.page_url;
        record_.ready = std::move(ready);
        record_.last_check_failed = false;
        save_record();
        clean_folder();
        available_.reset();
        log::info("Downloaded update {} ({}), {}", release.version.text(), size_text(asset.size),
                  asset.sha256.empty() ? "size checked (GitHub gave no checksum)" : "SHA-256 matches GitHub's");
        set_state(ready_state());
    } catch (const UpdateError& e) {
        log::warn("Update download failed: {}", e.what());
        fail_state(e.plain());
    } catch (const std::exception& e) {
        log::error("Update download failed: {}", e.what());
        fail_state("The download failed. Backtrack tries again later.");
    }
}

bool Updater::install_now() {
    return install(Relaunch::Shown, false);
}

bool Updater::install_on_exit() {
    {
        std::lock_guard lock(mutex_);
        if (!automatic_) return false;
    }
    return install(Relaunch::No, true);
}

bool Updater::install_at_start(bool hidden) {
    {
        std::lock_guard lock(mutex_);
        if (!automatic_ || !record_.ready) return false;
    }
    return install(hidden ? Relaunch::Hidden : Relaunch::Shown, true);
}

bool Updater::install(Relaunch relaunch, bool automatic) {
    {
        std::lock_guard lock(mutex_);
        if (!record_.ready || !installed_) return false;
        if (automatic && record_.ready->install_attempts >= kMaxAutomaticInstallAttempts) {
            log::warn("Update {} is not installed automatically again: it failed {} times",
                      record_.ready->version.text(), record_.ready->install_attempts);
            return false;
        }
    }
    // Nothing may replace or delete the installer between the check below and its start.
    stop_worker();
    bool started = false;
    bool withdrawn = false;
    {
        std::lock_guard lock(mutex_);
        withdrawn = !record_.ready;  // the check that was running when stop_worker() came dropped it
    }
    if (withdrawn) {
        start_worker();
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        const UpdaterRecord::Ready ready = *record_.ready;
        const fs::path file = folder_ / to_wide(ready.file);
        // Open with only read sharing: nobody can change, rename or delete it until it runs.
        UniqueHandle handle(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr));
        const DWORD open_error = handle ? ERROR_SUCCESS : GetLastError();
        LARGE_INTEGER size{};
        std::optional<std::string> hash;
        try {
            if (handle && GetFileSizeEx(handle.get(), &size) && size.QuadPart == ready.size) hash = hash_open_file(handle.get());
        } catch (const std::exception& e) {
            log::error("Could not check the downloaded update: {}", e.what());  // treated as damaged
        }
        if (open_error == ERROR_SHARING_VIOLATION) {
            // A virus scanner looking at the new installer: the file is fine, only busy.
            log::warn("The downloaded update {} is in use by another program; not installing now", ready.version.text());
            UpdateState busy = ready_state();
            busy.stage = UpdateStage::Failed;
            busy.error = "The update is being checked by another program (a virus scanner?). Try again in a moment.";
            set_state(std::move(busy));
        } else if (!hash || *hash != ready.sha256) {
            log::error("The downloaded update {} failed verification before installing; deleted it", ready.version.text());
            handle.reset();
            forget_ready();
            save_record();
            UpdateState failed;
            failed.stage = UpdateStage::Failed;
            failed.error = "The downloaded update was damaged and has been deleted. Backtrack downloads it again later.";
            set_state(std::move(failed));
        } else {
            ++record_.ready->install_attempts;  // counted before: a crash during the install counts too
            save_record();
            std::wstring parameters = std::format(L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /WAITPID={} \"/LOG={}\"",
                                                  GetCurrentProcessId(), (folder_ / kInstallLog).wstring());
            if (relaunch == Relaunch::Shown) parameters += L" /RELAUNCH";
            if (relaunch == Relaunch::Hidden) parameters += L" /RELAUNCH=hidden";
            started = start_process(file, parameters, folder_);
            if (started) {
                log::info("Started the installer for update {} ({}); Backtrack ends now", ready.version.text(),
                          relaunch == Relaunch::No ? "not starting again" : "starting again afterwards");
            } else {
                const DWORD error = GetLastError();
                log::error("Could not start the installer {}: error {}", path_text(file), error);
                --record_.ready->install_attempts;
                save_record();
                UpdateState failed = ready_state();
                failed.stage = UpdateStage::Failed;
                failed.error = "Windows did not start the update's installer.";
                set_state(std::move(failed));
            }
        }
    }
    if (!started) start_worker();
    return started;
}

std::optional<std::string> sha256_of_file(const fs::path& file) {
    const UniqueHandle handle(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle) return std::nullopt;
    return hash_open_file(handle.get());
}

}  // namespace bt
