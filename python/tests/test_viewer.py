"""How the viewer behaves, checked with pretend devices."""

import cv2

from console_viewer.viewer import ESC, Options, Viewer

from conftest import (
    CARD,
    CARD2,
    CARD_SOUND,
    CHEAP_CARD,
    CHEAP_CARD_SOUND,
    MIC,
    WEBCAM,
    wait_for,
    wait_for_frame,
)


def key(viewer, char):
    return viewer.handle_key(ord(char))


# ---------- Choosing what to open ----------

def test_webcam_only_shows_the_picture_and_never_plays_the_microphone(viewer, backend):
    viewer.update_devices([WEBCAM], [MIC])
    assert viewer.state == Viewer.LIVE
    assert viewer.current == WEBCAM
    assert backend.sounds == []
    assert backend.awake == [True]


def test_capture_card_is_preferred_and_gets_its_own_sound(viewer, backend):
    viewer.update_devices([WEBCAM, CARD], [MIC, CARD_SOUND])
    assert viewer.current == CARD
    assert viewer.audio_device == CARD_SOUND
    assert len(backend.sources) == 1


def test_cheap_card_gets_sound(viewer):
    viewer.update_devices([WEBCAM, CHEAP_CARD], [MIC, CHEAP_CARD_SOUND])
    assert viewer.current == CHEAP_CARD
    assert viewer.audio_device == CHEAP_CARD_SOUND


def test_device_named_on_the_command_line_wins(backend, clock):
    viewer = Viewer(backend, Options(wanted_video="true vision"), clock)
    viewer.update_devices([WEBCAM, CARD], [MIC, CARD_SOUND])
    assert viewer.current == WEBCAM
    assert viewer.audio is None
    viewer.close()


def test_no_audio_option_keeps_sound_off(backend, clock):
    viewer = Viewer(backend, Options(no_audio=True), clock)
    viewer.update_devices([CARD], [CARD_SOUND])
    assert viewer.state == Viewer.LIVE and backend.sounds == []
    viewer.close()


def test_sound_named_on_the_command_line_is_used(backend, clock):
    viewer = Viewer(backend, Options(wanted_audio="microphone"), clock)
    viewer.update_devices([CARD], [MIC, CARD_SOUND])
    assert viewer.audio_device == MIC
    viewer.close()


# ---------- Things going wrong ----------

def test_busy_device_falls_through_to_the_next(viewer, backend):
    backend.busy.add(CARD.key)
    viewer.update_devices([WEBCAM, CARD], [MIC, CARD_SOUND])
    assert viewer.state == Viewer.LIVE and viewer.current == WEBCAM


def test_everything_busy_shows_an_error_then_retries_by_itself(viewer, backend, clock):
    backend.busy.add(CARD.key)
    viewer.update_devices([CARD], [CARD_SOUND])
    assert viewer.state == Viewer.ERROR
    assert "Another app" in viewer.detail
    viewer.tick()
    assert viewer.state == Viewer.ERROR, "too early to retry"
    backend.busy.clear()
    clock.advance(3.1)
    viewer.tick()
    assert viewer.state == Viewer.LIVE


def test_sound_that_will_not_open_leaves_the_picture_and_is_not_retried(viewer, backend):
    backend.broken_audio.add(CARD_SOUND.name)
    viewer.update_devices([CARD], [CARD_SOUND])
    assert viewer.state == Viewer.LIVE and viewer.audio is None
    viewer.update_devices([CARD], [CARD_SOUND])
    viewer.update_devices([CARD], [CARD_SOUND])
    assert len(backend.sources) == 1, "the picture must not be reopened"


def test_no_devices_waits_then_connects_when_one_arrives(viewer):
    viewer.update_devices([], [])
    assert viewer.state == Viewer.WAITING
    assert "Plug your HDMI capture card" in viewer.detail
    viewer.update_devices([CARD], [CARD_SOUND])
    assert viewer.state == Viewer.LIVE and viewer.audio_device == CARD_SOUND


# ---------- Plugging and unplugging ----------

def unplug(viewer, backend):
    backend.sources[-1][1].alive = False
    assert wait_for(lambda: (viewer.tick(), viewer.state == Viewer.DISCONNECTED)[1])


def test_unplug_releases_everything(viewer, backend):
    viewer.update_devices([WEBCAM, CARD], [MIC, CARD_SOUND])
    wait_for_frame(viewer)
    unplug(viewer, backend)
    assert viewer.message == "Device disconnected"
    assert backend.sources[0][1].released
    assert backend.sounds[0].stopped
    assert backend.awake[-1] is False
    viewer.update_devices([WEBCAM], [MIC])
    assert viewer.state == Viewer.DISCONNECTED, "must not fall back to the webcam by itself"


