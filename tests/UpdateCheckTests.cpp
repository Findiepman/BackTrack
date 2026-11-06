#include "core/UpdateCheck.h"

#include "Check.h"

#include <limits>

using namespace bt;

namespace {

const Version kCurrent{0, 3, 0};

// Shaped like GET /repos/{owner}/{repo}/releases/latest in GitHub's REST docs (2022-11-28), cut
// to the fields that matter plus a few that must be ignored. The digest is the "sha256:..." GitHub
// adds to assets uploaded since mid 2025.
constexpr std::string_view kRelease = R"({
  "url": "https://api.github.com/repos/Findiepman/BackTrack/releases/1",
  "html_url": "https://github.com/Findiepman/BackTrack/releases/tag/V0.4.0",
  "assets_url": "https://api.github.com/repos/Findiepman/BackTrack/releases/1/assets",
  "upload_url": "https://uploads.github.com/repos/Findiepman/BackTrack/releases/1/assets{?name,label}",
  "id": 1,
  "author": {"login": "Findiepman", "id": 1, "type": "User", "site_admin": false},
  "node_id": "MDc6UmVsZWFzZTE=",
  "tag_name": "V0.4.0",
  "target_commitish": "main",
  "name": "Backtrack 0.4.0",
  "body": "- Updates itself\n- Faster \"Saved\" bar",
  "draft": false,
  "prerelease": false,
  "immutable": false,
  "created_at": "2026-11-10T12:00:00Z",
  "published_at": "2026-11-10T12:05:00Z",
  "assets": [
    {
      "url": "https://api.github.com/repos/Findiepman/BackTrack/releases/assets/7",
      "browser_download_url": "https://github.com/Findiepman/BackTrack/releases/download/V0.4.0/notes.txt",
      "id": 7, "name": "notes.txt", "label": "", "state": "uploaded", "content_type": "text/plain",
      "size": 12, "digest": null, "download_count": 0
    },
    {
      "url": "https://api.github.com/repos/Findiepman/BackTrack/releases/assets/8",
      "browser_download_url": "https://github.com/Findiepman/BackTrack/releases/download/V0.4.0/Backtrack-Setup-0.4.0.exe",
      "id": 8,
      "node_id": "RA_kwDOAAAAAc4AAAAI",
      "name": "Backtrack-Setup-0.4.0.exe",
      "label": "",
      "state": "uploaded",
      "content_type": "application/x-msdownload",
      "size": 2154321,
      "digest": "sha256:F756E7859D90DBB5E85AAE5194206C37DA220BEC9BAC23D3D659078A8E766FDC",
      "download_count": 42,
      "created_at": "2026-11-10T12:04:00Z",
      "updated_at": "2026-11-10T12:04:30Z",
      "uploader": {"login": "Findiepman", "id": 1}
    }
  ],
  "tarball_url": "https://api.github.com/repos/Findiepman/BackTrack/tarball/V0.4.0",
  "zipball_url": "https://api.github.com/repos/Findiepman/BackTrack/zipball/V0.4.0",
  "reactions": {"total_count": 0}
})";

std::string replaced(std::string_view text, std::string_view from, std::string_view to) {
    std::string out(text);
    const std::size_t at = out.find(from);
    CHECK(at != std::string::npos);
    if (at != std::string::npos) out.replace(at, from.size(), to);
    return out;
}

void versions() {
    CHECK(parse_version("0.4.0") == (Version{0, 4, 0}));
    CHECK(parse_version("v0.4.0") == (Version{0, 4, 0}));
    CHECK(parse_version("V0.4.0") == (Version{0, 4, 0}));
    CHECK(parse_version("1.2") == (Version{1, 2, 0}));
    CHECK(parse_version("3") == (Version{3, 0, 0}));
    CHECK(parse_version("0.10.0") == (Version{0, 10, 0}));
    CHECK(parse_version("01.002.3") == (Version{1, 2, 3}));
    CHECK(!parse_version(""));
    CHECK(!parse_version("v"));
    CHECK(!parse_version("0.4.0-beta"));
    CHECK(!parse_version("0.4.0.1"));
    CHECK(!parse_version("0.4."));
    CHECK(!parse_version(".4.0"));
    CHECK(!parse_version("0..4"));
    CHECK(!parse_version(" 0.4.0"));
    CHECK(!parse_version("0.4.0 "));
    CHECK(!parse_version("vv0.4.0"));
    CHECK(!parse_version("0.4.x"));
    CHECK(!parse_version("100000.0.0"));
    CHECK(!parse_version("99999999999999999999.0.0"));
    CHECK(parse_version("99999.0.0") == (Version{99999, 0, 0}));
    CHECK(!parse_version("-1.0.0"));

    // Numeric, not text order.
    CHECK((Version{0, 10, 0}) > (Version{0, 9, 0}));
    CHECK((Version{1, 0, 0}) > (Version{0, 99, 99}));
    CHECK((Version{0, 3, 1}) > (Version{0, 3, 0}));
    CHECK(!((Version{0, 3, 0}) > (Version{0, 3, 0})));
    CHECK_EQ((Version{0, 10, 2}).text(), std::string("0.10.2"));
}

