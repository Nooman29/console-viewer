"""Saving clips as MP4 with picture and sound.

Encoding video is slow compared with capturing it, so pictures and sound are
handed to a separate thread. If the PC cannot keep up, pictures are dropped
from the clip; the live picture is never held back. Sound is never dropped,
because a gap in it would push everything after it out of step.
"""

from __future__ import annotations

import threading
import time
from collections import deque
from fractions import Fraction
from pathlib import Path
from typing import Optional

import numpy as np

_MILLISECONDS = Fraction(1, 1000)


class ClipRecorder:
    """Writes frames, and sound if there is any, to an MP4 file."""

    def __init__(
        self,
        path: Path,
        width: int,
        height: int,
        fps: float,
        audio: Optional[tuple[int, int]] = None,   # (sample rate, channels), or None
        queue_size: int = 30,                      # pictures waiting; 30 is about 180 MB at 1080p
    ) -> None:
        import av

        self._av = av
        self.path = Path(path)
        self.dropped = 0
        self.error: Optional[BaseException] = None  # why the clip failed, if it did
        self.audio_format = audio
        # H.264 needs even sizes; trim a stray odd row or column.
        self._width, self._height = width - width % 2, height - height % 2
        self._layout = "stereo"
        self._container = av.open(str(self.path), "w")
        try:
            rate = max(1, round(fps))
            try:
                video = self._container.add_stream("libx264", rate=rate)
                video.options = {"preset": "ultrafast", "crf": "21"}
            except Exception:
                video = self._container.add_stream("mpeg4", rate=rate)
                video.bit_rate = 8_000_000
            video.width, video.height = self._width, self._height
            video.pix_fmt = "yuv420p"
            # Time-stamp each frame with the real time it arrived, so a clip
            # plays at the right speed even when frames were dropped.
            video.codec_context.time_base = _MILLISECONDS
            self._video = video

            self._audio = None
            self._resampler = None
            self._audio_rate = 0
            if audio is not None:
                self._audio_rate, channels = int(audio[0]), int(audio[1])
                self._layout = "stereo" if channels >= 2 else "mono"
                self._audio = self._container.add_stream("aac", rate=self._audio_rate, layout=self._layout)
                self._resampler = av.AudioResampler(
                    format=self._audio.codec_context.format,
                    layout=self._layout,
                    rate=self._audio_rate,
                    frame_size=self._audio.codec_context.frame_size or 1024,
                )
        except Exception:
            self._container.close()
            raise

        self._start = time.monotonic()
        self._last_video_pts = -1
        self._audio_samples: Optional[int] = None
        self._max_pictures = max(1, queue_size)
        self._pictures: deque = deque()
        self._sound: deque = deque()
        self._wake = threading.Condition()
        self._closed = False
        self._thread = threading.Thread(target=self._run, name="clip-recorder", daemon=True)
        self._thread.start()

    # ---- Called from the capture and sound threads; these never wait ----

    def write(self, frame: np.ndarray) -> None:
        """Add a picture (BGR, as OpenCV delivers it)."""
        with self._wake:
            if self._closed:
                return
            if len(self._pictures) >= self._max_pictures:
                self.dropped += 1
                return
            self._pictures.append((time.monotonic(), frame))
            self._wake.notify()

    def write_audio(self, samples: np.ndarray) -> None:
        """Add sound: float32 samples shaped (count, channels)."""
        if self._audio is None:
            return
        block = np.array(samples, dtype=np.float32, copy=True)
        with self._wake:
            if self._closed:
                return
            self._sound.append((time.monotonic(), block))
            self._wake.notify()

    def release(self) -> None:
        """Finish the file. Safe to call more than once, and never hangs."""
        with self._wake:
            self._closed = True
            self._wake.notify()
        if self._thread.is_alive() and threading.current_thread() is not self._thread:
            self._thread.join(timeout=30)

    # ---- The recording thread ----

    def _run(self) -> None:
        try:
            while True:
                with self._wake:
                    while not self._pictures and not self._sound and not self._closed:
                        self._wake.wait()
                    sound = list(self._sound)
                    self._sound.clear()
                    picture = self._pictures.popleft() if self._pictures else None
                    finished = self._closed and picture is None and not sound
                for when, samples in sound:
                    self._encode_audio(when, samples)
                if picture is not None:
                    self._encode_video(*picture)
                if finished:
                    break
            if self._resampler is not None:
                for frame in self._resampler.resample(None):
                    self._mux_audio(frame)
            for packet in self._video.encode(None):
                self._container.mux(packet)
            if self._audio is not None:
                for packet in self._audio.encode(None):
                    self._container.mux(packet)
        except BaseException as err:  # disk full, a sound format the encoder refuses...
            self.error = err
        finally:
            with self._wake:
                self._closed = True  # stop accepting, so nothing piles up behind a dead clip
                self._pictures.clear()
                self._sound.clear()
            try:
                self._container.close()
            except Exception as err:
                self.error = self.error or err

    def _encode_video(self, when: float, image: np.ndarray) -> None:
        image = np.ascontiguousarray(image[: self._height, : self._width])
        frame = self._av.VideoFrame.from_ndarray(image, format="bgr24")
        pts = max(int((when - self._start) * 1000), self._last_video_pts + 1)
        self._last_video_pts = pts
        frame.pts = pts
        frame.time_base = _MILLISECONDS
        for packet in self._video.encode(frame):
            self._container.mux(packet)

    def _encode_audio(self, when: float, samples: np.ndarray) -> None:
        if samples.ndim == 1:
            samples = samples[:, None]
        if self._audio_samples is None:
            # Line the sound up with the picture: start it where it began.
            self._audio_samples = max(int((when - self._start) * self._audio_rate) - len(samples), 0)
        wanted = 2 if self._layout == "stereo" else 1
        if samples.shape[1] < wanted:
            samples = np.repeat(samples[:, :1], wanted, axis=1)
        packed = np.ascontiguousarray(samples[:, :wanted].reshape(1, -1))
        frame = self._av.AudioFrame.from_ndarray(packed, format="flt", layout=self._layout)
        frame.sample_rate = self._audio_rate
        frame.pts = self._audio_samples
        frame.time_base = Fraction(1, self._audio_rate)
        self._audio_samples += len(samples)
        for resampled in self._resampler.resample(frame):
            self._mux_audio(resampled)

    def _mux_audio(self, frame) -> None:
        for packet in self._audio.encode(frame):
            self._container.mux(packet)
