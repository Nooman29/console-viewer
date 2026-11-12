# Console Viewer, Python version

The same idea as the web and desktop versions, written in Python: plug an HDMI capture card into a USB port and see your console in a window, with sound.

> **Status: early.** It passes its automated tests, which use pretend devices, and has been run against a laptop webcam and microphone. It has not been tested with a real capture card yet.

## Install

You need Python 3.10 or newer from [python.org](https://www.python.org/downloads/).

```powershell
cd python
py -m venv .venv
.\.venv\Scripts\python.exe -m pip install -e .
```

## Run

```powershell
.\.venv\Scripts\console-viewer.exe
```

The commands below are shortened to `console-viewer`; type the full `.\.venv\Scripts\console-viewer.exe` in their place.

It opens the capture card if one is plugged in, and otherwise the first camera it finds. If nothing is plugged in it waits, and connects by itself when a device appears.

| Command | What it does |
|---|---|
| `console-viewer --demo` | Shows a moving test picture. Needs no hardware. |
| `console-viewer --list` | Lists the cameras and sound inputs it can see, then exits. |
| `console-viewer --device elgato` | Opens the video device whose name contains "elgato". |
| `console-viewer --audio hdmi` | Uses the sound input whose name contains "hdmi". |
| `console-viewer --no-audio` | Picture only. |
| `console-viewer --fullscreen` | Starts in full screen. |
| `console-viewer --fill` | Fills the window, cutting off the edges of the picture. |
| `console-viewer --width 1280 --height 720 --fps 60` | Asks the device for a different picture size or speed. |

## Keys

| Key | Action |
|---|---|
| `F` | Full screen |
| `M` | Mute or unmute |
| `+` / `-` | Volume |
| `S` | Save a screenshot to Downloads |
| `R` | Start or stop recording a clip to Downloads |
| `Z` | Fit or fill the window |
| `D` | Next video device |
| `A` | Next sound input, then off |
| `H` | Show the keys |
| `Q` or `Esc` | Quit (`Esc` leaves full screen first) |

## How it differs from the web and desktop versions

- **Clips are MP4 with the game sound**, even while you have it muted. With no sound source, the clip has the picture only. On a slow PC the clip may skip some frames; the live picture is never held back.
- **Sound is matched by name.** A capture card's sound input is picked when its name contains the card's name, or looks like a capture card's. If yours is not picked, use `--audio` or press `A`.
- **Sound is only chosen automatically for capture cards**, so a webcam's microphone never plays through your speakers by accident.
- **No tray icon.** Close the window to quit.

## Tests

```powershell
.\.venv\Scripts\python.exe -m pip install -e ".[test]"
.\.venv\Scripts\python.exe -m pytest
```

The tests use pretend cameras and sound devices, so they need no hardware and never switch on your webcam.

## How the code is laid out

| File | What is in it |
|---|---|
| `console_viewer/rules.py` | The decisions: which device to open, which sound belongs to it, when a black picture means copy protection. No hardware. |
| `console_viewer/viewer.py` | The viewer: reacts to devices, keys and time, and draws the picture. Talks to hardware only through a backend. |
| `console_viewer/backend.py` | The real cameras, sound and files. The tests replace this. |
| `console_viewer/capture.py` | Reads frames on a background thread and keeps only the newest, for low delay. |
| `console_viewer/audio.py` | Plays the card's sound through the speakers. |
| `console_viewer/recorder.py` | Saves clips as MP4 with picture and sound, on its own thread. |
| `console_viewer/window.py` | The window. |
| `console_viewer/__main__.py` | The command line and the main loop. |
