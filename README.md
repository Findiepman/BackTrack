# Backtrack

A replay buffer for Windows. Backtrack keeps the last 30 seconds (or however many you choose) of
your screen, sound and microphone in memory, and saves them to an `.mp4` the moment you press a hotkey.
Nothing touches the disk until you press it.

Built from scratch on what Windows ships with: Desktop Duplication and Direct3D 11 for the
screen, the graphics card's own H.264 encoder through Media Foundation (AMD, NVIDIA or Intel, with
Windows' software encoder as fallback), WASAPI for sound. No FFmpeg, no OBS, no runtime to install.

> **Status:** the recording engine and the whole window are done: library, clip page (player, trim,
> tags, notes), Record and Settings, and the microphone. Sharing, long recordings and screenshots are
> shown in the window as "Soon".
>
> **Microphone:** recorded by default and mixed into the clip's sound track with what you hear.
> Mute it with **Alt+F7** (two short tones say muted / on), from the tray menu or the Record
> page, or turn on push-to-talk (hold **V**, or another key or a mouse button you pick). Noise
> suppression takes out fans and hum. All of it is under Settings > Audio and Hotkeys.

## Using it

1. Run `backtrack.exe`. The Backtrack window opens with your clip library; Instant Replay starts
   at once (the pill at the top right says `Replay on, 30 s`).
2. Play. Press **F8** (anywhere, also in a game) to save the last 30 seconds. You hear the Windows
   "asterisk" sound, the clip appears at the top of the library with a "Saved" bar, and the file is
   in `Videos\Backtrack`. When the window is not in front, Windows shows a small "Saved" message.
3. In the library:
   - Click a clip to open its page: play it (Space plays and pauses, the arrow keys jump 5 s), trim it,
     give it a title, tags and notes. Right-click it to play it in your own video player, show it
     in its folder, favorite it, rename it, add it to a collection or move it to the Trash.
   - **Trim:** drag the two handles under the player (or play to a moment and press **Start here** /
     **End here**, or the keys I and O), then **Save as new clip**. The original stays as it is; the
     new clip is saved next to it as `<name>_trim.mp4` in a fraction of a second, without losing
     any quality. Because nothing is re-encoded, the new clip starts on the keyframe at or before
     the start handle: up to one second earlier. The end is exact.
   - The star on a clip makes it a favorite. Favorites and clips in a collection are never removed
     automatically.
   - **+** next to Collections makes a new collection; right-click a collection to rename or delete
     it (deleting a collection keeps its clips).
   - The search box at the top (Ctrl+K) searches titles, file names, tags and notes.
   - Trash: clips you delete wait there for 30 days, then go to the Windows Recycle Bin. The file
     stays in the clips folder until then. Backtrack never deletes anything for good: on a drive
     without a Recycle Bin (USB stick, network share) the clip simply stays where it is.
4. **Record** shows which display Instant Replay records (click another to switch), how full the
   replay buffer is, Backtrack's own CPU and memory use and your latest clips.
