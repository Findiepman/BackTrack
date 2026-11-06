#include "core/UpdateCheck.h"

#include "core/Json.h"

#include <algorithm>
#include <array>
#include <format>

namespace bt {
namespace {

constexpr int kMaxVersionPart = 99999;
constexpr std::size_t kSha256HexDigits = 64;
constexpr int kHttpOk = 200;
constexpr int kHttpForbidden = 403;
constexpr int kHttpNotFound = 404;
constexpr int kHttpTooManyRequests = 429;
// A clock that jumped back by more than this makes a stored check time meaningless.
constexpr std::int64_t kClockSlackS = 5 * 60;

char lower_ascii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), lower_ascii);
    return out;
}

bool equals_ignoring_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && lower(a) == lower(b);
}

bool starts_with_ignoring_case(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && equals_ignoring_case(text.substr(0, prefix.size()), prefix);
}

bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

CheckResult failed(std::string error, bool rate_limited = false) {
    CheckResult result;
    result.outcome = CheckOutcome::Failed;
    result.rate_limited = rate_limited;
    result.error = std::move(error);
    return result;
}

CheckResult up_to_date(bool no_releases = false) {
    CheckResult result;
    result.outcome = CheckOutcome::UpToDate;
    result.no_releases = no_releases;
    return result;
}

std::optional<ReleaseAsset> find_installer(const Json& assets, const Version& version, const ReleaseSource& source) {
    const std::string wanted = source.asset_name(version);
    for (const Json& asset : assets.as_array()) {
        if (!equals_ignoring_case(asset["name"].as_string(), wanted)) continue;
        // "starter" while an upload is still running: its bytes are not all there yet.
        const Json& state = asset["state"];
        if (!state.is_null() && state.as_string() != "uploaded") return std::nullopt;
        ReleaseAsset found;
        found.name = wanted;
        found.size = asset["size"].as_int(0);
        found.url = asset["browser_download_url"].as_string();
        found.sha256 = parse_sha256_digest(asset["digest"].as_string()).value_or(std::string());
        if (found.size <= 0 || !is_release_download_url(found.url, source)) return std::nullopt;
        return found;
    }
    return std::nullopt;
}

}  // namespace

std::string Version::text() const {
    return std::format("{}.{}.{}", major, minor, patch);
}

std::optional<Version> parse_version(std::string_view text) {
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) text.remove_prefix(1);
    std::array<int, 3> parts{};
    std::size_t count = 0;
    std::size_t i = 0;
    while (true) {
        if (count == parts.size() || i >= text.size() || text[i] < '0' || text[i] > '9') return std::nullopt;
        int value = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            value = value * 10 + (text[i] - '0');
            if (value > kMaxVersionPart) return std::nullopt;
            ++i;
        }
        parts[count++] = value;
        if (i == text.size()) break;
        if (text[i] != '.') return std::nullopt;
        ++i;  // a trailing dot then fails on the next round: "1." is not a version
    }
    return Version{parts[0], parts[1], parts[2]};
}

std::string ReleaseSource::api_path() const {
    return std::format("/repos/{}/{}/releases/latest", owner, repo);
}

std::string ReleaseSource::releases_page() const {
    return std::format("https://github.com/{}/{}/releases", owner, repo);
}

std::string ReleaseSource::asset_name(const Version& version) const {
    return asset_prefix + version.text() + asset_suffix;
}

CheckResult read_latest_release(int status, std::string_view body, const Version& current,
                                const ReleaseSource& source) {
    // GitHub answers 404 both for "no releases yet" and for a repository it does not show; either
    // way there is nothing newer to install.
    if (status == kHttpNotFound) return up_to_date(true);
    const auto parsed = parse_json(body);
    if (status == kHttpTooManyRequests ||
        (status == kHttpForbidden && parsed.value &&
         lower((*parsed.value)["message"].as_string()).find("rate limit") != std::string::npos)) {
        return failed("GitHub has had too many update checks from your network. Backtrack tries again later.", true);
    }
    if (status != kHttpOk) {
        return failed(std::format("GitHub answered the update check with an error (HTTP {}). Backtrack tries again later.",
                                  status));
    }
    if (!parsed.value || !parsed.value->is_object()) {
        return failed("GitHub's answer to the update check could not be read. Backtrack tries again later.");
    }
    const Json& json = *parsed.value;
    // /releases/latest never returns these, but a changed API must not install a test build.
    if (json["draft"].as_bool(false) || json["prerelease"].as_bool(false)) return up_to_date();
    const std::optional<Version> version = parse_version(json["tag_name"].as_string());
    if (!version || *version <= current) {
        CheckResult result = up_to_date();
        result.latest = version;
        return result;
    }

    CheckResult result;
    result.outcome = CheckOutcome::Available;
    result.latest = version;
    Release& release = result.release;
    release.version = *version;
    release.tag = json["tag_name"].as_string();
    release.name = json["name"].as_string();
    release.notes = json["body"].as_string();
    const std::string page = json["html_url"].as_string();
    release.page_url = starts_with_ignoring_case(page, "https://github.com/") ? page : source.releases_page();
    release.installer = find_installer(json["assets"], *version, source);
    return result;
}

