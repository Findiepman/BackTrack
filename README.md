# Backtrack

A replay buffer for Windows. Backtrack keeps the last 30 seconds (or however many you choose) of
your screen and sound in memory, and saves them to an `.mp4` the moment you press a hotkey.
Nothing touches the disk until you press it.

Built from scratch on what Windows ships with: Desktop Duplication and Direct3D 11 for the
screen, the graphics card's own H.264 encoder through Media Foundation (AMD, NVIDIA or Intel, with
Windows' software encoder as fallback), WASAPI for sound. No FFmpeg, no OBS, no runtime to install.

> **Status:** the recording engine and the clip library window are done. Still to come: the clip
> page (player, trim), the Record and Settings screens. Until the Settings screen exists, settings
> are in `backtrack.ini`.

## Using it

1. Run `backtrack.exe`. The Backtrack window opens with your clip library; Instant Replay starts
   at once (the pill at the top right says `Replay on, 30 s`).
2. Play. Press **F8** (anywhere, also in a game) to save the last 30 seconds. You hear the Windows
   "asterisk" sound, the clip appears at the top of the library with a "Saved" bar, and the file is
   in `Videos\Backtrack`. When the window is not in front, Windows shows a small "Saved" message.
3. In the library:
   - Click a clip to play it (in your default player for now). Right-click it to show it in its
     folder, favorite it, rename it, add it to a collection or move it to the Trash.
   - The star on a clip makes it a favorite. Favorites and clips in a collection are never removed
     automatically.
   - **+** next to Collections makes a new collection; right-click a collection to rename or delete
     it (deleting a collection keeps its clips).
   - The search box at the top (Ctrl+K) searches titles, file names, tags and notes.
   - Trash: clips you delete wait there for 30 days, then go to the Windows Recycle Bin. The file
     stays in the clips folder until then. Backtrack never deletes anything for good: on a drive
     without a Recycle Bin (USB stick, network share) the clip simply stays where it is.
4. Clicking the window's **X** hides Backtrack to the tray (the icon next to the clock); Instant
   Replay keeps running. Click the tray icon to open the window again; right-click it to save a
   clip, pause, open the clips folder, edit the settings, open the log or **Quit**. Quitting waits
   for a clip that is still being saved.

Only one Backtrack runs at a time. Starting it again brings the running window to the front.

Next to `backtrack.exe` Backtrack keeps `library.json` (favorites, titles, collections, trash) and a
`cache` folder with thumbnails (safe to delete; they are made again). If that folder is read-only
(for example under `C:\Program Files`), they and `backtrack.log` go to `%LOCALAPPDATA%\Backtrack` instead. If
`library.json` is ever damaged, a copy is kept as `library.damaged-<date-time>.json` and the
library is rebuilt from the clips folder; a file written by a newer Backtrack is kept as
`library.newer-<date-time>.json` before it is saved again. When the clips folder cannot be reached
(an unplugged drive), the library is left exactly as it was. If `library.json` cannot be read at
all (locked by another program), the window says so and changes are not saved that run, so the
file is never overwritten.

### Things to know

- **Exclusive fullscreen games cannot be recorded.** Windows Desktop Duplication cannot see them.
  Set games to **borderless windowed** (often called "windowed fullscreen"); it looks the same.
- A clip is `seconds` long or up to one second longer: it has to start on a keyframe, and there is
  one every second.
- Changing the screen resolution (or a game doing it) does not lose your clip: recording carries on
  at the size it started with, with the new picture scaled in (black bars if the shape differs).
  Restart Backtrack to record at the new size.
- The only time the buffer starts over is when the graphics driver restarts (a driver update or
  crash): the next clip then begins at that moment.
- Pausing keeps what was recorded before the pause. A clip saved after resuming joins the time
  before and after the pause without a gap.
- Sound is what you hear on the default playback device. Switching the default device (plugging in
  a headset) is picked up automatically. Bluetooth headphones are fine: their playback delay is
  taken out, so sound and picture stay in sync in the clip.
- The "saved" sound (and the error sound when a save is refused) plays on your speakers, so the
  next clip that overlaps that moment contains it, like anything else you hear. `sound = false`
  turns it off.
- Settings and the log live next to `backtrack.exe`, so put it in a folder you can write to
  (not `C:\Program Files`).
- Windows "N" editions need the free Media Feature Pack from Microsoft for the video encoder.

## Settings (`backtrack.ini`)

Written with the defaults the first time Backtrack starts. Restart Backtrack after editing. A
missing key uses its default; a value that cannot be used is logged with its line number and the
default is kept. Backtrack never refuses to start because of the settings.

### `[clip]`

| Key | Default | Meaning |
|---|---|---|
| `seconds` | `30` | How many seconds to keep and save. 5 to 300. At 20 Mbit/s, 30 s is about 75 MB of memory. |
| `output_dir` | `%USERPROFILE%\Videos\Backtrack` | Where clips go. Environment variables work; a relative path is taken from the folder `backtrack.exe` is in. Created if missing; if it cannot be created, clips go to your Videos folder. |
| `filename` | `clip_%Y-%m-%d_%H-%M-%S.mp4` | File name. Allowed placeholders: `%Y %y %m %d %H %I %M %S %p %j %a %A %b %B %U %W %w %%`. Two clips in the same second get `_2`, `_3`. |