5. **Settings** changes everything below while Backtrack runs; changes are saved to `backtrack.ini`
   at once (your comments in that file are kept). Changing the display, frame rate, bitrate,
   encoder, buffer length, recording game sound or its quality, recording the microphone or which
   microphone restarts Instant Replay about a second later (the clips folder: at once): the seconds it was holding are lost, nothing else. If
   it cannot start with the new values, it goes back to the previous ones and says so. Changing the clips folder asks whether to move
   your clips there (with Windows' own progress window) or leave them; either way a copy of the
   library is kept as `library.before-folder-change-<date-time>.json`.
6. Clicking the window's **X** hides Backtrack to the tray (the icon next to the clock); Instant
   Replay keeps running. Click the tray icon to open the window again; right-click it to save a
   clip, pause, mute the microphone, open the clips folder, open Settings, open the log or **Quit**. Quitting waits
   for a clip that is still being saved.

Backtrack puts itself in the Start menu the first time it runs, so you can press the Windows key and
type "backtrack" to start it. If you move `backtrack.exe`, the Start menu entry follows it the next
time you start it from its new place.

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
- The microphone is mixed into the same sound track. Your voice is in the clip about 50 ms after
  you said it (the microphone's own buffer, noise suppression and the step that keeps the two sound
  cards in step); that stays the same however long Backtrack runs. While the microphone is
  recorded, Windows shows its microphone icon in the taskbar; pausing stops it. If Windows' privacy
  settings do not let desktop apps use the microphone (Settings > Privacy & security > Microphone),
  the status says so, and the microphone is used as soon as that is turned on.
- The microphone is timed by your playback device. When nothing at all can play sound (no
  speakers or headphones), the microphone is not recorded either.
- Push-to-talk and a game (or any program) running as administrator: while such a program is in
  front, Windows may hide your keys from Backtrack, so holding the push-to-talk key does nothing.
  The Record page says so when it happens. Run Backtrack as administrator too, or turn
  push-to-talk off for that game.
- The "saved" sound (and the error sound when a save is refused) plays on your speakers, so the
  next clip that overlaps that moment contains it, like anything else you hear. `sound = false`
  turns it off.
- Settings and the log live next to `backtrack.exe`, so put it in a folder you can write to
  (not `C:\Program Files`).
- Windows "N" editions need the free Media Feature Pack from Microsoft for the video encoder.

## Settings (`backtrack.ini`)

Everything here can be changed on the Settings page; this file is where it is kept. It is written
with the defaults the first time Backtrack starts. If you edit it by hand, restart Backtrack
afterwards. A missing key uses its default; a value that cannot be used is logged with its line
number and the default is kept. Backtrack never refuses to start because of the settings.
"Launch when Windows starts" is not in this file: it is the `Backtrack` entry in Windows' own
startup list (Task Manager > Startup apps).

### `[general]`

| Key | Default | Meaning |
|---|---|---|
| `start_minimized` | `false` | When Windows starts Backtrack, open it in the tray instead of showing the window. |

### `[clip]`

| Key | Default | Meaning |
|---|---|---|
| `seconds` | `30` | How many seconds to keep and save. 5 to 300. At 20 Mbit/s, 30 s is about 75 MB of memory. |
| `output_dir` | `%USERPROFILE%\Videos\Backtrack` | Where clips go. Environment variables work; a relative path is taken from the folder `backtrack.exe` is in. Created if missing; if it cannot be created, clips go to your Videos folder. |
| `filename` | `clip_%Y-%m-%d_%H-%M-%S.mp4` | File name. Allowed placeholders: `%Y %y %m %d %H %I %M %S %p %j %a %A %b %B %U %W %w %%`. Two clips in the same second get `_2`, `_3`. |

To use `;` or `#` in a path, put the whole value in double quotes: `output_dir = "D:\Clips #2"`.

### `[storage]`

| Key | Default | Meaning |
|---|---|---|
| `max_gb` | `0` | When clips take more than this many GB, the oldest go to the Recycle Bin. `0` is no limit. Favorites and clips in a collection are never removed. |
| `delete_after_days` | `0` | Clips older than this go to the Recycle Bin. `0` is never. Favorites and clips in a collection are kept. |

### `[hotkey]`

| Key | Default | Meaning |
|---|---|---|
| `mic_mute` | `Alt+F7` | Mutes and unmutes the microphone. Empty: no key. Same rules as `save`. |
| `push_to_talk` | `V` | The key to hold for push-to-talk. Plain letters and digits work here, and also CapsLock, Tab, Backquote (the key left of 1), Up/Down/Left/Right and `Mouse3` / `Mouse4` / `Mouse5` (wheel and side buttons): it is only watched, every program still gets it. Pause and PrintScreen cannot be held. |
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
| `enabled` | `true` | Record what you hear. `false` leaves it out of clips (with `mic = false` too, clips have no sound). |
| `bitrate_kbps` | `160` | Sound quality: 96, 128, 160 or 192. Others are rounded to the nearest. |
| `system_volume` | `100` | Volume of what you hear in the clip, 0 to 200 percent. |
| `mic` | `true` | Record the microphone, mixed into the same sound track. |
| `mic_device` | empty | Which microphone (a Windows device id; the Settings page fills it in). Empty is the Windows default microphone, followed when it changes. A chosen microphone that is not connected is replaced by the default until it is. |
| `mic_volume` | `100` | Volume of the microphone in the clip, 0 to 200 percent. |
| `noise_suppression` | `true` | Take steady background noise (fans, hum, hiss) out of the microphone and soften key clicks between words. |
| `push_to_talk` | `false` | Only record the microphone while the `[hotkey] push_to_talk` key is held (and 0.2 s after, so the last word is not cut off). |

### `[feedback]`

| Key | Default | Meaning |
|---|---|---|
| `sound` | `true` | Play the Windows "asterisk" sound when a clip is saved. |
| `notification` | `true` | Show a Windows message when a clip is saved while the Backtrack window is not in front. |
| `saved_bar` | `true` | Show the "Saved" bar at the bottom of the window after a save. |

### `[logging]`

| Key | Default | Meaning |
|---|---|---|
| `level` | `info` | `error`, `warning`, `info` or `debug`. `error` still shows warnings (why a hotkey or a save did not work). `debug` adds counters once a minute: frames, drops, sound corrections, buffer size and memory use. |

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

`build\Release\record_check.exe <backtrack.ini> <log file> <save after s> <exit after s>` runs
the same recording engine without the window, prints the sound meters and the microphone's state
every second and saves one clip to the ini's `output_dir`. It has no single-instance lock, so it
also works while Backtrack itself is running.

### Checking sound/picture sync

`build\Release\sync_beacon.exe <seconds>` shows a square in the top-left corner of the primary
monitor that flashes white exactly when a short beep plays. Record it, save a clip, then run
`tools\check_sync.ps1 <clip.mp4>` (needs ffmpeg on PATH, only for this check). It prints the
offset between each flash and beep in the clip.

## How it works

Screen frames are captured on a fixed tick (a static screen still produces every frame), converted
to NV12 on the GPU and encoded to H.264 with one keyframe per second and no B-frames. Sound is
captured from the default playback device, resampled to 48 kHz and encoded to AAC. The microphone
is recorded on its own thread, resampled to 48 kHz, cleaned by a noise suppressor (short-time
Fourier transform, a noise floor per frequency found from its quiet moments, Wiener gain) and
mixed into the game sound just before the AAC encoder, through a small buffer that keeps the two
sound cards' clocks in step by stretching or squeezing the voice by a fraction of a percent. Both
streams of compressed packets go into ring buffers in memory that always start on a keyframe. Saving copies
the last `seconds` out of the buffers in a few milliseconds and writes them to an MP4 on a separate
thread without re-encoding, under a `.tmp` name that is renamed when complete. Every timestamp
comes from one clock, so sound and picture stay in sync however long Backtrack runs.

Code layout: `src/core` is plain C++ with unit tests (settings, hotkeys, ring buffer, clip
planning, the clip library and its JSON file, the microphone's buffer, mixer and noise
suppressor); `src/platform` is everything that talks to Windows;
`src/platform/Recorder.h` is the engine the window drives; `src/ui` is the window, drawn with
Direct2D and DirectWrite on a Direct3D 11 swap chain (part of Windows, nothing to install). The
clip page plays clips with Windows' Media Foundation media engine on the same graphics device, so
the controls are drawn over the picture. Trimming (`src/platform/Trim.cpp`) copies the compressed
frames into a new file without decoding them.
