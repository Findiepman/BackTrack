# Backtrack

A replay buffer for Windows: it keeps the last few seconds of your screen in memory and saves them
to an `.mp4` when you press a hotkey. Built from scratch on what Windows ships with (DXGI,
Direct3D 11, Media Foundation, WASAPI). No FFmpeg, no OBS.

> **Status: milestone 1 of 5.** Screen capture and colour conversion work. There is no encoder,
> no clip saving and no tray icon yet. The current exe is a test program for the capture path.

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

## Trying milestone 1

Run `build\Release\backtrack.exe` from a terminal. It records the monitor set in `backtrack.ini`
at the configured frame rate and prints what it does.

- Press **F8** (or the hotkey in `backtrack.ini`) to dump the current frame to
  `build\Release\frames\`. It writes the raw NV12 frame, the desktop image it came from
  (`_desktop.bmp`) and the NV12 frame decoded back to a picture (`_decoded.bmp`), and checks that
  the colours match. Look for `Colour check PASSED` in the output.
- Press **Ctrl+C** to quit.

Options for testing: `--dump-after <seconds>`, `--exit-after <seconds>`,
`--lose-capture-every <seconds>` (pretends Windows took the screen away, to test recovery).

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
