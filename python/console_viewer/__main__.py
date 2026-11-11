"""Command line entry point: `python -m console_viewer`."""

from __future__ import annotations

import argparse
import queue
import sys
import threading
import time
from typing import Any, Optional

from . import __version__
from .rules import choose_audio, looks_like_capture


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="console-viewer",
        description="Show a games console on your PC through an HDMI capture card.",
    )
    parser.add_argument("--list", action="store_true", help="list cameras and sound inputs, then exit")
    parser.add_argument("--device", metavar="NAME", help="open the video device whose name contains NAME")
    parser.add_argument("--audio", metavar="NAME", help="use the sound input whose name contains NAME")
    parser.add_argument("--no-audio", action="store_true", help="picture only")
    parser.add_argument("--fullscreen", action="store_true", help="start in full screen")
    parser.add_argument("--fill", action="store_true", help="fill the window, cutting off the edges")
    parser.add_argument("--width", type=int, default=1920, help="picture width to ask for (default 1920)")
    parser.add_argument("--height", type=int, default=1080, help="picture height to ask for (default 1080)")
    parser.add_argument("--fps", type=int, default=60, help="frames per second to ask for (default 60)")
    parser.add_argument("--demo", action="store_true", help="show a moving test picture; needs no hardware")
    parser.add_argument("--exit-after", type=float, metavar="SECONDS", help=argparse.SUPPRESS)
    parser.add_argument("--version", action="version", version=f"console-viewer {__version__}")
    return parser


def list_devices(backend: Any) -> None:
    videos = backend.list_videos()
    audios = backend.list_audios()
    print("Video devices:")
    for device in videos:
        kind = "capture card" if looks_like_capture(device.name) else "camera"
        print(f"  {device.name}  ({kind})")
        sound = choose_audio(device, audios)
        if sound:
            print(f"      sound: {sound.name}")
    if not videos:
        print("  none found")
    print("Sound inputs:")
    for device in audios:
        print(f"  {device.name}")
    if not audios:
        print("  none found")


def run(viewer: Any, backend: Any, window: Any, scan_seconds: float = 2.0,
        exit_after: Optional[float] = None) -> int:
    """The main loop: look for devices in the background, draw in the foreground."""
    found: "queue.Queue[tuple[list, list]]" = queue.Queue(maxsize=1)
    stop = threading.Event()

    def scan() -> None:
        # Listing devices can take a moment, so it must not hold up the picture.
        while not stop.is_set():
            try:
                result = (backend.list_videos(), backend.list_audios())
            except Exception:
                result = None
            if result is not None:
                try:
                    found.get_nowait()  # drop an unread older list
                except queue.Empty:
                    pass
                found.put(result)
            stop.wait(scan_seconds)

    threading.Thread(target=scan, name="device-scan", daemon=True).start()

    # Windows normally wakes a waiting program only every 15 ms or so, which
    # is too coarse to show 60 pictures a second smoothly. Ask for 1 ms.
    fine_timer = False
    if sys.platform == "win32":
        try:
            import ctypes
            fine_timer = ctypes.windll.winmm.timeBeginPeriod(1) == 0
        except Exception:
            fine_timer = False

    started = time.monotonic()
    last_count = -1
    last_draw = 0.0
    frames_drawn = 0
    is_fullscreen = False
    try:
        while True:
            try:
                videos, audios = found.get_nowait()
                viewer.update_devices(videos, audios)
            except queue.Empty:
                pass
            viewer.tick()

            if viewer.fullscreen != is_fullscreen:
                is_fullscreen = viewer.fullscreen
                window.set_fullscreen(is_fullscreen)

            # Draw only when there is a new picture or something else changed.
            now = time.monotonic()
            count = viewer.frame_count()
            if count != last_count or viewer.dirty or now - last_draw > 0.25:
                last_count, last_draw = count, now
                width, height = window.size()
                window.show(viewer.render(width, height))
                frames_drawn += 1

            if not viewer.handle_key(window.poll_key(1 if viewer.state == viewer.LIVE else 30)):
                break
            if not window.is_open():
                break
            if exit_after is not None and now - started >= exit_after:
                break
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        viewer.close()
        window.close()
        if fine_timer:
            ctypes.windll.winmm.timeEndPeriod(1)
    if exit_after is not None:
        print(f"drew {frames_drawn} frames in {time.monotonic() - started:.1f} s; last state: {viewer.state}")
    return 0


def main(argv: Optional[list[str]] = None) -> int:
    args = build_parser().parse_args(argv)

    try:
        from .backend import DemoBackend, SystemBackend
    except ImportError as err:
        print(f"A required package is missing: {err.name}. Run: pip install -e .", file=sys.stderr)
        return 1

    backend_class = DemoBackend if args.demo else SystemBackend
    backend = backend_class(args.width, args.height, args.fps)
    if args.list:
        list_devices(backend)
        return 0

    from .viewer import Options, Viewer
    from .window import Window

    viewer = Viewer(backend, Options(
        wanted_video=args.device,
        wanted_audio=args.audio,
        no_audio=args.no_audio,
        fill=args.fill,
        fullscreen=args.fullscreen,
        background_open=True,
    ))
    return run(viewer, backend, Window(), exit_after=args.exit_after)


if __name__ == "__main__":
    sys.exit(main())
