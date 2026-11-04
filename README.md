# Backtrack

A replay buffer for Windows. Backtrack keeps the last 30 seconds (or however many you choose) of
your screen and sound in memory, and saves them to an `.mp4` the moment you press a hotkey.
Nothing touches the disk until you press it.

Built from scratch on what Windows ships with: Desktop Duplication and Direct3D 11 for the
screen, the graphics card's own H.264 encoder through Media Foundation (AMD, NVIDIA or Intel, with
Windows' software encoder as fallback), WASAPI for sound. No FFmpeg, no OBS, no runtime to install.

> **Status:** the recording engine is complete. The real user interface is being designed;
> until it exists, Backtrack runs in a small console window (see below).

## Using it

1. Run `backtrack.exe`. A console window opens and shows what Backtrack is doing; its title bar
   shows the state, for example `Backtrack - Recording 30 s, 60 fps, hardware`.
2. Play. Press **F8** (anywhere, also in a game) to save the last 30 seconds. You hear the Windows
   "asterisk" sound and the file appears in `Videos\Backtrack`.
3. Keys in the console window:

   | Key | Does |
   |---|---|
   | S | save a clip (same as the hotkey) |
   | P | pause / resume recording |
   | O | open the clips folder |
   | E | open the settings file |
   | L | open the log |
   | Q | quit (waits for a clip that is still being saved) |

Only one Backtrack runs at a time; starting it again just says it is already running.

### Things to know

- **Exclusive fullscreen games cannot be recorded.** Windows Desktop Duplication cannot see them.
  Set games to **borderless windowed** (often called "windowed fullscreen"); it looks the same.
- A clip is `seconds` long or up to one second longer: it has to start on a keyframe, and there is
  one every second.
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
| `output_dir` | `%USERPROFILE%\Videos\Backtrack` | Where clips go. Environment variables work. Created if missing; if it cannot be created, clips go to your Videos folder. |
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
the log says so and the S key still works.

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
| `notification` | `true` | Reserved for the user interface; the console version shows saves in the window and the log. |

### `[logging]`

| Key | Default | Meaning |
|---|---|---|
| `level` | `info` | `error`, `info` or `debug`. `debug` adds counters once a minute: frames, drops, sound corrections, buffer size and memory use. |

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
screen away, as a UAC prompt does) and `--exit-after <s>`.

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
planning); `src/platform` is everything that talks to Windows; `src/platform/Recorder.h` is the
engine the user interface drives.
