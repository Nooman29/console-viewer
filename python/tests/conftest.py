"""Pretend hardware shared by the tests. No camera or microphone is touched."""

from __future__ import annotations

import time
from pathlib import Path

import numpy as np
import pytest

from console_viewer.backend import SystemBackend
from console_viewer.rules import AudioDevice, VideoDevice
from console_viewer.viewer import Viewer

WEBCAM = VideoDevice(0, "HP True Vision HD Camera", "cam1")
MIC = AudioDevice(1, "Microphone Array (Intel Smart Sound Technology)")
CARD = VideoDevice(1, "USB3.0 HD Video Capture", "card1")
CARD_SOUND = AudioDevice(2, "Digital Audio Interface (USB3.0 HD Video Capture)")
CARD2 = VideoDevice(2, "Elgato HD60 X", "card2")
# The most common cheap card: its sound input is not named after its picture.
CHEAP_CARD = VideoDevice(3, "USB Video", "ms1")
CHEAP_CARD_SOUND = AudioDevice(3, "Digital Audio Interface (USB Digital Audio)")


class FakeSource:
    width, height, fps = 320, 180, 60.0

    def __init__(self) -> None:
        self.alive = True
        self.black = False
        self.released = False

    def read(self):
        if not self.alive:
            return None
        time.sleep(0.002)
        frame = np.zeros((self.height, self.width, 3), np.uint8)
        if not self.black:
            frame[:] = (60, 140, 40)
            frame[40:80, 40:80] = 255
        return frame

    def release(self) -> None:
        self.released = True


class FakeAudio:
    def __init__(self, device: AudioDevice) -> None:
        self.device = device
        self.muted = False
        self.volume = 1.0
        self.stopped = False
        self.on_audio = None
        self.samplerate = device.samplerate
        self.channels = min(2, device.channels)

    def stop(self) -> None:
        self.stopped = True


class FakeBackend(SystemBackend):
    """Devices that exist only in memory. Saving files uses the real code."""

    def __init__(self, folder: Path) -> None:
        super().__init__()
        self.folder = folder
        self.busy: set[str] = set()          # video keys that refuse to open
        self.broken_audio: set[str] = set()  # sound names that refuse to open
        self.sources: list[tuple[VideoDevice, FakeSource]] = []
        self.sounds: list[FakeAudio] = []
        self.awake: list[bool] = []

    def open_video(self, device: VideoDevice) -> FakeSource:
        if device.key in self.busy:
            raise OSError(f"Could not open {device.name}. Another app may be using it.")
        source = FakeSource()
        self.sources.append((device, source))
        return source

    def open_audio(self, device: AudioDevice) -> FakeAudio:
        if device.name in self.broken_audio:
            raise OSError("no sound")
        sound = FakeAudio(device)
        self.sounds.append(sound)
        return sound

    def save_folder(self) -> Path:
        return self.folder

    def keep_awake(self, on: bool) -> None:
        self.awake.append(on)


class Clock:
    """A clock the test moves by hand."""

    def __init__(self) -> None:
        self.now = 1000.0

    def __call__(self) -> float:
        return self.now

    def advance(self, seconds: float) -> None:
        self.now += seconds


@pytest.fixture
def backend(tmp_path) -> FakeBackend:
    return FakeBackend(tmp_path)


@pytest.fixture
def clock() -> Clock:
    return Clock()


@pytest.fixture
def viewer(backend, clock):
    made = Viewer(backend, clock=clock)
    yield made
    made.close()


def wait_for(condition, seconds: float = 4.0) -> bool:
    """Wait for something that happens on another thread."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if condition():
            return True
        time.sleep(0.01)
    return bool(condition())


def wait_for_frame(viewer: Viewer) -> None:
    assert wait_for(lambda: viewer.frame_count() > 0), "no picture arrived"