void source_names() {
    const ReleaseSource source;
    CHECK_EQ(source.api_path(), std::string("/repos/Findiepman/BackTrack/releases/latest"));
    CHECK_EQ(source.releases_page(), std::string("https://github.com/Findiepman/BackTrack/releases"));
    CHECK_EQ(source.asset_name(Version{0, 4, 0}), std::string("Backtrack-Setup-0.4.0.exe"));
}

void a_newer_release() {
    const CheckResult r = read_latest_release(200, kRelease, kCurrent);
    CHECK(r.outcome == CheckOutcome::Available);
    CHECK(r.release.version == (Version{0, 4, 0}));
    CHECK_EQ(r.release.tag, std::string("V0.4.0"));
    CHECK_EQ(r.release.name, std::string("Backtrack 0.4.0"));
    CHECK_EQ(r.release.notes, std::string("- Updates itself\n- Faster \"Saved\" bar"));
    CHECK_EQ(r.release.page_url, std::string("https://github.com/Findiepman/BackTrack/releases/tag/V0.4.0"));
    CHECK(r.release.installer.has_value());
    if (r.release.installer) {
        CHECK_EQ(r.release.installer->name, std::string("Backtrack-Setup-0.4.0.exe"));
        CHECK_EQ(r.release.installer->size, std::int64_t{2154321});
        CHECK_EQ(r.release.installer->url,
                 std::string("https://github.com/Findiepman/BackTrack/releases/download/V0.4.0/Backtrack-Setup-0.4.0.exe"));
        CHECK_EQ(r.release.installer->sha256,
                 std::string("f756e7859d90dbb5e85aae5194206c37da220bec9bac23d3d659078a8e766fdc"));
    }
    // Lower-case "v" and a bare number are the same release.
    CHECK(read_latest_release(200, replaced(kRelease, "\"V0.4.0\"", "\"v0.4.0\""), kCurrent).outcome ==
          CheckOutcome::Available);
    CHECK(read_latest_release(200, replaced(kRelease, "\"V0.4.0\"", "\"0.4.0\""), kCurrent).outcome ==
          CheckOutcome::Available);
}

void same_or_older_is_up_to_date() {
    CHECK(read_latest_release(200, kRelease, Version{0, 4, 0}).outcome == CheckOutcome::UpToDate);
    // The newest version is reported, so a waiting newer installer can be recognised as withdrawn.
    CHECK(read_latest_release(200, kRelease, Version{0, 5, 0}).latest == (Version{0, 4, 0}));
    CHECK(read_latest_release(200, kRelease, kCurrent).latest == (Version{0, 4, 0}));
    CHECK(!read_latest_release(200, replaced(kRelease, "\"V0.4.0\"", "\"V0.4.0-fix\""), kCurrent).latest);
    CHECK(!read_latest_release(404, "", kCurrent).latest);
    CHECK(read_latest_release(200, kRelease, Version{0, 10, 0}).outcome == CheckOutcome::UpToDate);
    CHECK(read_latest_release(200, kRelease, Version{1, 0, 0}).outcome == CheckOutcome::UpToDate);
    CHECK(!read_latest_release(200, kRelease, Version{0, 4, 0}).no_releases);
    // 0.10.0 is newer than 0.9.0, which text order would get wrong.
    const std::string ten = replaced(replaced(kRelease, "\"V0.4.0\"", "\"V0.10.0\""), "\"name\": \"Backtrack-Setup-0.4.0.exe\"",
                                     "\"name\": \"Backtrack-Setup-0.10.0.exe\"");
    const CheckResult r = read_latest_release(200, ten, Version{0, 9, 0});
    CHECK(r.outcome == CheckOutcome::Available);
    CHECK(r.release.installer.has_value());
}

