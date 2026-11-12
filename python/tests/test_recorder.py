"""The clip recorder on its own."""

import time

import numpy as np

from console_viewer.recorder import ClipRecorder

from test_viewer import read_clip


def picture(width=320, height=180, shade=120):
    frame = np.full((height, width, 3), shade, np.uint8)
    frame[20:60, 20:60] = 255
    return frame


def test_clip_plays_at_the_speed_it_was_recorded(tmp_path):
    import av

    path = tmp_path / "clip.mp4"
    recorder = ClipRecorder(path, 320, 180, 60)
    started = time.monotonic()
    for i in range(30):                 # 30 pictures spread over about a second
        recorder.write(picture(shade=i * 8))
        time.sleep(1 / 30)
    took = time.monotonic() - started
    recorder.release()
    with av.open(str(path)) as clip:
        length = float(clip.duration) / av.time_base
    assert read_clip(path)[0] == 30
    assert abs(length - took) < 0.35, f"recorded for {took:.2f} s but the clip lasts {length:.2f} s"


def test_odd_sizes_are_trimmed_not_refused(tmp_path):
    path = tmp_path / "odd.mp4"
    recorder = ClipRecorder(path, 321, 181, 30)
    for _ in range(5):
        recorder.write(picture(321, 181))
    recorder.release()
    assert read_clip(path)[0] == 5


def test_mono_sound_is_recorded(tmp_path):
    path = tmp_path / "mono.mp4"
    recorder = ClipRecorder(path, 320, 180, 30, audio=(44100, 1))
    for _ in range(10):
        recorder.write(picture())
        recorder.write_audio(np.zeros((4410, 1), np.float32))
    recorder.release()
    pictures, seconds = read_clip(path)
    assert pictures == 10 and 0.9 <= seconds <= 1.1


def test_a_slow_pc_drops_pictures_from_the_clip_instead_of_blocking(tmp_path):
    recorder = ClipRecorder(tmp_path / "busy.mp4", 1920, 1080, 60, queue_size=2)
    big = picture(1920, 1080)
    started = time.monotonic()
    for _ in range(60):
        recorder.write(big)
    assert time.monotonic() - started < 1.0, "write() must never wait for the encoder"
    recorder.release()
    assert recorder.dropped > 0
    assert read_clip(tmp_path / "busy.mp4")[0] >= 1


def test_release_twice_and_writing_afterwards_are_harmless(tmp_path):
    recorder = ClipRecorder(tmp_path / "done.mp4", 320, 180, 30)
    recorder.write(picture())
    recorder.release()
    recorder.release()
    recorder.write(picture())
    assert read_clip(tmp_path / "done.mp4")[0] == 1
