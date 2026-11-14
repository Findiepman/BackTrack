#!/usr/bin/env bash
# Packs a Linux build into Backtrack-<version>-x86_64.AppImage with linuxdeploy and its Qt plugin.
# Made on Ubuntu 24.04 (as CI does), so it runs on every distribution with glibc 2.39 or newer:
# Arch, Kubuntu/KDE neon 24.04+, Fedora 40+, openSUSE Tumbleweed.
#
#   cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release -DBT_LINUX_APP=ON
#   cmake --build build-linux --parallel
#   packaging/linux/make-appimage.sh build-linux
#
# Not bundled, taken from the user's system: the sound server (libpulse), PipeWire, VA-API and
# the graphics driver (their versions must match the running daemons and driver), plus the usual
# AppImage exclusions (glibc, X11, OpenGL, fontconfig). Every KDE Plasma desktop has them.
set -euo pipefail

build=${1:-build-linux}
root=$(cd "$(dirname "$0")/../.." && pwd)
version=$(sed -nE 's/.*project\(Backtrack VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$root/CMakeLists.txt")
[ -n "$version" ] || { echo "No version in CMakeLists.txt"; exit 1; }
[ -x "$build/backtrack" ] || { echo "$build/backtrack is missing: build with -DBT_LINUX_APP=ON first"; exit 1; }
build=$(cd "$build" && pwd)

tools=$build/appimage-tools
mkdir -p "$tools"
fetch() {
    if [ ! -x "$tools/$1" ]; then
        curl -fsSL --retry 3 -o "$tools/$1" "$2"
        chmod +x "$tools/$1"
    fi
}
fetch linuxdeploy-x86_64.AppImage \
    https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
fetch linuxdeploy-plugin-qt-x86_64.AppImage \
    https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
fetch linuxdeploy-plugin-appimage-x86_64.AppImage \
    https://github.com/linuxdeploy/linuxdeploy-plugin-appimage/releases/download/continuous/linuxdeploy-plugin-appimage-x86_64.AppImage

appdir=$build/AppDir
rm -rf "$appdir"
DESTDIR=$appdir cmake --install "$build" --prefix /usr
install -Dm644 "$root/LICENSE" "$appdir/usr/share/doc/backtrack/LICENSE"
"$build/backtrack" --write-icon "$build/backtrack.png"

output="Backtrack-$version-x86_64.AppImage"
rm -f "$build/$output"
export APPIMAGE_EXTRACT_AND_RUN=1  # no FUSE needed for the tools themselves
export PATH="$tools:$PATH"
export QMAKE=${QMAKE:-$(command -v qmake6 || echo /usr/lib/qt6/bin/qmake)}
export OUTPUT=$output LDAI_OUTPUT=$output LINUXDEPLOY_OUTPUT_VERSION=$version
cd "$build"
linuxdeploy-x86_64.AppImage --appdir "$appdir" \
    --executable "$appdir/usr/bin/backtrack" \
    --desktop-file "$appdir/usr/share/applications/backtrack.desktop" \
    --icon-file "$build/backtrack.png" \
    --exclude-library 'libpulse*' --exclude-library 'libpipewire*' --exclude-library 'libspa*' \
    --exclude-library 'libva*' --exclude-library 'libvdpau*' --exclude-library 'libdrm*' \
    --exclude-library 'libcuda*' --exclude-library 'libnvidia*' \
    --plugin qt --output appimage
[ -f "$build/$output" ] || { echo "$output was not made"; exit 1; }
echo "Made $build/$output ($(stat -c %s "$build/$output") bytes)"
