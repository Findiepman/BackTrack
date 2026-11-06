# Publishes Backtrack <version> on GitHub: builds build\installer\Backtrack-Setup-<version>.exe
# (installer\make-installer.ps1: Release build, tests, Inno Setup) and creates the release
# V<version> with that file attached. Every installed Backtrack with "Install updates
# automatically" on downloads it within a day, so this is the owner's decision: run it by hand.
#
#   powershell -ExecutionPolicy Bypass -File tools\publish-release.ps1 -DryRun
#   powershell -ExecutionPolicy Bypass -File tools\publish-release.ps1
#   powershell -ExecutionPolicy Bypass -File tools\publish-release.ps1 -Notes "- Faster saving`n- Fixed X"
#
# -Notes: the release text shown in Backtrack's Settings page. Without it: the commit subjects
# since the last V* tag (subjects starting with "CI:" left out).
# Needs the GitHub CLI, signed in: winget install GitHub.cli, then gh auth login.
param(
    [string]$Notes = "",
    [switch]$DryRun
)
# Not "Stop": Windows PowerShell turns a git or gh message on stderr (an expected "no such tag")
# into a terminating error. Every step checks its own result instead.
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
$repo = "Findiepman/BackTrack"

function Step([string]$text) { Write-Host "-> $text" }
# A dry run reports every problem and carries on, so one run shows all of them.
function Stop([string]$text) { if ($DryRun) { Write-Warning $text } else { throw $text } }

$version = (Select-String -Path "$root\CMakeLists.txt" -Pattern 'project\(Backtrack VERSION ([0-9]+\.[0-9]+\.[0-9]+)').Matches[0].Groups[1].Value
if (-not $version) { throw "No version found in CMakeLists.txt" }
$tag = "V$version"
# The updater picks exactly this name out of the release (core/UpdateCheck.cpp, ReleaseSource).
$setupName = "Backtrack-Setup-$version.exe"
$setup = "$root\build\installer\$setupName"
Step "Version $version, tag $tag, asset $setupName"

Push-Location $root
try {
    # Released code must be the code on GitHub: the tag points at the pushed commit.
    $dirty = git status --porcelain  # a new source file not committed would be built but not tagged
    if ($dirty) { Stop "There are uncommitted changes; commit or stash them first.`n$dirty" }
    $branch = git rev-parse --abbrev-ref HEAD
    # Whatever is released becomes "latest" for every installed copy.
    if ($branch -ne "main") { Stop "Not on main but on '$branch'; releases are made from main." }
    git fetch --quiet --tags origin
    if ($LASTEXITCODE -ne 0) { Stop "git fetch failed (no network?); the checks below need GitHub." }
    $head = git rev-parse HEAD
    $remote = git rev-parse "origin/$branch" 2>$null
    if ($LASTEXITCODE -ne 0 -or $head -ne $remote) { Stop "HEAD $head is not pushed to origin/$branch; push first." }

    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { Stop "The GitHub CLI is not installed: winget install GitHub.cli" }
    & gh auth status *> $null
    if ($LASTEXITCODE -ne 0) { Stop "The GitHub CLI is not signed in: gh auth login" }
    & gh release view $tag --repo $repo *> $null
    if ($LASTEXITCODE -eq 0) { Stop "Release $tag already exists. Bump the version in CMakeLists.txt; never reuse one." }
    # An existing tag would be reused by gh as it is, whatever commit it points at.
    $remoteTag = git ls-remote --tags origin "refs/tags/$tag"
    if ($LASTEXITCODE -ne 0) { Stop "Could not list the tags on GitHub." }
    elseif ($remoteTag) { Stop "Tag $tag already exists on GitHub (without a release); bump the version." }
    # Installed copies only move up: an equal or lower "latest" would make them drop a waiting update.
    $latest = & gh release view --repo $repo --json tagName --jq .tagName 2>$null
    if ($LASTEXITCODE -eq 0 -and $latest) {
        $latestVersion = $null
        try { $latestVersion = [version]($latest -replace '^[vV]', '') } catch { }
        if ($null -eq $latestVersion) { Stop "The latest release's tag $latest is not a version Backtrack can read." }
        elseif ([version]$version -le $latestVersion) { Stop "$version is not newer than the latest release $latest." }
        Step "Latest release now: $latest"
    } else {
        Step "No release on GitHub yet: this is the first"
    }

    if (-not $Notes) {
        $last = git describe --tags --abbrev=0 --match "V*" 2>$null
        $range = if ($LASTEXITCODE -eq 0 -and $last) { "$last..HEAD" } else { "HEAD" }
        $subjects = git log --no-merges --format=%s $range | Where-Object { $_ -and -not $_.StartsWith("CI:") }
        $Notes = ($subjects | ForEach-Object { "- $_" }) -join "`n"
        Step "Release notes from the commits in $range"
    }
    if (-not $Notes) { throw "No release notes: pass -Notes" }
    Write-Host "----- release notes -----"
    Write-Host $Notes
    Write-Host "-------------------------"

    if ($DryRun) {
        Step "Would run: installer\make-installer.ps1 (build, tests, $setupName)"
        Step "Would run: gh release create $tag `"$setup`" --repo $repo --target $head --title `"Backtrack $version`" --notes-file <the notes above>"
        Step "Dry run: nothing was built or published."
        return
    }

    & powershell -ExecutionPolicy Bypass -File "$root\installer\make-installer.ps1"
    if ($LASTEXITCODE -ne 0) { throw "Building the installer failed; nothing was published" }
    if (-not (Test-Path $setup)) { throw "$setup was not made" }
    $hash = (Get-FileHash -Algorithm SHA256 $setup).Hash.ToLower()
    Step "$setupName, $((Get-Item $setup).Length) bytes, SHA-256 $hash"

    $notesFile = New-TemporaryFile
    try {
        [IO.File]::WriteAllText($notesFile.FullName, $Notes, (New-Object System.Text.UTF8Encoding $false))
        & gh release create $tag $setup --repo $repo --target $head --title "Backtrack $version" --notes-file $notesFile.FullName
        if ($LASTEXITCODE -ne 0) { throw "gh release create failed" }
    } finally {
        Remove-Item $notesFile.FullName -ErrorAction SilentlyContinue
    }
    # GitHub adds a "digest" (sha256) to the asset; installed copies check the download against it.
    $digest = & gh api "repos/$repo/releases/tags/$tag" --jq ".assets[] | select(.name == `"$setupName`") | .digest"
    if ($digest -and $digest -ne "sha256:$hash") { Write-Warning "GitHub's digest $digest differs from the local file's sha256:$hash" }
    Step "Published https://github.com/$repo/releases/tag/$tag (GitHub digest: $digest)"
} finally {
    Pop-Location
}
