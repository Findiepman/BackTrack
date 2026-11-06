#pragma once

// What the updater decides without the network: reading GitHub's "latest release" answer, which
// version is newer, which download URLs may be followed, when the automatic mode checks next and
// the small file the updater keeps between runs. Pure logic, no Windows: platform/Updater.cpp does
// the HTTPS requests, the download and running the installer.

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bt {

struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;

    std::string text() const;  // "0.4.0": the form the installer's file name uses
    friend auto operator<=>(const Version&, const Version&) = default;
};

// "0.4.0", "v0.4.0" or "V0.4.0"; one to three numbers ("1.2" is 1.2.0). Anything else (a "-beta"
// suffix, letters, spaces, four numbers, a number over 99999) is nullopt, so a tag we do not
// understand never offers an update.
std::optional<Version> parse_version(std::string_view text);

// Where releases come from. The defaults are Backtrack's own; the update_check tool points them
// at another public repository to try a real download.
struct ReleaseSource {
    std::string owner = "Findiepman";
    std::string repo = "BackTrack";
    // The asset to download is <prefix><version><suffix>, e.g. Backtrack-Setup-0.4.0.exe.
    std::string asset_prefix = "Backtrack-Setup-";
    std::string asset_suffix = ".exe";

    std::string api_path() const;  // "/repos/Findiepman/BackTrack/releases/latest"
    std::string releases_page() const;  // "https://github.com/Findiepman/BackTrack/releases"
    std::string asset_name(const Version& version) const;
};

struct ReleaseAsset {
    std::string name;
    std::int64_t size = 0;
    std::string url;  // browser_download_url, already checked with is_release_download_url
    std::string sha256;  // 64 lower-case hex digits from the "digest" field; empty when GitHub gave none
};

struct Release {
    Version version;
    std::string tag;
    std::string name;
    std::string notes;  // the release text as written on GitHub (Markdown)
    std::string page_url;  // html_url, or the releases page
    std::optional<ReleaseAsset> installer;  // empty: nothing to download, only the page to show
};

enum class CheckOutcome { UpToDate, Available, Failed };

struct CheckResult {
    CheckOutcome outcome = CheckOutcome::Failed;
    Release release;  // for Available
    std::optional<Version> latest;  // the newest release's version, when GitHub's answer had a readable one
    bool no_releases = false;  // UpToDate because the repository has no release yet (GitHub's 404)
    bool rate_limited = false;  // Failed because GitHub refused more checks from this network for now
    std::string error;  // Failed: one plain line for the window
};

// Reads GET /repos/<owner>/<repo>/releases/latest. `status` is the HTTP status code.
CheckResult read_latest_release(int status, std::string_view body, const Version& current,
                                const ReleaseSource& source = {});

// "sha256:<64 hex digits>" -> the digits in lower case; anything else (missing, another
// algorithm, wrong length) -> nullopt.
std::optional<std::string> parse_sha256_digest(std::string_view digest);

struct HttpsUrl {
    std::string host;  // lower case
    std::string path;  // "/..." including the query, never empty
};

// Only "https://host/path" (or with ":443"). No other scheme, port or user name, so a redirect can
// never take a download to plain HTTP or to a look-alike address.
std::optional<HttpsUrl> parse_https_url(std::string_view url);

// The hosts a release download may be served from: github.com, which redirects to
// release-assets.githubusercontent.com (objects.githubusercontent.com before 2025).
bool is_trusted_download_host(std::string_view host);

// browser_download_url must be https://github.com/<owner>/<repo>/releases/download/...
bool is_release_download_url(std::string_view url, const ReleaseSource& source);

// Automatic mode: when to check next.
inline constexpr std::int64_t kAutoCheckStartDelayS = 90;  // after startup: never slow down the boot
inline constexpr std::int64_t kAutoCheckIntervalS = 24 * 60 * 60;
inline constexpr std::int64_t kAutoCheckRetryS = 60 * 60;  // after a check that could not reach GitHub

struct AutoCheckInput {
    bool enabled = false;  // Settings::auto_update
    std::int64_t seconds_running = 0;  // since Backtrack started
    std::int64_t now = 0;  // Unix seconds
    std::optional<std::int64_t> last_check;  // Unix seconds of the last automatic or manual check
    bool last_check_failed = false;
};

// Seconds until the next automatic check (0: now), or nullopt when the automatic mode is off.
std::optional<std::int64_t> seconds_until_auto_check(const AutoCheckInput& input);

// An installer that failed this often is not started automatically again (at quit or startup), so
// a broken update can never keep Backtrack from starting. The button still can.
inline constexpr int kMaxAutomaticInstallAttempts = 2;

// updates\state.json: what the updater remembers between runs.
struct UpdaterRecord {
    std::optional<std::int64_t> last_check;  // Unix seconds, never negative
    bool last_check_failed = false;

    // A verified installer waiting in the updates folder.
    struct Ready {
        Version version;
        std::string file;  // always source.asset_name(version): never a path from the file itself
        std::int64_t size = 0;
        std::string sha256;  // of the downloaded file (from GitHub's digest, or computed when it had none)
        std::string notes;
        std::string page_url;
        int install_attempts = 0;  // installers started for this version that did not update Backtrack
    };
    std::optional<Ready> ready;
};

// Never fails: a damaged or hand-edited file gives an empty record, and a "ready" entry whose file
// name is not the expected asset name is dropped.
UpdaterRecord parse_updater_record(std::string_view text, const ReleaseSource& source = {});
std::string dump_updater_record(const UpdaterRecord& record);

}  // namespace bt