std::optional<std::string> parse_sha256_digest(std::string_view digest) {
    constexpr std::string_view kPrefix = "sha256:";
    if (!starts_with_ignoring_case(digest, kPrefix)) return std::nullopt;
    digest.remove_prefix(kPrefix.size());
    if (digest.size() != kSha256HexDigits || !std::all_of(digest.begin(), digest.end(), is_hex)) return std::nullopt;
    return lower(digest);
}

std::optional<HttpsUrl> parse_https_url(std::string_view url) {
    constexpr std::string_view kScheme = "https://";
    if (!starts_with_ignoring_case(url, kScheme)) return std::nullopt;
    url.remove_prefix(kScheme.size());
    for (const char c : url) {
        if (static_cast<unsigned char>(c) <= ' ' || static_cast<unsigned char>(c) >= 0x7F) return std::nullopt;
    }
    const std::size_t authority_end = url.find_first_of("/?#");
    std::string_view authority = url.substr(0, authority_end);
    std::string_view rest = authority_end == std::string_view::npos ? std::string_view() : url.substr(authority_end);
    if (authority.ends_with(":443")) authority.remove_suffix(4);
    if (authority.empty() || authority.front() == '.' || authority.front() == '-') return std::nullopt;
    for (const char c : authority) {
        // No ':' (another port), '@' (a user name in front of the real host) or anything exotic.
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '-';
        if (!ok) return std::nullopt;
    }
    if (const std::size_t fragment = rest.find('#'); fragment != std::string_view::npos) rest = rest.substr(0, fragment);
    HttpsUrl out;
    out.host = lower(authority);
    out.path = rest.empty() ? std::string("/") : rest.front() == '?' ? "/" + std::string(rest) : std::string(rest);
    return out;
}

bool is_trusted_download_host(std::string_view host) {
    return equals_ignoring_case(host, "github.com") ||
           equals_ignoring_case(host, "release-assets.githubusercontent.com") ||
           equals_ignoring_case(host, "objects.githubusercontent.com");
}

bool is_release_download_url(std::string_view url, const ReleaseSource& source) {
    const auto parsed = parse_https_url(url);
    if (!parsed || parsed->host != "github.com") return false;
    const std::string prefix = std::format("/{}/{}/releases/download/", source.owner, source.repo);
    return parsed->path.size() > prefix.size() && starts_with_ignoring_case(parsed->path, prefix) &&
           parsed->path.find("..") == std::string::npos;
}

std::optional<std::int64_t> seconds_until_auto_check(const AutoCheckInput& input) {
    if (!input.enabled) return std::nullopt;
    std::int64_t wait = std::max<std::int64_t>(0, kAutoCheckStartDelayS - input.seconds_running);
    if (input.last_check && *input.last_check >= 0 && *input.last_check <= input.now + kClockSlackS) {
        const std::int64_t interval = input.last_check_failed ? kAutoCheckRetryS : kAutoCheckIntervalS;
        wait = std::max(wait, *input.last_check + interval - input.now);
    }
    return wait;
}

UpdaterRecord parse_updater_record(std::string_view text, const ReleaseSource& source) {
    UpdaterRecord record;
    const auto parsed = parse_json(text);
    if (!parsed.value || !parsed.value->is_object()) return record;
    const Json& json = *parsed.value;
    // Negative (hand-edited) would overflow the schedule's arithmetic; "never checked" instead.
    if (json["last_check"].is_number() && json["last_check"].as_int() >= 0) record.last_check = json["last_check"].as_int();
    record.last_check_failed = json["last_check_failed"].as_bool(false);
    const Json& ready = json["ready"];
    const std::optional<Version> version = parse_version(ready["version"].as_string());
    const std::optional<std::string> sha256 = parse_sha256_digest("sha256:" + ready["sha256"].as_string());
    if (version && sha256 && ready["file"].as_string() == source.asset_name(*version) && ready["size"].as_int() > 0) {
        UpdaterRecord::Ready entry;
        entry.version = *version;
        entry.file = source.asset_name(*version);
        entry.size = ready["size"].as_int();
        entry.sha256 = *sha256;
        entry.notes = ready["notes"].as_string();
        entry.page_url = ready["page_url"].as_string();
        entry.install_attempts = static_cast<int>(std::clamp<std::int64_t>(ready["install_attempts"].as_int(), 0, 100));
        record.ready = std::move(entry);
    }
    return record;
}

std::string dump_updater_record(const UpdaterRecord& record) {
    Json json;
    if (record.last_check) json.set("last_check", Json(*record.last_check));
    json.set("last_check_failed", Json(record.last_check_failed));
    if (record.ready) {
        Json ready;
        ready.set("version", Json(record.ready->version.text()));
        ready.set("file", Json(record.ready->file));
        ready.set("size", Json(record.ready->size));
        ready.set("sha256", Json(record.ready->sha256));
        ready.set("notes", Json(record.ready->notes));
        ready.set("page_url", Json(record.ready->page_url));
        ready.set("install_attempts", Json(record.ready->install_attempts));
        json.set("ready", std::move(ready));
    }
    return json.dump(2) + "\n";
}

}  // namespace bt
