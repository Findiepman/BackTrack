# Backtrack

A replay buffer for Windows: it keeps the last few seconds of your screen in memory and saves them
to an `.mp4` when you press a hotkey. Built from scratch on what Windows ships with (DXGI,
Direct3D 11, Media Foundation, WASAPI). No FFmpeg, no OBS.

> **Status: milestone 2 of 5.** Screen capture, colour conversion, the H.264 encoder (GPU, with a
> software fallback) and the in-memory clip buffer work. There is no MP4 saving, audio or tray
> icon yet. The current exe is a test program that saves raw `.h264` clips.

## Building

Needs Visual Studio 2022 (or its Build Tools) with the C++ workload, and CMake 3.25 or newer
(`winget install Kitware.CMake`).

```
build.bat
```

This builds `build\Release\backtrack.exe`, puts a default `backtrack.ini` next to it and runs the
unit tests. To run only the unit tests (about two seconds, no GPU needed):

```
tests\run-tests.bat
```

## Trying the test program

Run `build\Release\backtrack.exe` from a terminal. It records the monitor set in `backtrack.ini`,
encodes it and keeps the last `seconds` in memory.

- Press **F8** (or the hotkey in `backtrack.ini`) to save the last `seconds` to
  `build\Release\frames\clip_<date>_<time>.h264`. The log says how long the clip is, that it
  starts on a keyframe and how often keyframes come (once per second).
  A raw `.h264` file has no timestamps, so players guess the speed; the MP4 files from milestone 3
  will not have that problem. To watch one at the right speed: `ffplay -framerate 60 clip.h264`.
- Press **Ctrl+C** to quit.

Options for testing: `--save-after <seconds>`, `--dump-after <seconds>` (dumps one frame and checks
its colours), `--exit-after <seconds>`, `--lose-capture-every <seconds>` (pretends Windows took the
screen away, to test recovery).

Everything is also written to `backtrack.log` next to the exe.

## Hotkeys that are refused

Backtrack refuses hotkeys that could never work and falls back to F8, with the reason in the log:
letters, digits or Space without a modifier (they would stop working in every other program),
F12 on its own (reserved by Windows), Ctrl+Pause (the keyboard sends Break instead) and
Shift+Numpad keys (NumLock turns them into arrow keys). PrintScreen on its own is allowed but
Windows 11 gives it to the Snipping Tool unless that is turned off.

## Known limitations

- Exclusive fullscreen games cannot be recorded by Windows Desktop Duplication. Set games to
  borderless windowed.
- Settings and the log live next to the exe, so put Backtrack in a folder you can write to
  (not `C:\Program Files`).
