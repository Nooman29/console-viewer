#include "controller.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace cv {

Controller::Controller(Backend& backend, Options options)
    : backend_(backend), options_(std::move(options)) {
    fill_ = options_.fill;
    muted_ = options_.muted;
    fullscreen_ = options_.fullscreen;
}

Controller::~Controller() { close(); }

void Controller::close() { stopStream(); }

// ---------------------------------------------------------------------
// Devices coming and going
// ---------------------------------------------------------------------

void Controller::updateDevices(std::vector<VideoDevice> videos, std::vector<AudioDevice> audios, double now) {
    const std::set<std::string> before = knownKeys_;
    const bool firstTime = !seenDevices_;
    seenDevices_ = true;
    videos_ = std::move(videos);
    audios_ = std::move(audios);
    knownKeys_.clear();
    for (const auto& v : videos_) knownKeys_.insert(v.key);

    std::vector<VideoDevice> added;
    for (const auto& v : videos_) {
        if (!before.count(v.key)) added.push_back(v);
    }
    const auto newCapture = std::find_if(added.begin(), added.end(),
                                         [](const VideoDevice& v) { return looksLikeCapture(v.name); });
    const bool haveNewCapture = newCapture != added.end();

    if (state_ == State::Connecting) {
        // The open has not happened yet; it will use this newer list. A
        // capture card that has just arrived goes to the front of the queue.
        if (haveNewCapture) pendingPrefer_ = newCapture->key;
        return;
    }

    if (state_ != State::Live) {
        if (firstTime) {
            requestConnect("");
        } else if (!added.empty()) {
            requestConnect(haveNewCapture ? newCapture->key : added.front().key);
        } else if (!lostKey_.empty() && knownKeys_.count(lostKey_)) {
            // A quick unplug and replug, or a card that reset itself when the
            // console changed resolution, never looks "new".
            requestConnect(lostKey_);
        } else if (videos_.empty() && state_ != State::Disconnected) {
            setWaiting();
        }
        return;
    }

    // Take over from a webcam, but never interrupt a capture card mid-game.
    if (haveNewCapture && current_ && !looksLikeCapture(current_->name)) {
        showToast("Switched to " + newCapture->name, now);
        requestConnect(newCapture->key);
        return;
    }
    // A missing entry in the list is not trusted while pictures still arrive;
    // a real unplug stops the frames, which tick() notices.

    // A capture card's sound device can register a moment after its picture.
    if (!audio_) startAudio();
}

void Controller::requestConnect(const std::string& preferKey) {
    stopStream();
    if (videos_.empty()) {
        setWaiting();
        return;
    }
    // Opening a device can take a second or two. Doing it on the next tick
    // lets the window draw "Connecting..." first.
    pendingPrefer_ = preferKey;
    setState(State::Connecting, "Connecting...", "");
}

void Controller::connectNow(double now) {
    if (videos_.empty()) {
        setWaiting();
        return;
    }
    std::string firstError;
    for (const auto& device : rankVideos(videos_, pendingPrefer_, options_.wantedVideo)) {
        std::string error;
        auto source = backend_.openVideo(device, error);
        if (!source) {
            if (firstError.empty()) firstError = error;
            continue;
        }
        source_ = std::move(source);
        current_ = device;
        lostKey_.clear();
        audioFailed_.clear();
        pendingPrefer_.clear();
        black_.reset();
        showHdcpTip_ = false;
        setState(State::Live, "", "");
        backend_.keepAwake(true);
        startAudio();
        return;
    }
    pendingPrefer_.clear();
    setState(State::Error, "Could not open the device",
             firstError.empty() ? "Another app may be using it." : firstError);
    retryAt_ = now + options_.retrySeconds;
}

void Controller::setWaiting() {
    setState(State::Waiting, "No video device found",
             "Plug your HDMI capture card into a USB port. It will appear here automatically.");
}

void Controller::setState(State state, std::string message, std::string detail) {
    state_ = state;
    message_ = std::move(message);
    detail_ = std::move(detail);
    dirty_ = true;
}

void Controller::stopStream() {
    stopAudio();
    if (source_) {
        source_.reset();
        backend_.keepAwake(false);
    }
    current_.reset();
    showHdcpTip_ = false;
}

// ---------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------

std::optional<AudioDevice> Controller::wantedAudio() const {
    if (!current_ || options_.noAudio || audioChoice_ == AudioChoice::Off) return std::nullopt;
    if (audioChoice_ == AudioChoice::Named) return findAudioByName(audioChoiceName_, audios_);
    if (!options_.wantedAudio.empty()) return findAudioByName(options_.wantedAudio, audios_);
    return chooseAudio(*current_, audios_);
}

void Controller::startAudio() {
    if (audio_) return;
    const auto device = wantedAudio();
    if (!device || device->name == audioFailed_) return;
    audio_ = backend_.openAudio(*device);
    if (!audio_) {
        audioFailed_ = device->name;  // do not retry it on every rescan
        toast_ = "Could not open that sound source. Showing picture only.";
        toastUntil_ = -1;  // tick() gives it a real expiry
        dirty_ = true;
        return;
    }
    audioDevice_ = device;
    applyVolume();
    dirty_ = true;
}