void odd_tags_never_offer_an_update() {
    for (const char* tag : {"\"V0.4.0-beta\"", "\"nightly\"", "\"\"", "null", "4", "\"v0.4.0.1\""}) {
        const CheckResult r = read_latest_release(200, replaced(kRelease, "\"V0.4.0\"", tag), kCurrent);
        CHECK(r.outcome == CheckOutcome::UpToDate);
    }
    CHECK(read_latest_release(200, replaced(kRelease, "\"draft\": false", "\"draft\": true"), kCurrent).outcome ==
          CheckOutcome::UpToDate);
    CHECK(read_latest_release(200, replaced(kRelease, "\"prerelease\": false", "\"prerelease\": true"), kCurrent)
              .outcome == CheckOutcome::UpToDate);
}

void no_releases_yet() {
    // What api.github.com really answers for Findiepman/BackTrack before the first release.
    constexpr std::string_view kNotFound = R"({
  "message": "Not Found",
  "documentation_url": "https://docs.github.com/rest/releases/releases#get-the-latest-release",
  "status": "404"
})";
    const CheckResult r = read_latest_release(404, kNotFound, kCurrent);
    CHECK(r.outcome == CheckOutcome::UpToDate);
    CHECK(r.no_releases);
    CHECK(r.error.empty());
    CHECK(read_latest_release(404, "", kCurrent).outcome == CheckOutcome::UpToDate);
}

