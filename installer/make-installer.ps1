# Builds Backtrack (build.bat: Release + tests) and then build\installer\Backtrack-Setup-<version>.exe
# with Inno Setup 6 (winget install JRSoftware.InnoSetup). Only the developer needs Inno Setup;
# the setup file runs on any Windows 10 1903+ / 11 PC as is.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$version = (Select-String -Path "$root\CMakeLists.txt" -Pattern 'project\(Backtrack VERSION ([0-9.]+)').Matches[0].Groups[1].Value
if (-not $version) { throw "No version found in CMakeLists.txt" }

& cmd /c "`"$root\build.bat`""
if ($LASTEXITCODE -ne 0) { throw "The build or the tests failed; no installer made" }

$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
          "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "Inno Setup 6 is not installed: winget install JRSoftware.InnoSetup" }

& $iscc "/DAppVersion=$version" "$root\installer\backtrack.iss"
if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed" }
Write-Host "Made build\installer\Backtrack-Setup-$version.exe"