def test_unplug_then_replug_reconnects(viewer, backend):
    viewer.update_devices([WEBCAM, CARD], [MIC, CARD_SOUND])
    unplug(viewer, backend)
    viewer.update_devices([WEBCAM], [MIC])
    viewer.update_devices([WEBCAM, CARD], [MIC, CARD_SOUND])
    assert viewer.state == Viewer.LIVE and viewer.current == CARD
    assert viewer.audio_device == CARD_SOUND


def test_quick_replug_that_the_scan_never_saw_reconnects(viewer, backend):
    viewer.update_devices([WEBCAM, CARD], [MIC, CARD_SOUND])
    unplug(viewer, backend)
    viewer.update_devices([WEBCAM, CARD], [MIC, CARD_SOUND])  # the list looks unchanged
    assert viewer.state == Viewer.LIVE and viewer.current == CARD


def test_capture_card_takes_over_from_the_webcam(viewer, backend):
    viewer.update_devices([WEBCAM], [MIC])
    viewer.update_devices([WEBCAM, CARD], [MIC, CARD_SOUND])
    assert viewer.current == CARD and viewer.audio_device == CARD_SOUND
    assert backend.sources[0][1].released, "the webcam must be let go"


def test_second_capture_card_does_not_interrupt(viewer, backend):
    viewer.update_devices([CARD], [CARD_SOUND])
    viewer.update_devices([CARD, CARD2], [CARD_SOUND])
    assert viewer.current == CARD and len(backend.sources) == 1


def test_late_sound_is_added_without_reopening_the_picture(viewer, backend):
    viewer.update_devices([CARD], [])
    assert viewer.audio is None
    viewer.update_devices([CARD], [CARD_SOUND])
    assert viewer.audio_device == CARD_SOUND and len(backend.sources) == 1


def test_a_hiccup_in_the_device_list_does_not_stop_the_picture(viewer, backend):
    viewer.update_devices([CARD], [CARD_SOUND])
    wait_for_frame(viewer)
    viewer.update_devices([], [])
    viewer.tick()
    assert viewer.state == Viewer.LIVE and not backend.sounds[0].stopped


# ---------- The black-picture tip ----------

def test_black_picture_shows_the_tip_after_ten_seconds_and_clears(viewer, backend, clock):
    viewer.update_devices([CARD], [CARD_SOUND])
    source = backend.sources[0][1]
    source.black = True
    wait_for_frame(viewer)
    assert wait_for(lambda: int(viewer.grabber.latest()[0].max()) == 0)
    viewer.tick()
    assert viewer.show_hdcp_tip is False
    clock.advance(11)
    viewer.tick()
    assert viewer.show_hdcp_tip is True
    source.black = False
    assert wait_for(lambda: int(viewer.grabber.latest()[0].max()) > 0)
    clock.advance(2)
    viewer.tick()
    assert viewer.show_hdcp_tip is False


# ---------- Keys ----------

def test_mute_and_volume_reach_the_sound(viewer, backend):
    viewer.update_devices([CARD], [CARD_SOUND])
    sound = backend.sounds[0]
    key(viewer, "m")
    assert sound.muted is True
    key(viewer, "M")
    assert sound.muted is False
    key(viewer, "-")
    key(viewer, "-")
    assert sound.volume == 0.8
    for _ in range(5):
        key(viewer, "+")
    assert sound.volume == 1.0


def test_mute_set_before_connecting_is_kept(viewer, backend):
    key(viewer, "m")
    viewer.update_devices([CARD], [CARD_SOUND])
    assert backend.sounds[0].muted is True


def test_keys_that_mean_nothing_change_nothing(viewer, backend):
    viewer.update_devices([CARD], [CARD_SOUND])
    for code in (255, 200, 0, ord("x"), -1):
        assert viewer.handle_key(code) is True
    assert viewer.volume == 1.0 and not viewer.muted and not viewer.fill


def test_fill_fullscreen_and_help_toggle(viewer):
    key(viewer, "z")
    key(viewer, "f")
    key(viewer, "h")
    assert viewer.fill and viewer.fullscreen and viewer.show_help


def test_escape_leaves_full_screen_first_then_quits(viewer):
    key(viewer, "f")
    assert viewer.handle_key(ESC) is True and viewer.fullscreen is False
    assert viewer.handle_key(ESC) is False