void errors() {
    constexpr std::string_view kRateLimit = R"json({"message":"API rate limit exceeded for 203.0.113.7. (But here's the good news: Authenticated requests get a higher rate limit. Check out the documentation for more details.)","documentation_url":"https://docs.github.com/rest/overview/resources-in-the-rest-api#rate-limiting"})json";
    CheckResult r = read_latest_release(403, kRateLimit, kCurrent);
    CHECK(r.outcome == CheckOutcome::Failed);
    CHECK(r.rate_limited);
    CHECK(bt_test::contains(r.error, "too many update checks"));
    r = read_latest_release(403, R"({"message":"You have exceeded a secondary rate limit."})", kCurrent);
    CHECK(r.rate_limited);
    r = read_latest_release(429, "", kCurrent);
    CHECK(r.rate_limited);
    r = read_latest_release(403, R"({"message":"Repository access blocked"})", kCurrent);
    CHECK(r.outcome == CheckOutcome::Failed);
    CHECK(!r.rate_limited);
    CHECK(bt_test::contains(r.error, "HTTP 403"));
    r = read_latest_release(502, "<html>Bad gateway</html>", kCurrent);
    CHECK(r.outcome == CheckOutcome::Failed);
    CHECK(bt_test::contains(r.error, "HTTP 502"));
    // A captive portal or proxy page with status 200.
    r = read_latest_release(200, "<html><body>Log in to the Wi-Fi</body></html>", kCurrent);
    CHECK(r.outcome == CheckOutcome::Failed);
    CHECK(!r.error.empty());
    CHECK(read_latest_release(200, "", kCurrent).outcome == CheckOutcome::Failed);
    CHECK(read_latest_release(200, "[1,2]", kCurrent).outcome == CheckOutcome::Failed);
    CHECK(read_latest_release(200, "{\"tag_name\": \"V0.4", kCurrent).outcome == CheckOutcome::Failed);
}

void missing_fields() {
    // Only a tag: available, the releases page instead of the missing html_url, nothing to download.
    CheckResult r = read_latest_release(200, R"({"tag_name": "V0.4.0"})", kCurrent);
    CHECK(r.outcome == CheckOutcome::Available);
    CHECK_EQ(r.release.page_url, std::string("https://github.com/Findiepman/BackTrack/releases"));
    CHECK(r.release.notes.empty());
    CHECK(!r.release.installer);
    // body null (a release without text), assets not an array.
    r = read_latest_release(200, R"({"tag_name": "V0.4.0", "body": null, "assets": {}})", kCurrent);
    CHECK(r.outcome == CheckOutcome::Available);
    CHECK(!r.release.installer);
    // An html_url that is not on github.com is not shown.
    r = read_latest_release(200, R"({"tag_name": "V0.4.0", "html_url": "https://evil.example/x"})", kCurrent);
    CHECK_EQ(r.release.page_url, std::string("https://github.com/Findiepman/BackTrack/releases"));
    // No digest: still installable, verified by size only.
    r = read_latest_release(200,
                            replaced(kRelease, "\"sha256:F756E7859D90DBB5E85AAE5194206C37DA220BEC9BAC23D3D659078A8E766FDC\"",
                                     "null"),
                            kCurrent);
    CHECK(r.release.installer.has_value());
    if (r.release.installer) CHECK(r.release.installer->sha256.empty());
}

void installer_asset_rules() {
    // The installer of another version in the same release is not this release's installer.
    CheckResult r = read_latest_release(200, replaced(kRelease, "\"name\": \"Backtrack-Setup-0.4.0.exe\"", "\"name\": \"Backtrack-Setup-0.3.9.exe\""),
                                        kCurrent);
    CHECK(r.outcome == CheckOutcome::Available);
    CHECK(!r.release.installer);
    // Still uploading.
    r = read_latest_release(200, replaced(kRelease, "\"state\": \"uploaded\",\n      \"content_type\": \"application/x-msdownload\"",
                                          "\"state\": \"starter\",\n      \"content_type\": \"application/x-msdownload\""),
                            kCurrent);
    CHECK(!r.release.installer);
    // Size missing or zero.
    r = read_latest_release(200, replaced(kRelease, "\"size\": 2154321", "\"size\": 0"), kCurrent);
    CHECK(!r.release.installer);
    // A download URL outside this repository's releases is never followed.
    for (const char* url : {"https://evil.example/Backtrack-Setup-0.4.0.exe",
                            "http://github.com/Findiepman/BackTrack/releases/download/V0.4.0/Backtrack-Setup-0.4.0.exe",
                            "https://github.com/someone/BackTrack/releases/download/V0.4.0/Backtrack-Setup-0.4.0.exe",
                            "https://github.com@evil.example/Findiepman/BackTrack/releases/download/V0.4.0/x.exe",
                            "https://github.com/Findiepman/BackTrack/releases/download/../../../x/Backtrack-Setup-0.4.0.exe"}) {
        r = read_latest_release(
            200,
            replaced(kRelease,
                     "https://github.com/Findiepman/BackTrack/releases/download/V0.4.0/Backtrack-Setup-0.4.0.exe", url),
            kCurrent);
        CHECK(r.outcome == CheckOutcome::Available);
        CHECK(!r.release.installer);
    }
    // Owner and repository in another case are the same repository (GitHub ignores case there).
    r = read_latest_release(200, replaced(kRelease, "https://github.com/Findiepman/BackTrack/releases/download/V0.4.0/Backtrack",
                                          "https://github.com/findiepman/backtrack/releases/download/V0.4.0/Backtrack"),
                            kCurrent);
    CHECK(r.release.installer.has_value());
}

void another_repository() {
    // The update_check tool's real download test: GitHub CLI's checksum file.
    ReleaseSource cli;
    cli.owner = "cli";
    cli.repo = "cli";
    cli.asset_prefix = "gh_";
    cli.asset_suffix = "_checksums.txt";
    constexpr std::string_view kCli = R"({"tag_name":"v2.103.3","html_url":"https://github.com/cli/cli/releases/tag/v2.103.3",
      "draft":false,"prerelease":false,"assets":[{"name":"gh_2.103.3_checksums.txt","state":"uploaded","size":1971,
      "digest":"sha256:f756e7859d90dbb5e85aae5194206c37da220bec9bac23d3d659078a8e766fdc",
      "browser_download_url":"https://github.com/cli/cli/releases/download/v2.103.3/gh_2.103.3_checksums.txt"}]})";
    const CheckResult r = read_latest_release(200, kCli, Version{0, 0, 1}, cli);
    CHECK(r.outcome == CheckOutcome::Available);
    CHECK(r.release.installer.has_value());
    if (r.release.installer) CHECK_EQ(r.release.installer->size, std::int64_t{1971});
}

void digests() {
    const std::string hex = "f756e7859d90dbb5e85aae5194206c37da220bec9bac23d3d659078a8e766fdc";
    CHECK(parse_sha256_digest("sha256:" + hex) == hex);
    CHECK(parse_sha256_digest("SHA256:F756E7859D90DBB5E85AAE5194206C37DA220BEC9BAC23D3D659078A8E766FDC") == hex);
    CHECK(!parse_sha256_digest(""));
    CHECK(!parse_sha256_digest(hex));
    CHECK(!parse_sha256_digest("sha512:" + hex));
    CHECK(!parse_sha256_digest("sha256:" + hex.substr(1)));
    CHECK(!parse_sha256_digest("sha256:" + hex + "0"));
    CHECK(!parse_sha256_digest("sha256:" + hex.substr(1) + "g"));
}

