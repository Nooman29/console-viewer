"""The real hardware and operating system, behind one small interface.

The viewer only talks to a `Backend`. The tests give it a pretend one, so
everything in viewer.py can be checked without a capture card.
"""

from __future__ import annotations

import sys
import threading
import time
from pathlib import Path
from typing import Any, Optional

import numpy as np

from .audio import AudioPassthrough
from .capture import DemoSource, OpenCVSource, Source
from .rules import AudioDevice, VideoDevice


class SystemBackend:
    """Cameras through OpenCV, sound through sounddevice."""

    def __init__(self, width: int = 1920, height: int = 1080, fps: int = 60) -> None:
        self.width, self.height, self.fps = width, height, fps
        self._audio_lock = threading.Lock()
        self._audio: Optional[AudioPassthrough] = None

    # ---- Devices ----

    def list_videos(self) -> list[VideoDevice]:
        import cv2
        from cv2_enumerate_cameras import enumerate_cameras

        backend = cv2.CAP_DSHOW if sys.platform == "win32" else cv2.CAP_ANY
        devices = []
        for cam in enumerate_cameras(backend):
            key = getattr(cam, "path", None) or f"{cam.name}#{cam.index}"
            devices.append(VideoDevice(index=cam.index, name=cam.name, key=str(key)))
        return devices

    def list_audios(self) -> list[AudioDevice]:
        import sounddevice as sd

        with self._audio_lock:
            # The sound library reads the device list once, at start-up. To
            # notice a card plugged in later it has to be restarted, which is
            # only safe while no sound is playing.
            if self._audio is None or not self._audio.active:
                try:
                    sd._terminate()
                    sd._initialize()
                except Exception:
                    pass
            return self._query_audios(sd)

    @staticmethod
    def _query_audios(sd: Any) -> list[AudioDevice]:
        apis = sd.query_hostapis()
        # Windows lists every device once per sound system. WASAPI gives the
        # full names and the lowest delay, so use only those when present.
        wanted = [i for i, api in enumerate(apis) if "WASAPI" in api["name"]]
        devices = []
        for index, info in enumerate(sd.query_devices()):
            if info["max_input_channels"] < 1:
                continue
            if wanted and info["hostapi"] not in wanted:
                continue
            devices.append(AudioDevice(
                index=index,
                name=info["name"],
                samplerate=int(info["default_samplerate"]),
                channels=int(info["max_input_channels"]),
            ))
        return devices

    def open_video(self, device: VideoDevice) -> Source:
        return OpenCVSource(device, self.width, self.height, self.fps)

    def open_audio(self, device: AudioDevice) -> Any:
        import sounddevice as sd

        with self._audio_lock:
            # Device numbers can shift when the list is refreshed, so find the
            # device again by name just before opening it.
            fresh = next((a for a in self._query_audios(sd) if a.name == device.name), device)
            self._audio = AudioPassthrough(fresh, sd, self._audio_lock)
            return self._audio

    # ---- Saving files ----

    def save_folder(self) -> Path:
        downloads = Path.home() / "Downloads"
        return downloads if downloads.is_dir() else Path.cwd()

    def save_image(self, path: Path, frame: np.ndarray) -> bool:
        import cv2

        # cv2.imwrite fails on Windows for paths with accented letters (a user
        # called Jose with an accent, say), so encode in memory and let Python
        # write the file.
        ok, encoded = cv2.imencode(path.suffix or ".png", frame)
        if not ok:
            return False
        try:
            encoded.tofile(str(path))
        except OSError:
            return False
        return True

    def open_recorder(self, path: Path, width: int, height: int, fps: float,
                      audio: Optional[tuple[int, int]] = None) -> Any:
        from .recorder import ClipRecorder

        return ClipRecorder(path, width, height, fps, audio)

    # ---- Keeping the screen on ----
    # Playing with a controller sends no keyboard or mouse input to the PC,
    # so without this Windows would dim or lock the screen mid-game.

    def keep_awake(self, on: bool) -> None:
        if sys.platform != "win32":
            return
        import ctypes

        ES_CONTINUOUS, ES_DISPLAY_REQUIRED = 0x80000000, 0x00000002
        flags = ES_CONTINUOUS | (ES_DISPLAY_REQUIRED if on else 0)
        ctypes.windll.kernel32.SetThreadExecutionState(flags)


class DemoBackend(SystemBackend):
    """A pretend capture card showing a moving test picture. No hardware used."""

    DEVICE = VideoDevice(index=0, name="Demo HDMI Capture", key="demo")

    def list_videos(self) -> list[VideoDevice]:
        return [self.DEVICE]

    def list_audios(self) -> list[AudioDevice]:
        return []

    def open_video(self, device: VideoDevice) -> Source:
        return DemoSource(1280, 720, self.fps)


def timestamp(now: Optional[float] = None) -> str:
    """A file-name-safe local time, for example 20261111-140259."""
    return time.strftime("%Y%m%d-%H%M%S", time.localtime(now))
