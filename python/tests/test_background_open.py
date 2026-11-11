"""Opening devices on another thread, the way the real app does."""

import threading
import time

import pytest

from console_viewer.viewer import Options, Viewer

from conftest import CARD, CARD_SOUND, MIC, WEBCAM, FakeBackend, wait_for


class SlowBackend(FakeBackend):
    """Opening takes a moment, as a real camera does, and can be held back."""

    def __init__(self, folder):
        super().__init__(folder)
        self.gate = threading.Event()
        self.gate.set()
        self.open_calls = []

    def open_video(self, device):
        self.open_calls.append(device)
        self.gate.wait(5)
        time.sleep(0.05)
        return super().open_video(device)


@pytest.fixture
def slow(tmp_path):
    return SlowBackend(tmp_path)


@pytest.fixture
def viewer(slow, clock):
    made = Viewer(slow, Options(background_open=True), clock)
    yield made
    slow.gate.set()
    made.close()


def live(viewer):
    return wait_for(lambda: (viewer.tick(), viewer.state == Viewer.LIVE)[1])


def test_the_window_stays_free_while_a_device_opens(viewer, slow):
    slow.gate.clear()
    started = time.monotonic()
    viewer.update_devices([CARD], [CARD_SOUND])
    assert time.monotonic() - started < 0.5, "update_devices must not wait for the device"
    assert viewer.state == Viewer.CONNECTING
    assert viewer.render(640, 360).shape == (360, 640, 3)
    slow.gate.set()
    assert live(viewer)
    assert viewer.current == CARD and viewer.audio_device == CARD_SOUND


def test_device_changes_while_connecting_do_not_open_it_twice(viewer, slow):
    slow.gate.clear()
    viewer.update_devices([CARD], [CARD_SOUND])
    viewer.update_devices([CARD], [CARD_SOUND])
    viewer.update_devices([CARD, WEBCAM], [CARD_SOUND, MIC])
    slow.gate.set()
    assert live(viewer)
    assert slow.open_calls == [CARD]


def test_capture_card_plugged_in_while_the_webcam_is_opening_takes_over(viewer, slow):
    slow.gate.clear()
    viewer.update_devices([WEBCAM], [MIC])
    viewer.update_devices([WEBCAM, CARD], [MIC, CARD_SOUND])
    slow.gate.set()
    assert wait_for(lambda: (viewer.tick(), viewer.current == CARD and viewer.state == Viewer.LIVE)[1])
    assert viewer.audio_device == CARD_SOUND
    assert slow.sources[0][0] == WEBCAM and slow.sources[0][1].released


def test_everything_busy_reports_an_error_and_retries(viewer, slow, clock):
    slow.busy.add(CARD.key)
    viewer.update_devices([CARD], [CARD_SOUND])
    assert wait_for(lambda: (viewer.tick(), viewer.state == Viewer.ERROR)[1])
    slow.busy.clear()
    clock.advance(3.1)
    assert live(viewer)


def test_closing_while_a_device_is_opening_releases_it(slow, clock):
    viewer = Viewer(slow, Options(background_open=True), clock)
    slow.gate.clear()
    viewer.update_devices([CARD], [CARD_SOUND])
    viewer.close()
    slow.gate.set()
    assert wait_for(lambda: len(slow.sources) == 1 and slow.sources[0][1].released)
    viewer.tick()
    assert viewer.state != Viewer.LIVE and viewer.grabber is None


def test_an_open_that_was_replaced_is_thrown_away(viewer, slow):
    viewer.update_devices([WEBCAM, CARD], [MIC, CARD_SOUND])
    assert live(viewer)
    slow.gate.clear()
    viewer.handle_key(ord("d"))   # starts opening the webcam
    viewer.handle_key(ord("d"))   # ignored: still connecting
    assert viewer.state == Viewer.CONNECTING
    viewer.connect(prefer=CARD.key)  # a newer request replaces it
    slow.gate.set()
    assert wait_for(lambda: (viewer.tick(), viewer.current == CARD and viewer.state == Viewer.LIVE)[1])
    assert wait_for(lambda: all(s.released for d, s in slow.sources if d == WEBCAM))
    live_sources = [s for _, s in slow.sources if not s.released]
    assert len(live_sources) == 1
