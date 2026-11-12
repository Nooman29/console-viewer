// Tests for the core logic, using pretend devices. No hardware, no Windows.
//
// A tiny home-made runner keeps the project free of dependencies:
//   TEST(name) { ... CHECK(condition); ... }
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "../src/core/controller.h"
#include "../src/core/rules.h"

using namespace cv;

namespace {

struct TestCase {
    const char* name;
    std::function<void()> run;
};
std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}
int failures = 0;
bool currentFailed = false;

struct Registrar {
    Registrar(const char* name, std::function<void()> run) { registry().push_back({name, std::move(run)}); }
};

#define TEST(name)                                         \
    static void test_##name();                             \
    static Registrar registrar_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::printf("    FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            currentFailed = true;                                              \
        }                                                                      \
    } while (0)

// ---------- Pretend hardware ----------

const VideoDevice WEBCAM{"HP True Vision HD Camera", "cam1"};
const AudioDevice MIC{"Microphone Array (Intel Smart Sound Technology)", "mic1"};
const VideoDevice CARD{"USB3.0 HD Video Capture", "card1"};
const AudioDevice CARD_SOUND{"Digital Audio Interface (USB3.0 HD Video Capture)", "card1a"};
const VideoDevice CARD2{"Elgato HD60 X", "card2"};
// The most common cheap card: its sound input is not named after its picture.
const VideoDevice CHEAP_CARD{"USB Video", "ms1"};
const AudioDevice CHEAP_CARD_SOUND{"Digital Audio Interface (USB Digital Audio)", "ms1a"};

struct FakeBackend : Backend {
    struct Opened {
        VideoDevice device;
        bool* failed;
        bool* released;
    };
    struct Source : VideoSource {
        bool dead = false;
        bool* releasedFlag;
        explicit Source(bool* released) : releasedFlag(released) {}
        ~Source() override { *releasedFlag = true; }
        bool failed() const override { return dead; }
    };
    struct Sound : AudioOutput {
        FakeBackend& owner;
        std::size_t index;
        Sound(FakeBackend& o, std::size_t i) : owner(o), index(i) {}
        ~Sound() override { owner.soundStopped[index] = true; }
        void setVolume(float volume, bool muted) override {
            owner.soundVolume[index] = volume;
            owner.soundMuted[index] = muted;
        }
        bool failed() const override { return owner.soundDead[index]; }
    };

    std::vector<std::string> busy;         // video keys that refuse to open
    std::vector<std::string> brokenAudio;  // sound names that refuse to open
    std::vector<VideoDevice> opened;
    std::vector<Source*> sources;
    std::vector<bool*> released;
    std::vector<AudioDevice> sounds;
    std::vector<bool> soundStopped;
    std::vector<bool> soundMuted;
    std::vector<float> soundVolume;
    std::vector<bool> soundDead;           // set by a test to make a sound stop by itself
    int audioAttempts = 0;                 // every openAudio call, including refused ones
    std::vector<bool> awake;
    int screenshots = 0;

    ~FakeBackend() override {
        for (bool* flag : released) delete flag;
    }

    std::unique_ptr<VideoSource> openVideo(const VideoDevice& device, std::string& error) override {
        for (const auto& key : busy) {
            if (key == device.key) {
                error = "Could not open " + device.name + ". Another app may be using it.";
                return nullptr;
            }
        }
        bool* flag = new bool(false);
        released.push_back(flag);
        auto source = std::make_unique<Source>(flag);
        sources.push_back(source.get());
        opened.push_back(device);
        return source;
    }

    std::unique_ptr<AudioOutput> openAudio(const AudioDevice& device) override {
        ++audioAttempts;
        for (const auto& name : brokenAudio) {
            if (name == device.name) return nullptr;
        }
        sounds.push_back(device);
        soundStopped.push_back(false);
        soundMuted.push_back(false);
        soundVolume.push_back(1.0f);
        soundDead.push_back(false);
        return std::make_unique<Sound>(*this, sounds.size() - 1);
    }

    void keepAwake(bool on) override { awake.push_back(on); }
    std::string saveScreenshot() override {
        ++screenshots;
        return "Downloads";
    }
};

using State = Controller::State;

// Devices arrive, and the next tick opens one.
void plug(Controller& c, std::vector<VideoDevice> videos, std::vector<AudioDevice> audios, double now = 0) {
    c.updateDevices(std::move(videos), std::move(audios), now);
    c.tick(now);
}

void unplug(Controller& c, FakeBackend& b, double now = 0) {
    b.sources.back()->dead = true;
    c.tick(now);
}

// ---------- Rules ----------

TEST(capture_card_names_are_recognised) {
    for (const char* name : {"USB3.0 HD Video Capture", "USB Video", "USB Video (534d:2109)", "Elgato HD60 X",
                             "Cam Link 4K", "AVerMedia Live Gamer MINI", "HDMI Capture", "Game Capture HD60 S"}) {
        CHECK(looksLikeCapture(name));
    }
}

TEST(webcam_names_are_not) {
    for (const char* name : {"HP True Vision HD Camera", "Integrated Webcam", "USB2.0 HD UVC WebCam",
                             "FaceTime HD Camera", "Logitech BRIO", "USB Video Camera", ""}) {
        CHECK(!looksLikeCapture(name));
    }
}

TEST(ranking_prefers_capture_cards_and_keeps_order_otherwise) {
    const auto ranked = rankVideos({WEBCAM, CARD, CARD2});
    CHECK(ranked.size() == 3 && ranked[0] == CARD && ranked[1] == CARD2 && ranked[2] == WEBCAM);
    CHECK(rankVideos({}).empty());
}

TEST(ranking_puts_named_and_requested_devices_first) {
    CHECK(rankVideos({WEBCAM, CARD, CARD2}, "", "elgato")[0] == CARD2);
    CHECK(rankVideos({WEBCAM, CARD}, "", "true vision")[0] == WEBCAM);
    CHECK(rankVideos({WEBCAM, CARD, CARD2}, "cam1", "elgato")[0] == WEBCAM);
}

TEST(sound_is_matched_to_the_card_and_never_to_a_webcam) {
    const AudioDevice webcamMic{"Microphone (HP True Vision HD Camera)", "wm"};
    const AudioDevice otherCard{"HDMI (Cam Link 4K)", "cl"};
    CHECK(!chooseAudio(WEBCAM, {MIC, webcamMic}));
    CHECK(chooseAudio(CARD, {MIC, otherCard, CARD_SOUND}) == CARD_SOUND);
    CHECK(chooseAudio(CHEAP_CARD, {MIC, CHEAP_CARD_SOUND}) == CHEAP_CARD_SOUND);
    CHECK(!chooseAudio(CARD, {MIC}));
    CHECK(!chooseAudio(CARD, {}));
    CHECK(findAudioByName("microphone", {MIC}) == MIC);
    CHECK(!findAudioByName("nothing", {MIC}));
}

TEST(black_picture_needs_ten_seconds) {
    BlackDetector d;
    CHECK(!d.update(0, 0.0));
    CHECK(!d.update(3, 9.9));
    CHECK(d.update(0, 10.0));
    BlackDetector grey;
    grey.update(16, 0.0);
    CHECK(grey.update(20, 11.0));  // dark grey with noise still counts
    BlackDetector flicker;
    flicker.update(0, 0.0);
    CHECK(!flicker.update(200, 9.0));  // one bright frame starts the wait again
    CHECK(!flicker.update(0, 12.0));
    CHECK(flicker.update(0, 22.0));
}

TEST(fit_and_fill) {
    CHECK((fitRect(1920, 1080, 1280, 720) == Rect{0, 0, 1280, 720}));
    CHECK((fitRect(1920, 1080, 1000, 1000) == Rect{0, 218, 1000, 563}));  // bars above and below
    CHECK((fitRect(1280, 720, 2000, 720) == Rect{360, 0, 1280, 720}));    // bars left and right
    const Rect filled = fitRect(1920, 1080, 1000, 1000, true);
    CHECK(filled.w == 1778 && filled.h == 1000 && filled.x == -389 && filled.y == 0);
    CHECK((fitRect(0, 0, 100, 100) == Rect{0, 0, 100, 100}));
    CHECK((fitRect(1920, 1080, 0, 0) == Rect{0, 0, 1, 1}));
}

TEST(format_choice_prefers_size_then_smoothness) {
    // A USB 2.0 card: 1080p raw at 5 fps, 1080p MJPEG at 30, 720p MJPEG at 60.
    const std::vector<VideoFormat> usb2 = {
        {1920, 1080, 5, false}, {1920, 1080, 30, true}, {1280, 720, 60, true}, {640, 480, 60, false}};
    CHECK(pickFormat(usb2, 1920, 1080, 60) == 1);
    CHECK(pickFormat(usb2, 1280, 720, 60) == 2);
    // A USB 3.0 card: raw and MJPEG both at 1080p60. Raw costs less to show.
    const std::vector<VideoFormat> usb3 = {{1920, 1080, 60, true}, {1920, 1080, 60, false}, {3840, 2160, 30, false}};
    CHECK(pickFormat(usb3, 1920, 1080, 60) == 1);
    CHECK(pickFormat(usb3, 3840, 2160, 60) == 2);
    // Faster than asked for is no better than what was asked for.
    const std::vector<VideoFormat> fast = {{1280, 720, 120, true}, {1280, 720, 60, false}};
    CHECK(pickFormat(fast, 1920, 1080, 60) == 1);
    // Nothing small enough: take the smallest that is too big.
    const std::vector<VideoFormat> big = {{3840, 2160, 30, false}, {2560, 1440, 30, false}};
    CHECK(pickFormat(big, 1920, 1080, 60) == 1);
    CHECK(pickFormat({}, 1920, 1080, 60) == -1);
    CHECK(pickFormat({{0, 0, 30, false}}, 1920, 1080, 60) == -1);
}

// ---------- Choosing what to open ----------

TEST(webcam_only_shows_the_picture_and_never_plays_the_microphone) {
    FakeBackend b;
    Controller c(b, {});
    c.updateDevices({WEBCAM}, {MIC}, 0);
    CHECK(c.state() == State::Connecting);  // drawn before the open happens
    c.tick(0);
    CHECK(c.state() == State::Live);
    CHECK(c.current() && *c.current() == WEBCAM);
    CHECK(b.sounds.empty());
    CHECK(b.awake.size() == 1 && b.awake[0]);
}

TEST(capture_card_is_preferred_and_gets_its_own_sound) {
    FakeBackend b;
    Controller c(b, {});
    plug(c, {WEBCAM, CARD}, {MIC, CARD_SOUND});
    CHECK(c.current() && *c.current() == CARD);
    CHECK(c.audioDevice() && *c.audioDevice() == CARD_SOUND);
    CHECK(b.opened.size() == 1);
    FakeBackend b2;
    Controller cheap(b2, {});
    plug(cheap, {WEBCAM, CHEAP_CARD}, {MIC, CHEAP_CARD_SOUND});
    CHECK(cheap.audioDevice() && *cheap.audioDevice() == CHEAP_CARD_SOUND);
}

TEST(command_line_choices_win) {
    FakeBackend b;
    Controller::Options named;
    named.wantedVideo = "true vision";
    Controller c(b, named);
    plug(c, {WEBCAM, CARD}, {MIC, CARD_SOUND});
    CHECK(c.current() && *c.current() == WEBCAM && !c.audioDevice());

    FakeBackend b2;
    Controller::Options silent;
    silent.noAudio = true;
    Controller quiet(b2, silent);
    plug(quiet, {CARD}, {CARD_SOUND});
    CHECK(quiet.state() == State::Live && b2.sounds.empty());

    FakeBackend b3;
    Controller::Options sound;
    sound.wantedAudio = "microphone";
    Controller chosen(b3, sound);
    plug(chosen, {CARD}, {MIC, CARD_SOUND});
    CHECK(chosen.audioDevice() && *chosen.audioDevice() == MIC);
}

// ---------- Things going wrong ----------

TEST(busy_device_falls_through_to_the_next) {
    FakeBackend b;
    b.busy = {"card1"};
    Controller c(b, {});
    plug(c, {WEBCAM, CARD}, {MIC, CARD_SOUND});
    CHECK(c.state() == State::Live && c.current() && *c.current() == WEBCAM);
}

TEST(everything_busy_shows_an_error_then_retries_by_itself) {
    FakeBackend b;
    b.busy = {"card1"};
    Controller c(b, {});
    plug(c, {CARD}, {CARD_SOUND}, 100);
    CHECK(c.state() == State::Error);
    CHECK(c.detail().find("Another app") != std::string::npos);
    c.tick(101);
    CHECK(c.state() == State::Error);  // too early to retry
    b.busy.clear();
    c.tick(103.1);
    c.tick(103.1);
    CHECK(c.state() == State::Live);
}

TEST(sound_that_will_not_open_leaves_the_picture_and_is_not_retried) {
    FakeBackend b;
    b.brokenAudio = {CARD_SOUND.name};
    Controller c(b, {});
    plug(c, {CARD}, {CARD_SOUND});
    CHECK(c.state() == State::Live && !c.audioDevice());
    CHECK(!c.toast().empty());
    plug(c, {CARD}, {CARD_SOUND});
    plug(c, {CARD}, {CARD_SOUND});
    CHECK(b.opened.size() == 1);  // the picture must not be reopened
    CHECK(b.audioAttempts == 1);  // and the broken sound input is tried once only
    c.tick(1);
    c.tick(10);
    CHECK(c.toast().empty());  // the message goes away
}

TEST(no_devices_waits_then_connects_when_one_arrives) {
    FakeBackend b;
    Controller c(b, {});
    plug(c, {}, {});
    CHECK(c.state() == State::Waiting);
    CHECK(c.detail().find("Plug your HDMI capture card") != std::string::npos);
    plug(c, {CARD}, {CARD_SOUND});
    CHECK(c.state() == State::Live && c.audioDevice() && *c.audioDevice() == CARD_SOUND);
}

// ---------- Plugging and unplugging ----------

TEST(unplug_releases_everything_and_does_not_fall_back_to_the_webcam) {
    FakeBackend b;
    Controller c(b, {});
    plug(c, {WEBCAM, CARD}, {MIC, CARD_SOUND});
    unplug(c, b);
    CHECK(c.state() == State::Disconnected && c.message() == "Device disconnected");
    CHECK(*b.released[0]);
    CHECK(b.soundStopped[0]);
    CHECK(!b.awake.back());
    plug(c, {WEBCAM}, {MIC});
    CHECK(c.state() == State::Disconnected);
}

TEST(unplug_then_replug_reconnects) {
    FakeBackend b;
    Controller c(b, {});
    plug(c, {WEBCAM, CARD}, {MIC, CARD_SOUND});
    unplug(c, b);
    plug(c, {WEBCAM}, {MIC});
    plug(c, {WEBCAM, CARD}, {MIC, CARD_SOUND});
    CHECK(c.state() == State::Live && c.current() && *c.current() == CARD);
    CHECK(c.audioDevice() && *c.audioDevice() == CARD_SOUND);
}

TEST(quick_replug_that_the_scan_never_saw_reconnects) {
    FakeBackend b;
    Controller c(b, {});
    plug(c, {WEBCAM, CARD}, {MIC, CARD_SOUND});
    unplug(c, b);
    plug(c, {WEBCAM, CARD}, {MIC, CARD_SOUND});  // the list looks unchanged
    CHECK(c.state() == State::Live && c.current() && *c.current() == CARD);
}

TEST(capture_card_takes_over_from_the_webcam) {
    FakeBackend b;
    Controller c(b, {});
    plug(c, {WEBCAM}, {MIC});
    plug(c, {WEBCAM, CARD}, {MIC, CARD_SOUND});
    CHECK(c.current() && *c.current() == CARD && c.audioDevice());
    CHECK(*b.released[0]);  // the webcam must be let go
}

TEST(capture_card_arriving_while_the_webcam_is_about_to_open_goes_first) {
    FakeBackend b;
    Controller c(b, {});
    c.updateDevices({WEBCAM}, {MIC}, 0);
    c.updateDevices({WEBCAM, CARD}, {MIC, CARD_SOUND}, 0);
    c.tick(0);
    CHECK(c.current() && *c.current() == CARD);
    CHECK(b.opened.size() == 1);  // the webcam was never opened
}

TEST(second_capture_card_does_not_interrupt) {
    FakeBackend b;
    Controller c(b, {});
    plug(c, {CARD}, {CARD_SOUND});
    plug(c, {CARD, CARD2}, {CARD_SOUND});
    CHECK(c.current() && *c.current() == CARD && b.opened.size() == 1);
}

TEST(late_sound_is_added_without_reopening_the_picture) {
    FakeBackend b;
    Controller c(b, {});
    plug(c, {CARD}, {});
    CHECK(!c.audioDevice());
    plug(c, {CARD}, {CARD_SOUND});
    CHECK(c.audioDevice() && *c.audioDevice() == CARD_SOUND && b.opened.size() == 1);
}

TEST(a_hiccup_in_the_device_list_does_not_stop_the_picture) {
    FakeBackend b;
    Controller c(b, {});
    plug(c, {CARD}, {CARD_SOUND});
    plug(c, {}, {});
    CHECK(c.state() == State::Live && !b.soundStopped[0]);
}

// ---------- The black-picture tip ----------

TEST(black_picture_shows_the_tip_after_ten_seconds_and_clears) {
    FakeBackend b;
    Controller c(b, {});
    plug(c, {CARD}, {CARD_SOUND});
    c.reportBrightness(0, 0);
    CHECK(!c.showHdcpTip());
    c.reportBrightness(0, 11);
    CHECK(c.showHdcpTip());
    c.reportBrightness(200, 12);
    CHECK(!c.showHdcpTip());
}

// ---------- Keys ----------

TEST(mute_and_volume_reach_the_sound) {
    FakeBackend b;
    Controller c(b, {});
    plug(c, {CARD}, {CARD_SOUND});
    c.handleKey(Key::Mute, 0);
    CHECK(b.soundMuted[0]);
    c.handleKey(Key::Mute, 0);
    CHECK(!b.soundMuted[0]);
    c.handleKey(Key::VolumeDown, 0);
    c.handleKey(Key::VolumeDown, 0);
    CHECK(b.soundVolume[0] > 0.79f && b.soundVolume[0] < 0.81f);
    for (int i = 0; i < 5; ++i) c.handleKey(Key::VolumeUp, 0);
    CHECK(b.soundVolume[0] == 1.0f);
}

TEST(starting_muted_from_the_command_line) {
    FakeBackend b;
    Controller::Options options;
    options.muted = true;
    Controller c(b, options);
    plug(c, {CARD}, {CARD_SOUND});
    CHECK(c.muted() && b.soundMuted[0]);
}

TEST(mute_set_before_connecting_is_kept) {
    FakeBackend b;
    Controller c(b, {});
    c.handleKey(Key::Mute, 0);
    plug(c, {CARD}, {CARD_SOUND});
    CHECK(b.soundMuted[0]);
}

TEST(toggles_and_quitting) {
    FakeBackend b;
    Controller c(b, {});
    c.handleKey(Key::Fill, 0);
    c.handleKey(Key::Fullscreen, 0);
    c.handleKey(Key::Help, 0);
    CHECK(c.fill() && c.fullscreen() && c.showHelp());
    CHECK(c.handleKey(Key::None, 0));
    CHECK(c.handleKey(Key::Escape, 0) && !c.fullscreen());  // Esc leaves full screen first
    CHECK(!c.handleKey(Key::Escape, 0));
    CHECK(!c.handleKey(Key::Quit, 0));
}

TEST(stepping_through_video_devices) {
    FakeBackend b;
    Controller c(b, {});
    plug(c, {WEBCAM, CARD}, {MIC, CARD_SOUND});
    c.handleKey(Key::NextVideo, 0);
    c.tick(0);
    CHECK(c.current() && *c.current() == WEBCAM && !c.audioDevice());
    c.handleKey(Key::NextVideo, 0);
    c.tick(0);
    CHECK(c.current() && *c.current() == CARD);
}

TEST(stepping_through_sound_inputs_and_then_off) {
    FakeBackend b;
    Controller c(b, {});
    plug(c, {CARD}, {MIC, CARD_SOUND});
    CHECK(c.audioDevice() && *c.audioDevice() == CARD_SOUND);
    c.handleKey(Key::NextAudio, 0);
    CHECK(!c.audioDevice() && b.soundStopped[0]);
    plug(c, {CARD}, {MIC, CARD_SOUND});
    CHECK(!c.audioDevice());  // sound switched off by hand must stay off
    c.handleKey(Key::NextAudio, 0);
    CHECK(c.audioDevice() && *c.audioDevice() == MIC);
}

TEST(a_key_steps_past_a_sound_input_that_will_not_open) {
    FakeBackend b;
    b.brokenAudio = {MIC.name};
    Controller c(b, {});
    plug(c, {CARD}, {MIC, CARD_SOUND});
    c.handleKey(Key::NextAudio, 0);  // card sound -> off (it was last in the list)
    c.handleKey(Key::NextAudio, 0);  // off -> microphone, which will not open
    CHECK(!c.audioDevice());
    CHECK(c.toast().find("Could not open") != std::string::npos);
    c.handleKey(Key::NextAudio, 0);  // must move on, not retry the microphone
    CHECK(c.audioDevice() && *c.audioDevice() == CARD_SOUND);
}

TEST(sound_that_stops_by_itself_is_restarted) {
    FakeBackend b;
    Controller c(b, {});
    plug(c, {CARD}, {CARD_SOUND});
    b.soundDead[0] = true;  // the headphones were disconnected
    c.tick(10);
    CHECK(!c.audioDevice() && b.soundStopped[0]);
    c.tick(10.5);
    CHECK(!c.audioDevice());  // not yet
    c.tick(11.1);
    CHECK(c.audioDevice() && *c.audioDevice() == CARD_SOUND && b.sounds.size() == 2);
}

TEST(screenshot_only_when_there_is_a_picture) {
    FakeBackend b;
    Controller c(b, {});
    plug(c, {}, {});
    c.handleKey(Key::Screenshot, 0);
    CHECK(b.screenshots == 0);
    plug(c, {CARD}, {CARD_SOUND});
    c.handleKey(Key::Screenshot, 0);
    CHECK(b.screenshots == 1);
    CHECK(c.toast() == "Screenshot saved to Downloads");
}

TEST(closing_releases_the_device_and_the_sound) {
    FakeBackend b;
    {
        Controller c(b, {});
        plug(c, {CARD}, {CARD_SOUND});
    }
    CHECK(*b.released[0] && b.soundStopped[0] && !b.awake.back());
}

TEST(dirty_flag_reports_changes_once) {
    FakeBackend b;
    Controller c(b, {});
    CHECK(c.takeDirty());
    CHECK(!c.takeDirty());
    c.handleKey(Key::Fill, 0);
    CHECK(c.takeDirty());
    CHECK(!c.takeDirty());
}

}  // namespace

int main() {
    for (const auto& test : registry()) {
        currentFailed = false;
        test.run();
        std::printf("%s %s\n", currentFailed ? "FAIL" : "ok  ", test.name);
        if (currentFailed) ++failures;
    }
    std::printf("\n%zu tests, %d failed\n", registry().size(), failures);
    return failures == 0 ? 0 : 1;
}