void urls() {
    auto url = parse_https_url("https://GitHub.com/a/b?x=1#frag");
    CHECK(url.has_value());
    if (url) {
        CHECK_EQ(url->host, std::string("github.com"));
        CHECK_EQ(url->path, std::string("/a/b?x=1"));
    }
    url = parse_https_url("https://release-assets.githubusercontent.com:443/github-production-release-asset/1/2?sp=r&sig=a%2Fb");
    CHECK(url.has_value());
    if (url) {
        CHECK_EQ(url->host, std::string("release-assets.githubusercontent.com"));
        CHECK_EQ(url->path, std::string("/github-production-release-asset/1/2?sp=r&sig=a%2Fb"));
    }
    url = parse_https_url("https://github.com");
    CHECK(url && url->path == "/");
    url = parse_https_url("https://github.com?q");
    CHECK(url && url->path == "/?q");
    CHECK(!parse_https_url("http://github.com/x"));
    CHECK(!parse_https_url("ftp://github.com/x"));
    CHECK(!parse_https_url("https://github.com:8443/x"));
    CHECK(!parse_https_url("https://user@github.com/x"));
    CHECK(!parse_https_url("https://github.com@evil.example/x"));
    CHECK(!parse_https_url("https:///x"));
    CHECK(!parse_https_url("https://"));
    CHECK(!parse_https_url("https://git hub.com/x"));
    CHECK(!parse_https_url("https://github.com/a b"));
    CHECK(!parse_https_url("https://github.com/a\nb"));
    CHECK(!parse_https_url("//github.com/x"));
    CHECK(!parse_https_url("/relative/path"));

    CHECK(is_trusted_download_host("github.com"));
    CHECK(is_trusted_download_host("release-assets.githubusercontent.com"));
    CHECK(is_trusted_download_host("objects.githubusercontent.com"));
    CHECK(!is_trusted_download_host("api.github.com.evil.example"));
    CHECK(!is_trusted_download_host("evilgithub.com"));
    CHECK(!is_trusted_download_host("github.com.evil.example"));
    CHECK(!is_trusted_download_host("raw.githubusercontent.com"));
    CHECK(!is_trusted_download_host(""));
}

void auto_check_schedule() {
    AutoCheckInput in;
    in.now = 1'800'000'000;
    CHECK(!seconds_until_auto_check(in));  // off: only the button checks
    in.enabled = true;
    // Never checked: shortly after startup, not during it.
    CHECK(seconds_until_auto_check(in) == kAutoCheckStartDelayS);
    in.seconds_running = 30;
    CHECK(seconds_until_auto_check(in) == kAutoCheckStartDelayS - 30);
    in.seconds_running = 600;
    CHECK(seconds_until_auto_check(in) == 0);
    // Checked an hour ago: the rest of the day.
    in.last_check = in.now - 3600;
    CHECK(seconds_until_auto_check(in) == kAutoCheckIntervalS - 3600);
    // Checked two days ago (the PC was off): now, but still not in the first seconds.
    in.last_check = in.now - 2 * kAutoCheckIntervalS;
    CHECK(seconds_until_auto_check(in) == 0);
    in.seconds_running = 10;
    CHECK(seconds_until_auto_check(in) == kAutoCheckStartDelayS - 10);
    // Restarted 10 minutes after the last check: no second check that day.
    in.last_check = in.now - 600;
    CHECK(seconds_until_auto_check(in) == kAutoCheckIntervalS - 600);
    // The last one could not reach GitHub: again after an hour, not a day.
    in.seconds_running = 1000;
    in.last_check_failed = true;
    CHECK(seconds_until_auto_check(in) == kAutoCheckRetryS - 600);
    // A last check "in the future" (the clock was set back): it does not stop checks for days.
    in.last_check_failed = false;
    in.last_check = in.now + 10 * kAutoCheckIntervalS;
    CHECK(seconds_until_auto_check(in) == 0);
    // A little clock drift is normal.
    in.last_check = in.now + 60;
    CHECK(seconds_until_auto_check(in) == kAutoCheckIntervalS + 60);
    // A negative time (never written by Backtrack) is "never checked", not an overflow.
    in.last_check = std::numeric_limits<std::int64_t>::min();
    CHECK(seconds_until_auto_check(in) == 0);
}

