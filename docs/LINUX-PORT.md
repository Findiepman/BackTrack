# Backtrack on Linux (KDE Plasma, Arch)

Status on 2026-11-14 (version 0.6.0): **the first Linux version builds and passes its automatic
test on GitHub Actions; it has not run on a real Linux desktop yet.** It was written on a Windows
PC without Linux. On the `linux-port` branch it builds with GCC and Clang on Ubuntu 24.04 and with
GCC on Arch, and in the test it records a virtual screen (X11) with a virtual sound card and saves
three working clips (H.264 through x264, AAC sound): as a plain build, packed into the AppImage on
Ubuntu, and with that AppImage on a bare Arch Linux. No release gets an AppImage unless all of
that passes. Wayland, the graphics card encoders, the tray and the hotkey are not covered by that
test (see "Not tested yet").

## What the Linux version does

- Keeps the last 5 to 300 seconds of the screen and sound and saves them as MP4 with a hotkey
  (F8 by default), from the tray, with the Save clip button, or with `backtrack --save`.
- **Screen:** X11 sessions through XShm (one monitor, the `monitor` setting), Wayland sessions
  (Plasma's default) through the screen sharing portal and PipeWire. On Wayland KDE asks once which
  screen to share; the answer is remembered (`~/.local/state/Backtrack/screencast-token`), so it is
  not asked again after a relog. The mouse pointer is drawn in (`capture_cursor`).
- **Video:** H.264 on the graphics card (NVENC on NVIDIA, VA-API on AMD and Intel), else x264 on
  the processor (`encoder = auto/hardware/software`), the `fps`, `video_bitrate_kbps`,
  `resolution` and `quality` settings, a keyframe every second.
- **Sound:** what you hear (the default output) and the default microphone in one AAC track, with
  the same mixer and noise suppression as Windows (`audio_enabled`, `mic_enabled`,
  `system_volume`, `mic_volume`, `noise_suppression`, `audio_bitrate_kbps`). Through the
  PulseAudio API, which PipeWire provides on KDE and Arch (`pipewire-pulse`).
- **Hotkey:** X11: grabbed for the whole desktop, also in games. Wayland: registered with KDE
  through the GlobalShortcuts portal (Plasma 6 asks once to confirm the key; it can be changed in
  System Settings > Shortcuts > Backtrack). If the desktop refuses, the window shows the command to
  bind yourself (`backtrack --save`).
- **Window:** status, Save clip, Start/Stop recording, the 30 newest clips (double-click plays),
  open the clips folder, and the main settings (clip length, frame rate, bitrate, encoder,
  resolution, sound, microphone, noise suppression, hotkey, clips folder). Closing it keeps
  recording in the tray; Quit is in the tray menu.
- **Files:** settings in `~/.config/Backtrack/backtrack.ini` (the same keys as on Windows; a copied
  Windows file works), clips in `~/Videos/Backtrack` (or the XDG videos folder), the log in
  `~/.local/state/Backtrack/backtrack.log`. The AppImage adds itself to the application menu.

## What it does not do yet

- The library (favorites, collections, trash, search), the clip page (trim, filmstrip), export,
  share, repair, bookmarks, the overlay, themes, separate sound tracks, choosing a sound device,
  push-to-talk and the mic mute key, storage limits and auto-delete, start with the desktop,
  automatic updates. The settings keys for them are read but have no effect on Linux.
- Wayland: no DMA-BUF (the compositor copies the screen into memory: one extra copy per frame,
  fine at 1080p/60, more processor time at 1440p/144). No overlay on top of games (Wayland does not
  allow it): the tray notification says when a clip is saved.
- A second monitor change while recording on X11 (the size is taken at start; Stop and Start).

## Not tested yet (needs a real KDE PC)

Tested automatically in CI on every push: X11 capture, x264, sound through PulseAudio, saving,
`backtrack --save`, the AppImage on Ubuntu 24.04 and on Arch. **Not tested anywhere yet:**

- the Wayland path: the portal dialog, the restore token, PipeWire frames from KWin, the
  GlobalShortcuts portal on Plasma 6;
- NVENC and VA-API on real graphics cards (they fall back to x264 if they fail);
- `@DEFAULT_MONITOR@` on PipeWire's PulseAudio server (works on PulseAudio itself);
- the tray icon and notifications on Plasma.

A first check on the boss's KDE PC: start the AppImage on Wayland, accept the screen and the F8
shortcut, play something with sound, press F8, play the clip. Then the same on an X11 session.

## Using the AppImage

1. Download `Backtrack-<version>-x86_64.AppImage` from the release.
2. `chmod +x Backtrack-*.AppImage` (or Properties > Permissions > Executable in Dolphin).
3. Start it. On Arch, if it says FUSE is missing: `sudo pacman -S fuse2`, or start it with
   `--appimage-extract-and-run`.

It needs glibc 2.39 or newer (Arch, Kubuntu/KDE neon 24.04+, Fedora 40+, openSUSE Tumbleweed) and
takes the sound server, PipeWire, VA-API and the graphics driver from the system. The window uses
Qt's own style (not Breeze) and runs through XWayland on a Wayland session; recording and the
hotkey still use the Wayland portal.

The AppImage bundles FFmpeg with x264, which is GPL: the AppImage as a whole is distributed under
the GPL's terms. Backtrack's own code stays MIT and its source is public, which is what the GPL
asks for.

## Building

Ubuntu 24.04 (Arch: `pacman -S base-devel cmake qt6-base ffmpeg libpulse libpipewire libx11
libxext libxfixes libxrandr`):

```
sudo apt install build-essential cmake qt6-base-dev libavcodec-dev libavformat-dev libavutil-dev \
  libswscale-dev libpulse-dev libpipewire-0.3-dev libx11-dev libxext-dev libxfixes-dev libxrandr-dev
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release -DBT_LINUX_APP=ON
cmake --build build-linux --parallel
ctest --test-dir build-linux --output-on-failure
./build-linux/backtrack
```

Without `-DBT_LINUX_APP=ON` only the portable core and its unit tests build (no libraries needed).

## Building the AppImage

On Ubuntu 24.04, after the build above:

```
bash packaging/linux/make-appimage.sh build-linux
# -> build-linux/Backtrack-<version>-x86_64.AppImage
bash packaging/linux/smoke-test.sh run "$PWD/build-linux/Backtrack-<version>-x86_64.AppImage"
bash packaging/linux/smoke-test.sh check
```

The smoke test needs `xvfb pulseaudio pulseaudio-utils ffmpeg`. CI (`.github/workflows/build.yml`)
does exactly this in the `appimage` job, runs the AppImage again on Arch (`appimage-arch`), and the
`release` job attaches it next to `Backtrack-Setup-<version>.exe`. The Windows updater only takes
the asset named `Backtrack-Setup-<version>.exe` (a unit test checks this with an AppImage in the
release), so the extra file does not change Windows updates. The Linux version does not update
itself: download the new AppImage.

## How the code is laid out

| Part | Windows | Linux |
|---|---|---|
| Settings, hotkey parsing, replay buffer, clip plan and names, mixer, noise suppression | `src/core` | the same `src/core` |
| Linux-only pure logic (key codes, portal paths, clips folder) | | `src/core/LinuxDesktop` (unit tested on both) |
| Screen capture | `platform/ScreenCapture` (Desktop Duplication) | `linux/X11Capture`, `linux/PipeWireCapture` + `linux/Portal` |
| Video encoder | `platform/VideoEncoder` (Media Foundation) | `linux/VideoEncoder` (FFmpeg) |
| Sound | `platform/AudioCapture`, `MicCapture` (WASAPI) | `linux/AudioRecorder` (PulseAudio API) |
| Writing clips | `platform/ClipWriter` | `linux/ClipFile` (FFmpeg's MP4 muxer) |
| Recorder | `platform/Recorder` | `linux/Recorder` |
| Window, tray, hotkeys | `ui/*` (Win32, Direct2D) | `linux/MainWindow` (Qt 6), `linux/X11Hotkey`, `linux/Portal` |

## Next steps

1. Run it on the boss's KDE PC (the list above) and fix what the real desktop shows.
2. The library and clip page in Qt (the biggest part of the Windows app).
3. Export, share and repair (FFmpeg makes these simpler than on Windows).
4. Start with the desktop (`~/.config/autostart`), updates (download the new AppImage).
