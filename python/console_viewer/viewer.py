"""The viewer itself: what is on screen and how it reacts.

`Viewer` holds no window and touches no hardware directly. It is told which
devices exist (`update_devices`), asked to move forward (`tick`), given key
presses (`handle_key`) and asked for a picture to show (`render`). Everything
real goes through the backend it was given, so the tests can swap that out.
"""

from __future__ import annotations

import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Optional, Sequence

import cv2
import numpy as np

from .backend import timestamp
from .capture import FrameGrabber
from .rules import (
    AudioDevice,
    BlackDetector,
    VideoDevice,
    choose_audio,
    find_audio_by_name,
    fit_rect,
    looks_like_capture,
    rank_videos,
)

ESC = 27
HELP_LINES = [
    "F  full screen        M  mute           + / -  volume",
    "S  screenshot         R  record a clip  Z  fit or fill",
    "D  next video device  A  next sound     H  this help",
    "Q or Esc  quit",
]
HDCP_LINES = [
    "The picture is black.",
    "Check the console is on. On a PlayStation, turn off Enable HDCP:",
    "PS5: Settings > System > HDMI.   PS4: Settings > System.",
]


@dataclass
class Options:
    wanted_video: Optional[str] = None   # part of a device name, from the command line
    wanted_audio: Optional[str] = None   # part of a sound input name
    no_audio: bool = False
    fill: bool = False
    fullscreen: bool = False
    retry_seconds: float = 3.0
    background_open: bool = False        # open devices on another thread (the real app does)