def test_q_quits(viewer):
    assert key(viewer, "q") is False


def test_d_steps_through_video_devices(viewer):
    viewer.update_devices([WEBCAM, CARD], [MIC, CARD_SOUND])
    assert viewer.current == CARD
    key(viewer, "d")
    assert viewer.current == WEBCAM and viewer.audio is None
    key(viewer, "d")
    assert viewer.current == CARD


def test_a_steps_through_sound_inputs_and_then_off(viewer, backend):
    viewer.update_devices([CARD], [MIC, CARD_SOUND])
    assert viewer.audio_device == CARD_SOUND
    key(viewer, "a")
    assert viewer.audio is None and backend.sounds[0].stopped
    viewer.update_devices([CARD], [MIC, CARD_SOUND])
    assert viewer.audio is None, "sound switched off by hand must stay off"
    key(viewer, "a")
    assert viewer.audio_device == MIC


# ---------- Screenshots and clips ----------

def test_screenshot_saves_a_png(viewer, backend):
    viewer.update_devices([CARD], [CARD_SOUND])
    wait_for_frame(viewer)
    key(viewer, "s")
    files = list(backend.folder.glob("console-viewer-*.png"))
    assert len(files) == 1
    assert cv2.imread(str(files[0])).shape == (180, 320, 3)


def test_two_screenshots_in_one_second_do_not_overwrite(viewer, backend):
    viewer.update_devices([CARD], [CARD_SOUND])
    wait_for_frame(viewer)
    assert viewer.screenshot() != viewer.screenshot()
    assert len(list(backend.folder.glob("*.png"))) == 2


def count_frames(path):
    video = cv2.VideoCapture(str(path))
    count = 0
    while video.read()[0]:
        count += 1
    video.release()
    return count


def test_recording_saves_a_playable_clip(viewer, backend):
    viewer.update_devices([CARD], [CARD_SOUND])
    wait_for_frame(viewer)
    key(viewer, "r")
    assert viewer.recording
    start = viewer.frame_count()
    assert wait_for(lambda: viewer.frame_count() >= start + 30)
    key(viewer, "r")
    assert not viewer.recording
    clips = list(backend.folder.glob("console-viewer-*.mp4"))
    assert len(clips) == 1
    assert count_frames(clips[0]) >= 30


def test_unplugging_while_recording_keeps_the_clip(viewer, backend):
    viewer.update_devices([CARD], [CARD_SOUND])
    wait_for_frame(viewer)
    key(viewer, "r")
    start = viewer.frame_count()
    assert wait_for(lambda: viewer.frame_count() >= start + 20)
    unplug(viewer, backend)
    assert not viewer.recording
    clips = list(backend.folder.glob("*.mp4"))
    assert len(clips) == 1 and count_frames(clips[0]) >= 20


def test_nothing_to_record_or_capture_before_a_picture(viewer):
    viewer.update_devices([], [])
    key(viewer, "r")
    key(viewer, "s")
    assert not viewer.recording and viewer.screenshot() is None


# ---------- Drawing ----------

def test_render_matches_the_window_in_every_state(viewer, backend):
    for size in ((640, 360), (333, 777), (1, 1)):
        image = viewer.render(*size)
        assert image.shape == (max(size[1], 16), max(size[0], 16), 3)
    viewer.update_devices([], [])
    assert viewer.render(800, 450).shape == (450, 800, 3)
    viewer.update_devices([CARD], [CARD_SOUND])
    wait_for_frame(viewer)
    key(viewer, "h")
    key(viewer, "r")
    viewer.show_hdcp_tip = True
    for fill in (False, True):
        viewer.fill = fill
        for size in ((640, 360), (320, 180), (200, 600), (900, 200)):
            image = viewer.render(*size)
            assert image.shape == (size[1], size[0], 3) and image.dtype.name == "uint8"


def test_fit_shows_bars_and_fill_does_not(viewer):
    viewer.update_devices([CARD], [CARD_SOUND])
    wait_for_frame(viewer)
    bars = viewer.render(320, 400)
    assert bars[0].max() == 0 and bars[200].max() > 0
    viewer.fill = True
    assert viewer.render(320, 400)[0].max() > 0


def test_render_does_not_change_the_captured_frame(viewer):
    viewer.update_devices([CARD], [CARD_SOUND])
    wait_for_frame(viewer)
    viewer.toast("hello")
    frame = viewer.grabber.latest()[0]
    before = frame.copy()
    viewer.render(320, 180)  # same size as the picture: the fast path
    assert (frame == before).all()
