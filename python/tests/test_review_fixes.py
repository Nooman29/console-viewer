"""One test for each problem found when the code was reviewed."""

import threading
import time

import cv2
import numpy as np

from console_viewer.audio import AudioPassthrough
from console_viewer.capture import FrameGrabber
from console_viewer.recorder import ClipRecorder
from console_viewer.rules import AudioDevice
from console_viewer.viewer import Viewer

from conftest import CARD, CARD_SOUND, MIC, FakeSource, wait_for, wait_for_frame
from test_capture_and_cli import FakeSoundLibrary
from test_viewer import key, read_clip


def picture(width=320, height=180):
    frame = np.full((height, width, 3), 90, np.uint8)
    frame[20:60, 20:60] = 255
    return frame


def finishes(action, seconds=5.0):
    """True if `action` returns in time instead of hanging."""
    worker = threading.Thread(target=action, daemon=True)
    worker.start()
    worker.join(seconds)
    return not worker.is_alive()


# ---------- A clip that fails part-way ----------

def break_encoder(recorder):
    def explode(when, image):
        raise OSError("disk full")
    recorder._encode_video = explode


def test_stopping_a_failed_clip_does_not_hang(tmp_path):
    recorder = ClipRecorder(tmp_path / "broken.mp4", 320, 180, 30, queue_size=2)
    break_encoder(recorder)
    for _ in range(50):
        recorder.write(picture())
    assert wait_for(lambda: recorder.error is not None)
    assert finishes(recorder.release), "release() hung after the clip failed"
    recorder.write(picture())  # ignored, and must not pile up
    assert len(recorder._pictures) == 0


def test_a_failed_clip_is_not_reported_as_saved(viewer, backend):
    viewer.update_devices([CARD], [CARD_SOUND])
    wait_for_frame(viewer)
    key(viewer, "r")
    break_encoder(viewer._recorder)
    assert wait_for(lambda: viewer._recorder.error is not None)
    assert finishes(lambda: key(viewer, "r"))
    assert viewer._toast == "The clip could not be saved."
    assert not viewer.recording


# ---------- Sound must survive a slow PC ----------

def test_sound_is_kept_when_pictures_have_to_be_dropped(tmp_path):
    path = tmp_path / "slow.mp4"
    recorder = ClipRecorder(path, 1920, 1080, 60, audio=(48000, 2), queue_size=2)
    big = picture(1920, 1080)
    block = np.zeros((480, 2), np.float32)
    for _ in range(100):               # one second of sound, far more pictures than fit
        recorder.write(big)
        recorder.write(big)
        recorder.write_audio(block)
    recorder.release()
    assert recorder.dropped > 0, "the test needs pictures to have been dropped"
    pictures, seconds = read_clip(path)
    assert pictures >= 1
    assert 0.9 <= seconds <= 1.1, f"sent 1.0 s of sound, the clip has {seconds:.2f} s"


# ---------- Recording should not make the window redraw non-stop ----------

def test_recording_only_asks_for_a_redraw_when_the_timer_moves(viewer, clock):
    viewer.update_devices([CARD], [CARD_SOUND])
    wait_for_frame(viewer)
    key(viewer, "r")
    viewer.tick()
    viewer.render(320, 180)
    assert viewer.dirty is False
    viewer.tick()
    assert viewer.dirty is False, "nothing changed, so nothing to redraw"
    clock.advance(1.0)
    viewer.tick()
    assert viewer.dirty is True


# ---------- The A key ----------

def test_a_key_steps_past_a_sound_input_that_will_not_open(viewer, backend):
    backend.broken_audio.add(MIC.name)
    viewer.update_devices([CARD], [MIC, CARD_SOUND])
    key(viewer, "a")    # card sound -> off (it was last in the list)
    key(viewer, "a")    # off -> microphone, which will not open
    assert viewer.audio is None
    assert "Could not open" in viewer._toast
    key(viewer, "a")    # must move on to the card sound, not retry the microphone
    assert viewer.audio_device == CARD_SOUND


def test_changing_sound_source_while_recording_keeps_sound_in_the_clip(viewer, backend):
    twin = AudioDevice(5, "Second input")
    viewer.update_devices([CARD], [CARD_SOUND, twin])
    wait_for_frame(viewer)
    key(viewer, "r")
    assert backend.sounds[0].on_audio is not None
    key(viewer, "a")                      # switch to the second input (same format)
    assert viewer.audio_device == twin
    assert backend.sounds[1].on_audio is not None, "the new source must feed the clip"
    key(viewer, "r")


# ---------- Screenshots in a folder with accented letters ----------

def test_screenshot_saves_into_a_folder_with_accented_letters(viewer, backend, tmp_path):
    folder = tmp_path / "José Peña"
    folder.mkdir()
    backend.folder = folder
    viewer.update_devices([CARD], [CARD_SOUND])
    wait_for_frame(viewer)
    saved = viewer.screenshot()
    assert saved is not None and saved.exists()
    image = cv2.imdecode(np.fromfile(str(saved), np.uint8), cv2.IMREAD_COLOR)
    assert image.shape == (180, 320, 3)


# ---------- Sound that dies by itself ----------

def test_passthrough_reports_a_stream_that_stopped_by_itself():
    library = FakeSoundLibrary()
    sound = AudioPassthrough(AudioDevice(4, "HDMI", 48000, 2), sd=library)
    assert sound.active
    library.stream.stopped = True         # the device went away under it
    assert not sound.active


def test_dead_sound_is_dropped_and_started_again(viewer, backend):
    viewer.update_devices([CARD], [CARD_SOUND])
    first = backend.sounds[0]
    first.active = False                  # the card's sound input dropped out
    viewer.tick()
    assert viewer.audio is None and first.stopped
    viewer.update_devices([CARD], [CARD_SOUND])
    assert viewer.audio_device == CARD_SOUND and len(backend.sounds) == 2


# ---------- A card that stays plugged in but stops sending ----------

def test_a_stalled_device_is_noticed_by_the_clock():
    class SlowToFail(FakeSource):
        def read(self):
            if self.alive:
                return super().read()
            time.sleep(0.2)               # like DirectShow waiting on a silent device
            return None

    source = SlowToFail()
    grabber = FrameGrabber(source, max_failures=1000, stall_seconds=0.6).start()
    assert wait_for(lambda: grabber.latest()[1] > 0)
    source.alive = False
    started = time.monotonic()
    assert wait_for(grabber.failed.is_set, 5)
    assert time.monotonic() - started < 2.0, "should give up after about 0.6 s, not 1000 reads"
    grabber.stop()
