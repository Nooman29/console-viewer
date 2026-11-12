# Console Viewer, C++ version

The same idea as the other versions, written in C++ straight against Windows: Media Foundation reads the capture card, WASAPI plays its sound, and a plain Win32 window shows the picture. It is one small program file with nothing to install.

> **Status: early.** It passes its automated tests, which use pretend devices, and has been run against a laptop webcam and microphone. It has not been tested with a real capture card yet, and it has only been built with MinGW-w64 GCC so far.

## Run

```powershell
console-viewer.exe
```

It opens the capture card if one is plugged in, and otherwise the first camera it finds. If nothing is plugged in it waits, and connects by itself when a device appears.

| Command | What it does |
|---|---|
| `console-viewer --demo` | Shows a moving test picture. Needs no hardware. |
| `console-viewer --list` | Lists the cameras and sound inputs it can see, then exits. |
| `console-viewer --device elgato` | Opens the video device whose name contains "elgato". |
| `console-viewer --audio hdmi` | Uses the sound input whose name contains "hdmi". |
| `console-viewer --no-audio` | Picture only. |
| `console-viewer --mute` | Starts with the sound muted. |
| `console-viewer --fullscreen` | Starts in full screen. |
| `console-viewer --fill` | Fills the window, cutting off the edges of the picture. |
| `console-viewer --width 1280 --height 720 --fps 60` | Asks the device for a different picture size or speed. |

## Keys

| Key | Action |
|---|---|
| `F`, `F11` or double-click | Full screen |
| `M` | Mute or unmute |
| `+` / `-` | Volume |
| `S` | Save a screenshot (PNG) to Downloads |
| `Z` | Fit or fill the window |
| `D` | Next video device |
| `A` | Next sound input, then off |
| `H` | Show the keys |
| `Q` or `Esc` | Quit (`Esc` leaves full screen first) |

## How it differs from the other versions

- **No clip recording yet.** Screenshots only.
- **Sound is matched by name**, as in the Python version. If your card's sound is not picked, use `--audio` or press `A`.
- **Sound is only chosen automatically for capture cards**, so a webcam's microphone never plays through your speakers by accident.
- **No tray icon.** Close the window to quit.

## Build

You need CMake 3.16 or newer and a C++17 compiler for Windows.

With Visual Studio 2022 (the "Desktop development with C++" workload):

```powershell
cd cpp
cmake -S . -B build
cmake --build build --config Release
```

With MinGW-w64 (for example [WinLibs](https://winlibs.com/), which includes CMake and Ninja):

```powershell
cd cpp
cmake -S . -B build -G Ninja
cmake --build build
```

## Tests

The core logic is plain C++ with no Windows in it, and is tested with pretend devices:

```powershell
.\build\cv_tests.exe
```

With Visual Studio the file is at `.\build\Release\cv_tests.exe`.

## How the code is laid out

| Folder or file | What is in it |
|---|---|
| `src/core/rules.*` | The decisions: which device to open, which sound belongs to it, which picture format to ask for, when a black picture means copy protection. No Windows. |
| `src/core/controller.*` | What the viewer is doing and how it reacts to devices, keys and time. Talks to hardware only through a small interface, which the tests replace. |
| `src/win/capture.cpp` | Reads frames with Media Foundation on a background thread and keeps only the newest, for low delay. |
| `src/win/audio.cpp` | Plays the card's sound through the speakers with WASAPI. |
| `src/win/devices.cpp` | Lists cameras and sound inputs. |
| `src/win/window.cpp` | The window and everything drawn in it. |
| `src/win/screenshot.cpp` | Saves PNG screenshots. |
| `src/win/main.cpp` | The command line and the main loop. |
| `tests/test_core.cpp` | The tests. |
