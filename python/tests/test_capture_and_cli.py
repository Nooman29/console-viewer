"""The capture thread, the sound passthrough and the command line."""

import numpy as np

from console_viewer.__main__ import build_parser, list_devices, run
from console_viewer.audio import AudioPassthrough
from console_viewer.backend import DemoBackend, timestamp
from console_viewer.capture import DemoSource, FrameGrabber
from console_viewer.rules import AudioDevice
from console_viewer.viewer import Viewer

from conftest import FakeSource, wait_for


def test_grabber_keeps_the_newest_frame_and_counts():
    grabber = FrameGrabber(FakeSource()).start()
    assert wait_for(lambda: grabber.latest()[1] >= 5)
    frame, _ = grabber.latest()
    assert frame.shape == (180, 320, 3)
    grabber.stop()
    assert grabber.source.released


def test_grabber_reports_a_device_that_stops_delivering():
    source = FakeSource()
    grabber = FrameGrabber(source, max_failures=5, retry_delay=0.001).start()
    assert wait_for(lambda: grabber.latest()[1] > 0)
    assert not grabber.failed.is_set()
    source.alive = False
    assert wait_for(grabber.failed.is_set)
    grabber.stop()


def test_a_short_gap_is_not_a_disconnect():
    source = FakeSource()
    grabber = FrameGrabber(source, max_failures=1000, retry_delay=0.001).start()
    assert wait_for(lambda: grabber.latest()[1] > 0)
    source.alive = False
    wait_for(lambda: False, 0.05)
    source.alive = True
    count = grabber.latest()[1]
    assert wait_for(lambda: grabber.latest()[1] > count)
    assert not grabber.failed.is_set()
    grabber.stop()


def test_a_read_that_raises_counts_as_a_failure():
    class Exploding(FakeSource):
        def read(self):
            raise RuntimeError("device vanished")

    grabber = FrameGrabber(Exploding(), max_failures=3, retry_delay=0.001).start()
    assert wait_for(grabber.failed.is_set)
    grabber.stop()


def test_a_broken_frame_callback_does_not_stop_the_picture():
    def explode(frame):
        raise RuntimeError("recorder broke")

    grabber = FrameGrabber(FakeSource(), on_frame=explode).start()
    assert wait_for(lambda: grabber.latest()[1] >= 5)
    assert not grabber.failed.is_set()
    grabber.stop()


def test_demo_source_makes_moving_pictures():
    source = DemoSource(320, 180, fps=200)
    first, second = source.read(), source.read()
    assert first.shape == (180, 320, 3)
    assert (first != second).any()


class FakeStream:
    def __init__(self, **settings):
        self.settings = settings
        self.started = self.stopped = self.closed = False

    def start(self):
        self.started = True

    def stop(self):
        self.stopped = True

    def close(self):
        self.closed = True


class FakeSoundLibrary:
    """Stands in for the sounddevice module."""

    def __init__(self):
        self.stream = None

    def query_devices(self, index):
        return {"hostapi": 0}

    def query_hostapis(self, index):
        return {"name": "Windows WASAPI", "default_output_device": 7}

    def WasapiSettings(self, **settings):
        return ("wasapi", settings)

    def Stream(self, **settings):
        self.stream = FakeStream(**settings)
        return self.stream


def test_sound_goes_from_the_card_to_the_default_output():
    library = FakeSoundLibrary()
    sound = AudioPassthrough(AudioDevice(4, "HDMI (Cam Link 4K)", 48000, 2), sd=library)
    settings = library.stream.settings
    assert settings["device"] == (4, 7)
    assert settings["samplerate"] == 48000 and settings["channels"] == 2
    assert settings["extra_settings"] == ("wasapi", {"auto_convert": True})
    assert library.stream.started and sound.active
    sound.stop()
    assert library.stream.stopped and library.stream.closed and not sound.active
    sound.stop()  # stopping twice is harmless


def test_sound_callback_applies_volume_and_mute():
    sound = AudioPassthrough(AudioDevice(4, "HDMI", 48000, 2), sd=FakeSoundLibrary())
    incoming = np.full((8, 2), 0.5, np.float32)
    outgoing = np.zeros((8, 2), np.float32)
    sound._callback(incoming, outgoing, 8, None, None)
    assert np.allclose(outgoing, 0.5)
    sound.volume = 0.5
    sound._callback(incoming, outgoing, 8, None, None)
    assert np.allclose(outgoing, 0.25)
    sound.muted = True
    sound._callback(incoming, outgoing, 8, None, None)
    assert not outgoing.any()


def test_mono_sound_input_is_opened_as_mono():
    library = FakeSoundLibrary()
    AudioPassthrough(AudioDevice(4, "HDMI", 44100, 1), sd=library)
    assert library.stream.settings["channels"] == 1


def test_command_line_defaults():
    args = build_parser().parse_args([])
    assert (args.width, args.height, args.fps) == (1920, 1080, 60)
    assert not args.demo and not args.no_audio and args.device is None


def test_list_prints_devices(capsys):
    list_devices(DemoBackend())
    out = capsys.readouterr().out
    assert "Demo HDMI Capture  (capture card)" in out
    assert "Sound inputs:" in out and "none found" in out


def test_timestamp_is_local_time_and_safe_in_a_file_name():
    stamp = timestamp(0)
    assert len(stamp) == 15 and stamp[8] == "-" and stamp.replace("-", "").isdigit()


class FakeWindow:
    """A window that exists only in memory and presses keys on a schedule."""

    def __init__(self, keys=()):
        self.keys = list(keys)
        self.shown = 0
        self.fullscreen_calls = []
        self.closed = False

    def size(self):
        return 640, 360

    def show(self, image):
        assert image.shape == (360, 640, 3)
        self.shown += 1

    def poll_key(self, milliseconds):
        if self.shown >= 20 and self.keys:
            return ord(self.keys.pop(0))
        return -1

    def set_fullscreen(self, on):
        self.fullscreen_calls.append(on)

    def is_open(self):
        return True

    def close(self):
        self.closed = True


def test_main_loop_shows_the_demo_and_quits_on_q(tmp_path):
    backend = DemoBackend(fps=120)
    backend.save_folder = lambda: tmp_path
    viewer = Viewer(backend)
    window = FakeWindow(keys="fsq")
    assert run(viewer, backend, window, scan_seconds=0.05) == 0
    assert window.shown >= 20 and window.closed
    assert window.fullscreen_calls == [True]
    assert len(list(tmp_path.glob("*.png"))) == 1
    assert viewer.grabber is None, "the device must be released on exit"


def test_main_loop_stops_when_the_window_is_closed(tmp_path):
    backend = DemoBackend(fps=120)
    viewer = Viewer(backend)
    window = FakeWindow()
    window.is_open = lambda: window.shown < 10
    assert run(viewer, backend, window, scan_seconds=0.05) == 0
    assert window.closed