To use `;` or `#` in a path, put the whole value in double quotes: `output_dir = "D:\Clips #2"`.

### `[hotkey]`

| Key | Default | Meaning |
|---|---|---|
| `save` | `F8` | The save hotkey, e.g. `Ctrl+Alt+F8` or `Ctrl+Shift+F12`. Modifiers: Ctrl, Alt, Shift, Win. Keys: A-Z, 0-9, F1-F24, Numpad0-9, Home, End, Insert, Delete, PageUp, PageDown, Space, Pause, PrintScreen. |

Refused (Backtrack falls back to F8 and says why in the log): letters, digits or Space without a
modifier (they would stop working in every other program), F12 on its own (reserved by Windows),
Ctrl+Pause (the keyboard sends Break instead), Shift+Numpad keys (NumLock turns them into arrow
keys). PrintScreen on its own is allowed, but Windows 11 gives it to the Snipping Tool unless that
is turned off in Settings > Accessibility > Keyboard. If another program already uses the hotkey,
the sidebar says the key is taken, the log says why, and the "Save last 30 s" button in the window
and the tray menu still work.

### `[video]`

| Key | Default | Meaning |
|---|---|---|
| `monitor` | `0` | Which monitor to record. 0 is the primary monitor, then the others in Windows' order (the log lists them at startup). A number that does not exist falls back to 0. |
| `fps` | `60` | 30, 60, 120 or 144. Other numbers are rounded to the nearest. |
| `bitrate_kbps` | `20000` | Video quality in kbit/s, 1000 to 100000. 20000 suits 1080p60. |
| `encoder` | `auto` | `auto` uses the graphics card and falls back to software; `hardware` is the same but logs an error when it has to fall back; `software` always uses Windows' software encoder (high CPU use). |

### `[audio]`

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `true` | Record what you hear. `false` saves clips without sound. |
| `bitrate_kbps` | `160` | Sound quality: 96, 128, 160 or 192. Others are rounded to the nearest. |

### `[feedback]`

| Key | Default | Meaning |
|---|---|---|
| `sound` | `true` | Play the Windows "asterisk" sound when a clip is saved. |
| `notification` | `true` | Show a Windows message when a clip is saved while the Backtrack window is not in front. |

### `[logging]`

| Key | Default | Meaning |
|---|---|---|
| `level` | `info` | `error`, `info` or `debug`. `error` still shows warnings (why a hotkey or a save did not work). `debug` adds counters once a minute: frames, drops, sound corrections, buffer size and memory use. |

The log is `backtrack.log` next to the exe. It rolls over at 5 MB and keeps three files.

## Building

Needs Visual Studio 2022 (or its Build Tools) with the C++ workload, and CMake 3.25 or newer
(`winget install Kitware.CMake`). 64-bit only.

```
build.bat
```

This builds `build\Release\backtrack.exe`, puts a default `backtrack.ini` next to it and runs the
unit tests. The exe uses the static C runtime and only Windows system DLLs, so it can be copied to
any Windows 10 (1903 or newer) or Windows 11 PC as is.

To run only the unit tests (no GPU, no game, no Media Foundation needed):

```
tests\run-tests.bat
```

### Test options

`backtrack.exe` also takes options for automated testing: `--save-after <s>` (repeatable),
`--pause-after <s>`, `--resume-after <s>`, `--lose-capture-every <s>` (pretends Windows took the
screen away, as a UAC prompt does), `--exit-after <s>` and `--hidden` (start in the tray).

### Checking sound/picture sync

`build\Release\sync_beacon.exe <seconds>` shows a square in the top-left corner of the primary
monitor that flashes white exactly when a short beep plays. Record it, save a clip, then run
`tools\check_sync.ps1 <clip.mp4>` (needs ffmpeg on PATH, only for this check). It prints the
offset between each flash and beep in the clip.

## How it works

Screen frames are captured on a fixed tick (a static screen still produces every frame), converted
to NV12 on the GPU and encoded to H.264 with one keyframe per second and no B-frames. Sound is
captured from the default playback device, resampled to 48 kHz and encoded to AAC. Both streams of
compressed packets go into ring buffers in memory that always start on a keyframe. Saving copies
the last `seconds` out of the buffers in a few milliseconds and writes them to an MP4 on a separate
thread without re-encoding, under a `.tmp` name that is renamed when complete. Every timestamp
comes from one clock, so sound and picture stay in sync however long Backtrack runs.

Code layout: `src/core` is plain C++ with unit tests (settings, hotkeys, ring buffer, clip
planning, the clip library and its JSON file); `src/platform` is everything that talks to Windows;
`src/platform/Recorder.h` is the engine the window drives; `src/ui` is the window, drawn with
Direct2D and DirectWrite (part of Windows, nothing to install).