void Controller::stopAudio() {
    audio_.reset();
    audioDevice_.reset();
}

void Controller::applyVolume() {
    if (audio_) audio_->setVolume(volume_, muted_);
}

void Controller::nextAudio(double now) {
    // Step through: each sound input in turn, then off.
    // Go by what was last asked for, so an input that would not open is
    // stepped past instead of being tried again on every press.
    std::string currentName;
    if (audioChoice_ == AudioChoice::Named) currentName = audioChoiceName_;
    else if (audioDevice_) currentName = audioDevice_->name;
    std::size_t position = 0;
    for (std::size_t i = 0; i < audios_.size(); ++i) {
        if (!currentName.empty() && audios_[i].name == currentName) position = i + 1;
    }
    stopAudio();
    audioFailed_.clear();
    if (position < audios_.size()) {
        audioChoice_ = AudioChoice::Named;
        audioChoiceName_ = audios_[position].name;
        startAudio();
        if (audio_) showToast("Sound: " + audioChoiceName_, now);
        else showToast("Could not open " + audioChoiceName_ + ". Press A for the next one.", now);
    } else {
        audioChoice_ = AudioChoice::Off;
        showToast("Sound: off", now);
    }
}

void Controller::setVolume(float volume, double now) {
    volume_ = std::round(std::min(std::max(volume, 0.0f), 1.0f) * 100.0f) / 100.0f;
    applyVolume();
    showToast("Volume " + std::to_string(static_cast<int>(std::lround(volume_ * 100))) + "%", now);
}

// ---------------------------------------------------------------------
// Moving forward in time
// ---------------------------------------------------------------------

void Controller::tick(double now) {
    if (state_ == State::Connecting) {
        connectNow(now);
    } else if (state_ == State::Live && source_ && source_->failed()) {
        onDisconnect();
    } else if (state_ == State::Error && now >= retryAt_) {
        requestConnect("");
    }
    // Sound that stopped by itself (the speakers or headphones went away, or
    // the card's sound input did): drop it and try again shortly.
    if (audio_ && audio_->failed()) {
        stopAudio();
        audioRetryAt_ = now + 1.0;
    } else if (!audio_ && audioRetryAt_ > 0 && now >= audioRetryAt_) {
        audioRetryAt_ = 0;
        if (state_ == State::Live) startAudio();
    }
    if (!toast_.empty()) {
        if (toastUntil_ < 0) {
            toastUntil_ = now + 3.0;
        } else if (now >= toastUntil_) {
            toast_.clear();
            dirty_ = true;
        }
    }
}

void Controller::reportBrightness(int brightest, double now) {
    if (state_ != State::Live) return;
    const bool tip = black_.update(brightest, now);
    if (tip != showHdcpTip_) {
        showHdcpTip_ = tip;
        dirty_ = true;
    }
}

void Controller::onDisconnect() {
    const std::string lost = current_ ? current_->key : std::string();
    stopStream();
    lostKey_ = lost;
    setState(State::Disconnected, "Device disconnected",
             "Plug it back in and it will reconnect automatically.");
}

void Controller::showToast(std::string text, double now, double seconds) {
    toast_ = std::move(text);
    toastUntil_ = now + seconds;
    dirty_ = true;
}

// ---------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------

bool Controller::handleKey(Key key, double now) {
    if (key == Key::None) return true;
    dirty_ = true;
    switch (key) {
        case Key::Escape:
            if (fullscreen_) {
                fullscreen_ = false;
                return true;
            }
            return false;
        case Key::Quit:
            return false;
        case Key::Help:
            showHelp_ = !showHelp_;
            break;
        case Key::Fullscreen:
            fullscreen_ = !fullscreen_;
            break;
        case Key::Fill:
            fill_ = !fill_;
            break;
        case Key::Mute:
            muted_ = !muted_;
            applyVolume();
            showToast(muted_ ? "Muted" : "Sound on", now);
            break;
        case Key::VolumeUp:
            setVolume(volume_ + 0.1f, now);
            break;
        case Key::VolumeDown:
            setVolume(volume_ - 0.1f, now);
            break;
        case Key::Screenshot:
            if (state_ == State::Live) {
                const std::string folder = backend_.saveScreenshot();
                showToast(folder.empty() ? "Could not save the screenshot." : "Screenshot saved to " + folder, now);
            }
            break;
        case Key::NextAudio:
            if (state_ == State::Live) nextAudio(now);
            break;
        case Key::NextVideo:
            if (state_ == State::Live && current_ && videos_.size() > 1) {
                std::size_t at = 0;
                for (std::size_t i = 0; i < videos_.size(); ++i) {
                    if (videos_[i].key == current_->key) at = i;
                }
                requestConnect(videos_[(at + 1) % videos_.size()].key);
            }
            break;
        case Key::None:
            break;
    }
    return true;
}

}  // namespace cv