class Viewer:
    # What the viewer is doing right now.
    STARTING, CONNECTING, LIVE = "starting", "connecting", "live"
    WAITING, DISCONNECTED, ERROR = "waiting", "disconnected", "error"

    def __init__(
        self,
        backend: Any,
        options: Optional[Options] = None,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.backend = backend
        self.options = options or Options()
        self.clock = clock

        self.state = self.STARTING
        self.message = "Looking for devices..."
        self.detail = ""

        self.videos: list[VideoDevice] = []
        self.audios: list[AudioDevice] = []
        self._known_keys: set[str] = set()
        self._seen_devices = False

        self.current: Optional[VideoDevice] = None
        self.grabber: Optional[FrameGrabber] = None
        self.lost_key: Optional[str] = None     # the device that was showing when it dropped out
        self._retry_at = 0.0
        self._open_lock = threading.Lock()
        self._open_seq = 0                      # goes up with every connect()
        self._open_result: Optional[tuple] = None  # handed over by the opening thread
        self._added_while_connecting: list[VideoDevice] = []

        self.audio: Any = None                  # the running sound passthrough, if any
        self.audio_device: Optional[AudioDevice] = None
        self._audio_choice: Optional[str] = None  # None = automatic, "" = off, else a device name
        self._audio_failed: Optional[str] = None  # a sound input that would not open
        self.muted = False
        self.volume = 1.0

        self.fill = self.options.fill
        self.fullscreen = self.options.fullscreen
        self.show_help = False
        self.show_hdcp_tip = False
        self._black = BlackDetector()
        self._next_black_check = 0.0

        self._record_lock = threading.Lock()
        self._recorder: Any = None
        self._record_path: Optional[Path] = None
        self._record_since = 0.0
        self._record_shown = -1
        self._record_size = (0, 0)
        self._record_sound: Optional[tuple[int, int]] = None  # the clip's sound format

        self._toast = ""
        self._toast_until = 0.0
        self.dirty = True                       # something other than the picture changed

    # ------------------------------------------------------------------
    # Devices coming and going
    # ------------------------------------------------------------------

    def update_devices(self, videos: Sequence[VideoDevice], audios: Sequence[AudioDevice]) -> None:
        """Tell the viewer which devices exist right now."""
        before = self._known_keys
        first_time = not self._seen_devices
        self._seen_devices = True
        self.videos, self.audios = list(videos), list(audios)
        self._known_keys = {v.key for v in videos}
        added = [v for v in videos if v.key not in before]
        new_capture = next((v for v in added if looks_like_capture(v.name)), None)

        if self.state == self.CONNECTING:
            # Starting a second open of the same device would make both fail.
            self._added_while_connecting.extend(added)
            return
        if self.state != self.LIVE:
            if first_time:
                self.connect()
            elif added:
                self.connect(prefer=(new_capture or added[0]).key)
            elif self.lost_key and self.lost_key in self._known_keys:
                # A quick unplug and replug, or a card that reset itself when
                # the console changed resolution, never looks "new".
                self.connect(prefer=self.lost_key)
            elif not videos and self.state != self.DISCONNECTED:
                self._set_waiting()
            return

        # Take over from a webcam, but never interrupt a capture card mid-game.
        if new_capture and self.current and not looks_like_capture(self.current.name):
            self.connect(prefer=new_capture.key)
            self.toast(f"Switched to {new_capture.name}")
            return
        # A missing entry in the list is not trusted while pictures still
        # arrive; a real unplug stops the frames, which tick() notices.

        # A capture card's sound device can register a moment after its picture.
        if self.audio is None:
            self._start_audio()

    def connect(self, prefer: Optional[str] = None) -> None:
        """Open the best available device, falling through any that will not open."""
        self._stop_stream()
        with self._open_lock:
            self._open_seq += 1  # anything still opening is now out of date
            seq = self._open_seq
            stale, self._open_result = self._open_result, None
        if stale and stale[1] is not None:
            stale[1].release()  # opened, but never picked up
        if not self.videos:
            self._set_waiting()
            return
        candidates = rank_videos(self.videos, prefer, self.options.wanted_video)
        if not self.options.background_open:
            self._finish_open(*self._try_open(candidates))
            return
        # Opening a device can take a few seconds. Doing it on another thread
        # keeps the window responsive; tick() picks up the result.
        self._added_while_connecting = []
        self._set_state(self.CONNECTING, "Connecting...", "")
        threading.Thread(target=self._open_worker, args=(seq, candidates),
                         name="device-open", daemon=True).start()

    def _try_open(self, candidates: Sequence[VideoDevice]):
        """Open the first device that works. Returns (device, source, first error)."""
        first_error: Optional[Exception] = None
        for device in candidates:
            try:
                return device, self.backend.open_video(device), None
            except Exception as err:  # busy, unplugged mid-open, driver trouble
                first_error = first_error or err
        return None, None, first_error

    def _open_worker(self, seq: int, candidates: Sequence[VideoDevice]) -> None:
        device, source, error = self._try_open(candidates)
        with self._open_lock:
            if seq == self._open_seq:
                self._open_result = (device, source, error)
                return
        if source is not None:
            source.release()  # nobody is waiting for this one any more

    def _finish_open(self, device, source, error) -> None:
        if source is None:
            self._set_state(self.ERROR, "Could not open the device",
                            str(error) if error else "Another app may be using it.")
            self._retry_at = self.clock() + self.options.retry_seconds
            return
        self.grabber = FrameGrabber(source, on_frame=self._on_frame).start()
        self.current = device
        self.lost_key = None
        self._audio_failed = None
        self._black.reset()
        self.show_hdcp_tip = False
        self._set_state(self.LIVE, "", "")
        self.backend.keep_awake(True)
        self._start_audio()
        # A capture card that arrived while a webcam was opening still wins.
        late, self._added_while_connecting = self._added_while_connecting, []
        if not looks_like_capture(device.name):
            keys = {v.key for v in self.videos}
            newcomer = next((v for v in late if looks_like_capture(v.name) and v.key in keys), None)
            if newcomer is not None:
                self.connect(prefer=newcomer.key)

    def _set_waiting(self) -> None:
        self._set_state(self.WAITING, "No video device found",
                        "Plug your HDMI capture card into a USB port. It will appear here automatically.")

    def _set_state(self, state: str, message: str, detail: str) -> None:
        self.state, self.message, self.detail = state, message, detail
        self.dirty = True

    def _stop_stream(self) -> None:
        self.stop_recording()
        self._stop_audio()
        grabber, self.grabber = self.grabber, None
        if grabber is not None:
            grabber.stop()
            self.backend.keep_awake(False)
        self.current = None
        self.show_hdcp_tip = False

    # ------------------------------------------------------------------
    # Sound
    # ------------------------------------------------------------------

    def _wanted_audio(self) -> Optional[AudioDevice]:
        if self.current is None or self.options.no_audio or self._audio_choice == "":
            return None
        if self._audio_choice:
            return find_audio_by_name(self._audio_choice, self.audios)
        if self.options.wanted_audio:
            return find_audio_by_name(self.options.wanted_audio, self.audios)
        return choose_audio(self.current, self.audios)

    def _start_audio(self) -> None:
        if self.audio is not None:
            return
        device = self._wanted_audio()
        if device is None or device.name == self._audio_failed:
            return
        try:
            self.audio = self.backend.open_audio(device)
        except Exception:
            self._audio_failed = device.name  # do not retry it on every rescan
            self.toast("Could not open that sound source. Showing picture only.")
            return
        self.audio_device = device
        self._apply_volume()
        self._hook_recorder_sound()
        self.dirty = True

    def _hook_recorder_sound(self) -> None:
        """Feed the running clip from the current sound source, if they match."""
        sound = self.audio
        if sound is None or not hasattr(sound, "on_audio"):
            return
        with self._record_lock:
            recorder = self._recorder
        fmt = (int(getattr(sound, "samplerate", 48000)), int(getattr(sound, "channels", 2)))
        if recorder is not None and self._record_sound == fmt:
            sound.on_audio = recorder.write_audio

    def _stop_audio(self) -> None:
        audio, self.audio = self.audio, None
        self.audio_device = None
        if audio is not None:
            audio.stop()

    def _apply_volume(self) -> None:
        if self.audio is not None:
            self.audio.muted = self.muted
            self.audio.volume = self.volume

    def _next_audio(self) -> None:
        """Step through: each sound input in turn, then off."""
        names = [a.name for a in self.audios]
        # Go by what was last asked for, so an input that would not open is
        # stepped past instead of being tried again on every press.
        current = self._audio_choice or (self.audio_device.name if self.audio_device else None)
        position = names.index(current) + 1 if current in names else 0
        self._stop_audio()
        self._audio_failed = None
        if position < len(names):
            self._audio_choice = names[position]
            self._start_audio()
            if self.audio is not None:
                self.toast(f"Sound: {names[position]}")
            else:
                self.toast(f"Could not open {names[position]}. Press A for the next one.")
        else:
            self._audio_choice = ""
            self.toast("Sound: off")

    # ------------------------------------------------------------------
    # Moving forward in time
    # ------------------------------------------------------------------

    def tick(self) -> None:
        now = self.clock()
        with self._open_lock:
            result, self._open_result = self._open_result, None
        if result is not None:
            self._finish_open(*result)
        if self.state == self.LIVE and self.grabber is not None:
            if self.grabber.failed.is_set():
                self._on_disconnect()
            elif now >= self._next_black_check:
                self._next_black_check = now + 1.5
                frame, _ = self.grabber.latest()
                if frame is not None:
                    tip = self._black.update(int(frame[::16, ::16].max()), now)
                    if tip != self.show_hdcp_tip:
                        self.show_hdcp_tip, self.dirty = tip, True
        elif self.state == self.ERROR and now >= self._retry_at:
            self.connect()
        if self._toast and now >= self._toast_until:
            self._toast, self.dirty = "", True
        if self.recording:
            shown = int(now - self._record_since)
            if shown != self._record_shown:  # redraw only when the timer in the badge moves
                self._record_shown, self.dirty = shown, True
        # 7. Sound that died by itself (the card's sound input dropped out).
        if self.audio is not None and not getattr(self.audio, "active", True):
            self._stop_audio()

    def _on_disconnect(self) -> None:
        lost = self.current.key if self.current else None
        self._stop_stream()
        self.lost_key = lost
        self._set_state(self.DISCONNECTED, "Device disconnected",
                        "Plug it back in and it will reconnect automatically.")

    def frame_count(self) -> int:
        return self.grabber.latest()[1] if self.grabber is not None else 0

    # ------------------------------------------------------------------
    # Screenshots and clips
    # ------------------------------------------------------------------

    def screenshot(self) -> Optional[Path]:
        frame = self.grabber.latest()[0] if self.grabber is not None else None
        if frame is None:
            return None
        path = self._free_path("png")
        if not self.backend.save_image(path, frame):
            self.toast("Could not save the screenshot.")
            return None
        self.toast(f"Screenshot saved to {path.parent.name}")
        return path

    @property
    def recording(self) -> bool:
        return self._recorder is not None

    def start_recording(self) -> bool:
        if self.recording or self.grabber is None:
            return False
        frame = self.grabber.latest()[0]
        if frame is None:
            return False
        height, width = frame.shape[:2]
        fps = self.grabber.measured_fps or self.grabber.source.fps or 30.0
        fps = min(max(fps, 5.0), 120.0)
        path = self._free_path("mp4")
        sound = self.audio
        sound_format = None
        if sound is not None and hasattr(sound, "on_audio"):
            sound_format = (int(getattr(sound, "samplerate", 48000)), int(getattr(sound, "channels", 2)))
        try:
            recorder = self.backend.open_recorder(path, width, height, fps, sound_format)
        except Exception:
            self.toast("Could not start recording.")
            return False
        with self._record_lock:
            self._recorder, self._record_path = recorder, path
            self._record_size = (width, height)
        self._record_sound = sound_format
        self._record_shown = -1
        if sound_format is not None:
            sound.on_audio = recorder.write_audio
        self._record_since = self.clock()
        self.dirty = True
        return True

    def stop_recording(self) -> Optional[Path]:
        with self._record_lock:
            recorder, self._recorder = self._recorder, None
            path, self._record_path = self._record_path, None
        if recorder is None:
            return None
        if self.audio is not None and hasattr(self.audio, "on_audio"):
            self.audio.on_audio = None
        recorder.release()
        self._record_sound = None
        if getattr(recorder, "error", None) is not None:
            self.toast("The clip could not be saved.")
            return None
        self.toast(f"Clip saved to {path.parent.name}" if path else "Clip saved")
        return path

    def _on_frame(self, frame: np.ndarray) -> None:
        """Runs on the capture thread for every frame, so no frame is skipped."""
        with self._record_lock:
            if self._recorder is None:
                return
            width, height = self._record_size
            if frame.shape[1] != width or frame.shape[0] != height:
                frame = cv2.resize(frame, (width, height))  # the console changed resolution
            self._recorder.write(frame)

    def _free_path(self, extension: str) -> Path:
        folder = self.backend.save_folder()
        stem = f"console-viewer-{timestamp()}"
        path = folder / f"{stem}.{extension}"
        number = 1
        while path.exists():
            path = folder / f"{stem}-{number}.{extension}"
            number += 1
        return path

    # ------------------------------------------------------------------
    # Keys
    # ------------------------------------------------------------------

    def handle_key(self, key: int) -> bool:
        """React to a key press. Returns False when the viewer should close."""
        if key < 0:
            return True
        self.dirty = True
        if key == ESC:
            if self.fullscreen:
                self.fullscreen = False
                return True
            return False
        char = chr(key).lower() if key < 128 else ""
        if char == "q":
            return False
        if char == "h":
            self.show_help = not self.show_help
        elif char == "f":
            self.fullscreen = not self.fullscreen
        elif char == "z":
            self.fill = not self.fill
        elif char == "m":
            self.muted = not self.muted
            self._apply_volume()
            self.toast("Muted" if self.muted else "Sound on")
        elif char in ("+", "="):
            self._set_volume(self.volume + 0.1)
        elif char in ("-", "_"):
            self._set_volume(self.volume - 0.1)
        elif self.state == self.LIVE:
            if char == "s":
                self.screenshot()
            elif char == "r":
                if self.recording:
                    self.stop_recording()
                else:
                    self.start_recording()
            elif char == "a":
                self._next_audio()
            elif char == "d" and len(self.videos) > 1 and self.current is not None:
                keys = [v.key for v in self.videos]
                at = keys.index(self.current.key) if self.current.key in keys else -1
                self.connect(prefer=keys[(at + 1) % len(keys)])
        return True

    def _set_volume(self, volume: float) -> None:
        self.volume = round(min(max(volume, 0.0), 1.0), 2)
        self._apply_volume()
        self.toast(f"Volume {round(self.volume * 100)}%")

    def toast(self, text: str, seconds: float = 3.0) -> None:
        self._toast = text
        self._toast_until = self.clock() + seconds
        self.dirty = True

    # ------------------------------------------------------------------
    # Drawing
    # ------------------------------------------------------------------

    def render(self, width: int, height: int) -> np.ndarray:
        """The picture to put in a window of this size."""
        width, height = max(int(width), 16), max(int(height), 16)
        self.dirty = False
        frame = self.grabber.latest()[0] if self.grabber is not None else None

        if self.state == self.LIVE and frame is not None:
            canvas = _fit(frame, width, height, self.fill)
        else:
            canvas = np.full((height, width, 3), (16, 13, 11), np.uint8)
            message = self.message or ("Connecting..." if self.state == self.LIVE else "")
            _centered(canvas, ["Console Viewer"], height * 0.36, 1.5, (247, 244, 242), 2)
            _centered(canvas, [message], height * 0.36 + 60, 0.8, (247, 244, 242), 1)
            _centered(canvas, _wrap(self.detail, width - 80, 0.55), height * 0.36 + 100, 0.55, (185, 172, 163), 1)
            _centered(canvas, ["Press H for help"], height - 30, 0.5, (185, 172, 163), 1)

        if self.state == self.LIVE and self.show_hdcp_tip:
            _box(canvas, HDCP_LINES, height - 150, (32, 176, 255))
        if self.show_help:
            _box(canvas, HELP_LINES, max(30, height // 2 - 70), (255, 140, 79))
        if self.recording:
            seconds = int(self.clock() - self._record_since)
            cv2.circle(canvas, (28, height - 28), 8, (90, 90, 255), -1, cv2.LINE_AA)
            _text(canvas, f"Recording {seconds // 60}:{seconds % 60:02d}", 46, height - 22, 0.55, (255, 255, 255), 1)
        if self._toast:
            _centered(canvas, [self._toast], height - 60, 0.6, (255, 255, 255), 1, background=True)
        return canvas

    def close(self) -> None:
        self._stop_stream()
        with self._open_lock:
            self._open_seq += 1  # an open still in progress will release itself
            pending, self._open_result = self._open_result, None
        if pending and pending[1] is not None:
            pending[1].release()


# ----------------------------------------------------------------------
# Drawing helpers
# ----------------------------------------------------------------------

_FONT = cv2.FONT_HERSHEY_SIMPLEX


def _fit(frame: np.ndarray, width: int, height: int, fill: bool) -> np.ndarray:
    """Scale a frame into a window, with black bars or with the overflow cut off."""
    src_h, src_w = frame.shape[:2]
    x, y, w, h = fit_rect(src_w, src_h, width, height, fill)
    if (w, h) == (src_w, src_h):
        scaled = frame
    else:
        scaled = cv2.resize(frame, (w, h), interpolation=cv2.INTER_LINEAR)
    if (x, y, w, h) == (0, 0, width, height):
        return scaled.copy() if scaled is frame else scaled
    canvas = np.zeros((height, width, 3), np.uint8)
    # The part of the scaled picture that lands inside the window.
    left, top = max(x, 0), max(y, 0)
    right, bottom = min(x + w, width), min(y + h, height)
    canvas[top:bottom, left:right] = scaled[top - y:bottom - y, left - x:right - x]
    return canvas


def _text(canvas, text, x, y, scale, colour, thickness) -> None:
    cv2.putText(canvas, text, (int(x), int(y)), _FONT, scale, colour, thickness, cv2.LINE_AA)


def _centered(canvas, lines, y, scale, colour, thickness, background=False) -> None:
    width = canvas.shape[1]
    for line in lines:
        if not line:
            continue
        (text_w, text_h), _ = cv2.getTextSize(line, _FONT, scale, thickness)
        x = (width - text_w) // 2
        if background:
            cv2.rectangle(canvas, (x - 14, int(y) - text_h - 10), (x + text_w + 14, int(y) + 10), (24, 20, 18), -1)
        _text(canvas, line, x, y, scale, colour, thickness)
        y += text_h + 14


def _box(canvas, lines, top, edge_colour) -> None:
    scale, thickness = 0.55, 1
    sizes = [cv2.getTextSize(line, _FONT, scale, thickness)[0] for line in lines]
    box_w = max(w for w, _ in sizes) + 40
    box_h = len(lines) * 26 + 24
    left = max((canvas.shape[1] - box_w) // 2, 0)
    top = int(min(max(top, 0), max(canvas.shape[0] - box_h, 0)))
    cv2.rectangle(canvas, (left, top), (left + box_w, top + box_h), (24, 20, 18), -1)
    cv2.rectangle(canvas, (left, top), (left + box_w, top + box_h), edge_colour, 1)
    for i, line in enumerate(lines):
        _text(canvas, line, left + 20, top + 30 + i * 26, scale, (247, 244, 242), thickness)


def _wrap(text: str, max_width: int, scale: float) -> list[str]:
    """Break text into lines that fit the window."""
    lines: list[str] = []
    current = ""
    for word in text.split():
        trial = f"{current} {word}".strip()
        if current and cv2.getTextSize(trial, _FONT, scale, 1)[0][0] > max_width:
            lines.append(current)
            current = word
        else:
            current = trial
    if current:
        lines.append(current)
    return lines