void record_round_trip() {
    UpdaterRecord record;
    record.last_check = 1'800'000'123;
    record.last_check_failed = true;
    UpdaterRecord::Ready ready;
    ready.version = {0, 4, 0};
    ready.file = "Backtrack-Setup-0.4.0.exe";
    ready.size = 2154321;
    ready.sha256 = "f756e7859d90dbb5e85aae5194206c37da220bec9bac23d3d659078a8e766fdc";
    ready.notes = "Line one\n\"quoted\" é";
    ready.page_url = "https://github.com/Findiepman/BackTrack/releases/tag/V0.4.0";
    ready.install_attempts = 1;
    record.ready = ready;
    const UpdaterRecord back = parse_updater_record(dump_updater_record(record));
    CHECK(back.last_check == record.last_check);
    CHECK(back.last_check_failed);
    CHECK(back.ready.has_value());
    if (back.ready) {
        CHECK(back.ready->version == ready.version);
        CHECK_EQ(back.ready->file, ready.file);
        CHECK_EQ(back.ready->size, ready.size);
        CHECK_EQ(back.ready->sha256, ready.sha256);
        CHECK_EQ(back.ready->notes, ready.notes);
        CHECK_EQ(back.ready->page_url, ready.page_url);
        CHECK_EQ(back.ready->install_attempts, 1);
    }
    const UpdaterRecord empty = parse_updater_record(dump_updater_record(UpdaterRecord{}));
    CHECK(!empty.last_check);
    CHECK(!empty.ready);
}

void damaged_records() {
    CHECK(!parse_updater_record("").last_check);
    CHECK(!parse_updater_record("{not json").ready);
    CHECK(!parse_updater_record("[]").ready);
    const UpdaterRecord five = parse_updater_record(R"({"last_check": 5})");
    CHECK(five.last_check == std::int64_t{5});
    const UpdaterRecord words = parse_updater_record(R"({"last_check": "yesterday"})");
    CHECK(!words.last_check);
    const UpdaterRecord negative = parse_updater_record(R"({"last_check": -1e19})");
    CHECK(!negative.last_check);
    const std::string good_hash = "f756e7859d90dbb5e85aae5194206c37da220bec9bac23d3d659078a8e766fdc";
    const auto with_ready = [&](std::string_view version, std::string_view file, std::string_view sha, int size) {
        return parse_updater_record(std::format(R"({{"ready": {{"version": "{}", "file": "{}", "sha256": "{}", "size": {}}}}})",
                                                version, file, sha, size));
    };
    CHECK(with_ready("0.4.0", "Backtrack-Setup-0.4.0.exe", good_hash, 10).ready.has_value());
    // A file name that is not the asset name could point anywhere: "..\\..\\x.exe".
    CHECK(!with_ready("0.4.0", "..\\\\..\\\\evil.exe", good_hash, 10).ready);
    CHECK(!with_ready("0.4.0", "Backtrack-Setup-0.5.0.exe", good_hash, 10).ready);
    CHECK(!with_ready("0.4.0", "Backtrack-Setup-0.4.0.exe", "abc", 10).ready);
    CHECK(!with_ready("0.4.0", "Backtrack-Setup-0.4.0.exe", good_hash, 0).ready);
    CHECK(!with_ready("junk", "Backtrack-Setup-junk.exe", good_hash, 10).ready);
    const UpdaterRecord odd = parse_updater_record(
        std::format(R"({{"ready": {{"version": "0.4.0", "file": "Backtrack-Setup-0.4.0.exe", "sha256": "{}", "size": 3,
                    "install_attempts": -7}}}})",
                    good_hash));
    CHECK(odd.ready && odd.ready->install_attempts == 0);
}

}  // namespace

int main() {
    versions();
    source_names();
    a_newer_release();
    same_or_older_is_up_to_date();
    odd_tags_never_offer_an_update();
    no_releases_yet();
    errors();
    missing_fields();
    installer_asset_rules();
    another_repository();
    digests();
    urls();
    auto_check_schedule();
    record_round_trip();
    damaged_records();
    return bt_test::finish("UpdateCheckTests");
}
