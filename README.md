# Console Viewer

Play your console on your PC screen. Plug an HDMI capture card into a USB port and the picture appears, full screen, with sound.

Works with anything that has an HDMI output: PS4, PS5, Nintendo Switch, Xbox, a camera, another computer.

> **Status: early prototype.** The web version works with a webcam as a stand-in and passes its automated tests, which use pretend capture cards. It has not been tested with a real capture card yet. If you try it with one, please open an issue and say which card you used.

## What you need

Your PC's own HDMI port only sends a picture out. It cannot take one in. So you need a **USB HDMI capture card**: a small adapter with an HDMI socket on one end and a USB plug on the other. Basic ones cost about $15. For smooth play, look for one that says USB 3.0 and 1080p at 60 fps.

```
Console ──HDMI──▶ Capture card ──USB──▶ PC ──▶ Console Viewer
```

## Run it

You need Python (or any other local web server) and a current version of Chrome or Edge.

```powershell
python -m http.server 8000 --directory web
```

Then open <http://localhost:8000>, press **Start**, and allow camera and microphone access. The browser asks for both because a capture card presents itself as a camera and a microphone.

## Features

- Finds the capture card by itself and connects when you plug it in
- Reconnects if the card is unplugged and plugged back in
- Plays the console's sound through your PC
- Records video clips with sound and saves screenshots
- Keeps your screen awake while you play with a controller
- Tells you what to do if the picture is black
- Remembers your device, sound, quality and volume choices

| Key | Action |
|---|---|
| `F` or double-click | Full screen |
| `M` | Mute or unmute |
| `S` | Save a screenshot |
| `R` | Start or stop recording a video clip |
| `P` | Small floating window (Chrome and Edge) |
| `Z` | Fit or fill the screen |

## Black picture on a PlayStation?

PlayStations switch on copy protection (HDCP) by default, and capture cards show a black picture when it is on. Turn it off on the console:

**Settings → System → HDMI → Enable HDCP → off**

Some streaming apps (Netflix and similar) will not play while it is off. Games are not affected.

## Recording clips

Press `R` to start and `R` again to stop. The clip is saved to your Downloads folder as an MP4 (or WebM in browsers that cannot make MP4). If the capture card is unplugged while recording, what was recorded so far is still saved. A clip is held in memory until you stop, so keep clips to a few minutes.

## No sound?

Pick the capture card in the **Sound** menu at the top. Sound is only chosen automatically when the device's name looks like a capture card, so that a webcam's microphone never plays through your speakers by accident.

## Tests

The tests run the real page in Microsoft Edge with pretend capture cards, so they need no hardware. They cover plugging in, unplugging, a busy device, refused access, the black-picture tip, sound selection, recording and the keyboard shortcuts.

```powershell
cd tests
npm install
npm test
```

## Roadmap

| Phase | What | Status |
|---|---|---|
| 1 | Web app (`web/`) | In progress |
| 2 | Desktop app: opens by itself when a card is plugged in, tray icon, installer | Planned |
| 3 | Python version | Planned |
| 4 | C++ version using native Windows capture for the lowest delay | Planned |

## Contributing

Bug reports and capture card test results are welcome. Please open an issue.

## License

[MIT](LICENSE)
