import pytest

from console_viewer.rules import (
    AudioDevice,
    BlackDetector,
    VideoDevice,
    choose_audio,
    find_audio_by_name,
    fit_rect,
    looks_like_capture,
    rank_videos,
)


@pytest.mark.parametrize("name", [
    "USB3.0 HD Video Capture",
    "USB Video",
    "USB Video (534d:2109)",
    "Elgato HD60 X",
    "Cam Link 4K",
    "AVerMedia Live Gamer MINI",
    "HDMI Capture",
    "Game Capture HD60 S",
])
def test_capture_card_names_are_recognised(name):
    assert looks_like_capture(name)


@pytest.mark.parametrize("name", [
    "HP True Vision HD Camera",
    "Integrated Webcam",
    "USB2.0 HD UVC WebCam",
    "FaceTime HD Camera",
    "Logitech BRIO",
    "USB Video Camera",
    "",
])
def test_webcam_names_are_not(name):
    assert not looks_like_capture(name)


WEBCAM = VideoDevice(0, "HP True Vision HD Camera", "cam")
CARD = VideoDevice(1, "USB3.0 HD Video Capture", "card")
CARD2 = VideoDevice(2, "Elgato HD60 X", "card2")


def test_ranking_prefers_capture_cards_and_keeps_order_otherwise():
    assert rank_videos([WEBCAM, CARD, CARD2]) == [CARD, CARD2, WEBCAM]


def test_ranking_puts_the_named_device_first():
    assert rank_videos([WEBCAM, CARD, CARD2], wanted_name="elgato")[0] == CARD2
    assert rank_videos([WEBCAM, CARD], wanted_name="true vision")[0] == WEBCAM


def test_ranking_puts_the_requested_device_above_everything():
    assert rank_videos([WEBCAM, CARD, CARD2], prefer_key="cam", wanted_name="elgato")[0] == WEBCAM


def test_ranking_of_nothing_is_nothing():
    assert rank_videos([]) == []


MIC = AudioDevice(1, "Microphone Array (Intel Smart Sound Technology)")


def test_webcam_never_gets_sound_automatically():
    webcam_mic = AudioDevice(2, "Microphone (HP True Vision HD Camera)")
    assert choose_audio(WEBCAM, [MIC, webcam_mic]) is None


def test_sound_named_after_the_card_is_chosen():
    sound = AudioDevice(3, "Digital Audio Interface (USB3.0 HD Video Capture)")
    other_card_sound = AudioDevice(4, "HDMI (Cam Link 4K)")
    assert choose_audio(CARD, [MIC, other_card_sound, sound]) == sound


def test_cheap_card_sound_is_chosen_by_its_usual_name():
    cheap = VideoDevice(5, "USB Video", "ms")
    sound = AudioDevice(6, "Digital Audio Interface (USB Digital Audio)")
    assert choose_audio(cheap, [MIC, sound]) == sound


def test_capture_card_with_no_matching_sound_gets_none():
    assert choose_audio(CARD, [MIC]) is None
    assert choose_audio(CARD, []) is None


def test_find_audio_by_name_ignores_case():
    assert find_audio_by_name("microphone", [MIC]) == MIC
    assert find_audio_by_name("nothing", [MIC]) is None


def test_black_picture_needs_ten_seconds():
    detector = BlackDetector()
    assert detector.update(0, 0.0) is False
    assert detector.update(3, 9.9) is False
    assert detector.update(0, 10.0) is True


def test_dark_grey_with_noise_counts_as_black():
    detector = BlackDetector()
    detector.update(16, 0.0)
    assert detector.update(20, 11.0) is True


def test_one_bright_frame_starts_the_wait_again():
    detector = BlackDetector()
    detector.update(0, 0.0)
    assert detector.update(200, 9.0) is False
    assert detector.update(0, 12.0) is False
    assert detector.update(0, 22.0) is True


def test_fit_keeps_the_whole_picture_with_bars():
    assert fit_rect(1920, 1080, 1280, 720) == (0, 0, 1280, 720)
    assert fit_rect(1920, 1080, 1000, 1000) == (0, 219, 1000, 562)   # bars above and below
    assert fit_rect(1280, 720, 2000, 720) == (360, 0, 1280, 720)     # bars left and right


def test_fill_covers_the_window_and_overflows():
    x, y, w, h = fit_rect(1920, 1080, 1000, 1000, fill=True)
    assert (w, h) == (1778, 1000) and x < 0 and y == 0


def test_fit_survives_nonsense_sizes():
    assert fit_rect(0, 0, 100, 100) == (0, 0, 100, 100)
    assert fit_rect(1920, 1080, 0, 0) == (0, 0, 1, 1)
