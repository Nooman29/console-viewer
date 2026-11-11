"""The decisions the viewer makes, with no hardware involved.

Everything here is plain logic so it can be tested without a capture card:
which device to open, which sound source belongs to it, when a black picture
means copy protection, and how to fit a picture into a window.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Optional, Sequence

# Names that capture cards commonly report. Used only to prefer a capture
# card over a webcam when choosing automatically.
_CAPTURE_HINT = re.compile(
    r"capture|hdmi|cam link|elgato|avermedia|live gamer|usb ?3?\.?0? video"
    r"|macrosilicon|ezcap|video grabber|magewell|ripsaw",
    re.IGNORECASE,
)
_WEBCAM_HINT = re.compile(r"webcam|camera|facetime|integrated", re.IGNORECASE)
_CARD_SOUND_HINT = re.compile(r"digital audio interface|usb digital audio", re.IGNORECASE)


@dataclass(frozen=True)
class VideoDevice:
    """A camera or capture card. `key` stays the same across unplug and replug."""

    index: int
    name: str
    key: str


@dataclass(frozen=True)
class AudioDevice:
    """A sound input."""

    index: int
    name: str
    samplerate: int = 48000
    channels: int = 2


def looks_like_capture(name: str) -> bool:
    """True when a device name looks like a capture card and not a webcam."""
    return bool(_CAPTURE_HINT.search(name)) and not _WEBCAM_HINT.search(name)


def rank_videos(
    devices: Sequence[VideoDevice],
    prefer_key: Optional[str] = None,
    wanted_name: Optional[str] = None,
) -> list[VideoDevice]:
    """Every video device, best guess first.

    Order: the one just asked for, the one named on the command line, anything
    that looks like a capture card, then the rest in their original order.
    """
    wanted = (wanted_name or "").lower()

    def score(device: VideoDevice) -> int:
        if prefer_key and device.key == prefer_key:
            return 0
        if wanted and wanted in device.name.lower():
            return 1
        if looks_like_capture(device.name):
            return 2
        return 3

    return sorted(devices, key=score)  # sorted() keeps the original order for ties


def choose_audio(video: VideoDevice, audios: Sequence[AudioDevice]) -> Optional[AudioDevice]:
    """The sound input that belongs to a capture card, or None.

    Sound is only picked automatically for capture cards. Playing a webcam's
    microphone through the speakers would howl with feedback.
    """
    if not looks_like_capture(video.name):
        return None
    video_name = video.name.lower()
    # Windows names a card's sound input after the card: "HDMI (Cam Link 4K)".
    for audio in audios:
        if video_name in audio.name.lower():
            return audio
    for audio in audios:
        if looks_like_capture(audio.name) or _CARD_SOUND_HINT.search(audio.name):
            return audio
    return None


def find_audio_by_name(wanted: str, audios: Sequence[AudioDevice]) -> Optional[AudioDevice]:
    """The first sound input whose name contains `wanted`, ignoring case."""
    needle = wanted.lower()
    for audio in audios:
        if needle in audio.name.lower():
            return audio
    return None


class BlackDetector:
    """Decides when a picture has been black long enough to mention HDCP.

    A dark game scene still has some bright pixels; a blocked signal has none.
    Blocked signals are often dark grey with a little noise, not pure black,
    so the limit is above zero.
    """

    def __init__(self, limit: int = 24, seconds: float = 10.0) -> None:
        self.limit = limit
        self.seconds = seconds
        self._dark_since: Optional[float] = None

    def reset(self) -> None:
        self._dark_since = None

    def update(self, brightest: int, now: float) -> bool:
        """Feed the brightest pixel value of a frame. Returns True to show the tip."""
        if brightest >= self.limit:
            self._dark_since = None
            return False
        if self._dark_since is None:
            self._dark_since = now
        return now - self._dark_since >= self.seconds


def fit_rect(
    src_w: int, src_h: int, dst_w: int, dst_h: int, fill: bool = False
) -> tuple[int, int, int, int]:
    """Where a picture goes in a window: (x, y, width, height).

    With fill off the whole picture is visible with bars around it. With fill
    on the picture covers the window and the overflow is cut off, so x or y
    can be negative.
    """
    if min(src_w, src_h, dst_w, dst_h) <= 0:
        return 0, 0, max(dst_w, 1), max(dst_h, 1)
    scale_w = dst_w / src_w
    scale_h = dst_h / src_h
    scale = max(scale_w, scale_h) if fill else min(scale_w, scale_h)
    width = max(1, round(src_w * scale))
    height = max(1, round(src_h * scale))
    return (dst_w - width) // 2, (dst_h - height) // 2, width, height
