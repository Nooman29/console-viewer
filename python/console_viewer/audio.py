"""Playing the capture card's sound through the PC's speakers."""

from __future__ import annotations

import threading
from typing import Any, Optional

from .rules import AudioDevice


class AudioPassthrough:
    """Copies sound from an input device to the default output, with a volume."""

    def __init__(self, device: AudioDevice, sd: Any = None, lock: Optional[threading.Lock] = None) -> None:
        if sd is None:
            import sounddevice as sd  # imported here so tests can pass a stand-in
        self.device = device
        self.volume = 1.0
        self.muted = False
        self._lock = lock or threading.Lock()
        channels = max(1, min(2, device.channels))
        extra = None
        output = None
        try:
            # Use the output that belongs to the same Windows sound system as
            # the input, and let Windows convert sample rates if they differ.
            info = sd.query_devices(device.index)
            api = sd.query_hostapis(info["hostapi"])
            if api.get("default_output_device", -1) >= 0:
                output = api["default_output_device"]
            if "WASAPI" in api.get("name", ""):
                extra = sd.WasapiSettings(auto_convert=True)
        except Exception:
            pass  # fall back to the system defaults
        self._stream = sd.Stream(
            device=(device.index, output),
            samplerate=device.samplerate,
            channels=channels,
            dtype="float32",
            latency="low",
            callback=self._callback,
            extra_settings=extra,
        )
        self._stream.start()

    @property
    def active(self) -> bool:
        return self._stream is not None

    def _callback(self, indata, outdata, frames, time, status) -> None:  # noqa: ARG002
        if self.muted:
            outdata.fill(0)
        else:
            outdata[:] = indata * self.volume

    def stop(self) -> None:
        # The lock is shared with the device list refresh, which restarts the
        # sound library and must not do so while a stream is being closed.
        with self._lock:
            stream, self._stream = self._stream, None
            if stream is not None:
                try:
                    stream.stop()
                    stream.close()
                except Exception:
                    pass
