// What the viewer is doing and how it reacts, with no window and no hardware.
//
// The controller is told which devices exist (updateDevices), moved forward
// in time (tick), given key presses (handleKey) and read back by whatever
// draws the screen. Everything real goes through the Backend it was given,
// so the tests can pass a pretend one.
#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "rules.h"

namespace cv {

// An open camera or capture card. Destroying it lets go of the device.
struct VideoSource {
    virtual ~VideoSource() = default;
    // True once the device has stopped delivering pictures.
    virtual bool failed() const = 0;
};

// The card's sound playing through the speakers. Destroying it stops it.
struct AudioOutput {
    virtual ~AudioOutput() = default;
    virtual void setVolume(float volume, bool muted) = 0;
    // True once the sound has stopped by itself, for example because the
    // headphones it was playing through were disconnected.
    virtual bool failed() const { return false; }
};

struct Backend {
    virtual ~Backend() = default;
    // Returns null and fills `error` if the device will not open.
    virtual std::unique_ptr<VideoSource> openVideo(const VideoDevice& device, std::string& error) = 0;
    // Returns null if the sound input will not open.
    virtual std::unique_ptr<AudioOutput> openAudio(const AudioDevice& device) = 0;
    // Stop the screen dimming or locking while a picture is showing.
    virtual void keepAwake(bool on) = 0;
    // Save the current picture. Returns the folder name it went to, or "".
    virtual std::string saveScreenshot() = 0;
};

enum class Key { None, Escape, Quit, Help, Fullscreen, Fill, Mute, VolumeUp, VolumeDown, Screenshot, NextVideo, NextAudio };

class Controller {
public:
    enum class State { Starting, Connecting, Live, Waiting, Disconnected, Error };

    struct Options {
        std::string wantedVideo;   // part of a device name, from the command line
        std::string wantedAudio;   // part of a sound input name
        bool noAudio = false;
        bool muted = false;        // start with the sound muted
        bool fill = false;
        bool fullscreen = false;
        double retrySeconds = 3.0;
    };

    Controller(Backend& backend, Options options);
    ~Controller();

    // Tell the controller which devices exist right now.
    void updateDevices(std::vector<VideoDevice> videos, std::vector<AudioDevice> audios, double now);
    // Move forward: open a device that was asked for, notice one that died,
    // retry after an error, expire the toast.
    void tick(double now);
    // The brightest pixel of the current picture, for the black-picture tip.
    void reportBrightness(int brightest, double now);
    // Returns false when the viewer should close.
    bool handleKey(Key key, double now);
    void close();

    // ---- Read by whatever draws the screen ----
    State state() const { return state_; }
    const std::string& message() const { return message_; }
    const std::string& detail() const { return detail_; }
    const std::string& toast() const { return toast_; }
    const VideoDevice* current() const { return current_ ? &*current_ : nullptr; }
    const AudioDevice* audioDevice() const { return audioDevice_ ? &*audioDevice_ : nullptr; }
    bool fill() const { return fill_; }
    bool fullscreen() const { return fullscreen_; }
    bool muted() const { return muted_; }
    float volume() const { return volume_; }
    bool showHelp() const { return showHelp_; }
    bool showHdcpTip() const { return showHdcpTip_; }
    // True when something other than the picture changed since last asked.
    bool takeDirty() { const bool was = dirty_; dirty_ = false; return was; }

private:
    void requestConnect(const std::string& preferKey);
    void connectNow(double now);
    void stopStream();
    void setState(State state, std::string message, std::string detail);
    void setWaiting();
    std::optional<AudioDevice> wantedAudio() const;
    void startAudio();
    void stopAudio();
    void applyVolume();
    void nextAudio(double now);
    void setVolume(float volume, double now);
    void showToast(std::string text, double now, double seconds = 3.0);
    void onDisconnect();

    Backend& backend_;
    Options options_;

    State state_ = State::Starting;
    std::string message_ = "Looking for devices...";
    std::string detail_;

    std::vector<VideoDevice> videos_;
    std::vector<AudioDevice> audios_;
    std::set<std::string> knownKeys_;
    bool seenDevices_ = false;

    std::optional<VideoDevice> current_;
    std::unique_ptr<VideoSource> source_;
    std::string lostKey_;        // the device that was showing when it dropped out
    std::string pendingPrefer_;  // which device the next connect should try first
    double retryAt_ = 0;

    std::unique_ptr<AudioOutput> audio_;
    std::optional<AudioDevice> audioDevice_;
    enum class AudioChoice { Automatic, Off, Named } audioChoice_ = AudioChoice::Automatic;
    std::string audioChoiceName_;
    std::string audioFailed_;    // a sound input that would not open
    double audioRetryAt_ = 0;    // when to restart sound that stopped by itself

    bool muted_ = false;
    float volume_ = 1.0f;
    bool fill_ = false;
    bool fullscreen_ = false;
    bool showHelp_ = false;
    bool showHdcpTip_ = false;
    BlackDetector black_;

    std::string toast_;
    double toastUntil_ = 0;
    bool dirty_ = true;
};

}  // namespace cv
