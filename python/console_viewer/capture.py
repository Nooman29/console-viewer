"""Reading pictures from a capture card.

`FrameGrabber` reads on its own thread and keeps only the newest frame, so
the picture on screen is never a queue of old ones. That is what keeps the
delay low.
"""

from __future__ import annotations

import sys
import threading
import time
from typing import Callable, Optional, Protocol

import numpy as np

from .rules import VideoDevice


class Source(Protocol):
    """Anything that produces frames: a real capture card, or a test double."""

    width: int
    height: int
    fps: float

    def read(self) -> Optional[np.ndarray]:
        """The next frame, or None if the device gave nothing."""

    def release(self) -> None:
        """Let go of the device."""


class OpenCVSource:
    """A camera or capture card opened with OpenCV."""

    def __init__(self, device: VideoDevice, width: int, height: int, fps: int) -> None:
        import cv2  # imported here so the pure logic can be tested without OpenCV

        # DirectShow opens fast and lets us ask for MJPEG. Without MJPEG a
        # USB 2.0 card delivers 1080p at about 5 frames a second.
        backend = cv2.CAP_DSHOW if sys.platform == "win32" else cv2.CAP_ANY
        cap = cv2.VideoCapture(device.index, backend)
        if not cap.isOpened():
            cap.release()
            raise OSError(f"Could not open {device.name}. Another app may be using it.")
        # Order matters: with DirectShow, setting the size or speed after the
        # format makes OpenCV forget the format, so MJPEG goes last.
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
        cap.set(cv2.CAP_PROP_FPS, fps)
        cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"MJPG"))
        cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
        code = int(cap.get(cv2.CAP_PROP_FOURCC))
        self.format = "".join(chr((code >> shift) & 0xFF) for shift in (0, 8, 16, 24)).strip("\x00")
        self._cap = cap
        self.width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH)) or width
        self.height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT)) or height
        self.fps = float(cap.get(cv2.CAP_PROP_FPS)) or float(fps)

    def read(self) -> Optional[np.ndarray]:
        ok, frame = self._cap.read()
        return frame if ok and frame is not None else None

    def release(self) -> None:
        self._cap.release()


class DemoSource:
    """A moving test picture, for trying the viewer with no hardware at all."""

    def __init__(self, width: int = 1280, height: int = 720, fps: int = 60) -> None:
        self.width, self.height, self.fps = width, height, float(fps)
        self._start = time.monotonic()
        self._count = 0
        ramp = np.linspace(40, 200, width, dtype=np.uint8)
        self._background = np.dstack([
            np.tile(ramp, (height, 1)),
            np.full((height, width), 90, np.uint8),
            np.tile(ramp[::-1], (height, 1)),
        ])

    def read(self) -> Optional[np.ndarray]:
        # Pace the frames like a real device would.
        due = self._start + self._count / self.fps
        delay = due - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        self._count += 1
        frame = self._background.copy()
        size = self.height // 6
        x = int((self._count * 6) % (self.width - size))
        y = int(self.height / 2 - size / 2)
        frame[y:y + size, x:x + size] = 255
        return frame

    def release(self) -> None:
        pass


class FrameGrabber:
    """Reads a source on a background thread and keeps the newest frame."""

    def __init__(
        self,
        source: Source,
        on_frame: Optional[Callable[[np.ndarray], None]] = None,
        max_failures: int = 60,
        retry_delay: float = 0.02,
        stall_seconds: float = 5.0,
    ) -> None:
        self.source = source
        self.on_frame = on_frame            # called for every frame, e.g. by the recorder
        self.failed = threading.Event()     # set when the device stops delivering
        self.measured_fps = 0.0
        self._max_failures = max_failures
        self._retry_delay = retry_delay
        self._stall_seconds = stall_seconds
        self._lock = threading.Lock()
        self._frame: Optional[np.ndarray] = None
        self._count = 0
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="frame-grabber", daemon=True)

    def start(self) -> "FrameGrabber":
        self._thread.start()
        return self

    def latest(self) -> tuple[Optional[np.ndarray], int]:
        """The newest frame and how many frames have arrived so far."""
        with self._lock:
            return self._frame, self._count

    def stop(self) -> None:
        self._stop.set()
        if self._thread.is_alive() and threading.current_thread() is not self._thread:
            self._thread.join(timeout=3)
        self.source.release()

    def _run(self) -> None:
        failures = 0
        window_start = time.monotonic()
        window_frames = 0
        last_good = time.monotonic()
        while not self._stop.is_set():
            try:
                frame = self.source.read()
            except Exception:  # a device vanishing mid-read can raise anything
                frame = None
            if frame is None:
                failures += 1
                # Some devices fail fast when unplugged; others make every
                # read wait a second, so also go by the clock.
                stalled = time.monotonic() - last_good >= self._stall_seconds
                if failures >= self._max_failures or stalled:
                    self.failed.set()
                    return
                time.sleep(self._retry_delay)
                continue
            failures = 0
            last_good = time.monotonic()
            with self._lock:
                self._frame = frame
                self._count += 1
            callback = self.on_frame
            if callback is not None:
                try:
                    callback(frame)
                except Exception:
                    self.on_frame = None  # a broken recorder must not stop the picture
            window_frames += 1
            elapsed = time.monotonic() - window_start
            if elapsed >= 1.0:
                self.measured_fps = window_frames / elapsed
                window_start, window_frames = time.monotonic(), 0
