#include "rules.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>
#include <tuple>

namespace cv {
namespace {

const std::regex& captureHint() {
    // Names that capture cards commonly report.
    static const std::regex re(
        "capture|hdmi|cam link|elgato|avermedia|live gamer|usb ?3?\\.?0? video"
        "|macrosilicon|ezcap|video grabber|magewell|ripsaw",
        std::regex::icase | std::regex::optimize);
    return re;
}

const std::regex& webcamHint() {
    static const std::regex re("webcam|camera|facetime|integrated", std::regex::icase | std::regex::optimize);
    return re;
}

const std::regex& cardSoundHint() {
    // The most common cheap cards name their sound input like this.
    static const std::regex re("digital audio interface|usb digital audio", std::regex::icase | std::regex::optimize);
    return re;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool containsIgnoringCase(const std::string& haystack, const std::string& needle) {
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

}  // namespace

bool looksLikeCapture(const std::string& name) {
    return std::regex_search(name, captureHint()) && !std::regex_search(name, webcamHint());
}

std::vector<VideoDevice> rankVideos(const std::vector<VideoDevice>& devices,
                                    const std::string& preferKey, const std::string& wantedName) {
    auto score = [&](const VideoDevice& device) {
        if (!preferKey.empty() && device.key == preferKey) return 0;
        if (!wantedName.empty() && containsIgnoringCase(device.name, wantedName)) return 1;
        if (looksLikeCapture(device.name)) return 2;
        return 3;
    };
    std::vector<VideoDevice> ranked = devices;
    std::stable_sort(ranked.begin(), ranked.end(),
                     [&](const VideoDevice& a, const VideoDevice& b) { return score(a) < score(b); });
    return ranked;
}

std::optional<AudioDevice> chooseAudio(const VideoDevice& video, const std::vector<AudioDevice>& audios) {
    if (!looksLikeCapture(video.name)) return std::nullopt;
    // Windows names a card's sound input after the card: "HDMI (Cam Link 4K)".
    for (const auto& audio : audios) {
        if (containsIgnoringCase(audio.name, video.name)) return audio;
    }
    for (const auto& audio : audios) {
        if (looksLikeCapture(audio.name) || std::regex_search(audio.name, cardSoundHint())) return audio;
    }
    return std::nullopt;
}

std::optional<AudioDevice> findAudioByName(const std::string& wanted, const std::vector<AudioDevice>& audios) {
    for (const auto& audio : audios) {
        if (containsIgnoringCase(audio.name, wanted)) return audio;
    }
    return std::nullopt;
}

bool BlackDetector::update(int brightest, double now) {
    if (brightest >= limit_) {
        darkSince_.reset();
        return false;
    }
    if (!darkSince_) darkSince_ = now;
    return now - *darkSince_ >= seconds_;
}

Rect fitRect(int srcW, int srcH, int dstW, int dstH, bool fill) {
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) {
        return {0, 0, std::max(dstW, 1), std::max(dstH, 1)};
    }
    const double scaleW = static_cast<double>(dstW) / srcW;
    const double scaleH = static_cast<double>(dstH) / srcH;
    const double scale = fill ? std::max(scaleW, scaleH) : std::min(scaleW, scaleH);
    const int w = std::max(1, static_cast<int>(std::lround(srcW * scale)));
    const int h = std::max(1, static_cast<int>(std::lround(srcH * scale)));
    // Floor division, so an overflowing picture is centred the same way a
    // letterboxed one is.
    auto half = [](int value) { return static_cast<int>(std::floor(value / 2.0)); };
    return {half(dstW - w), half(dstH - h), w, h};
}

int pickFormat(const std::vector<VideoFormat>& formats, int wantW, int wantH, double wantFps) {
    int best = -1;
    std::tuple<int, long long, double, int> bestScore{};
    for (int i = 0; i < static_cast<int>(formats.size()); ++i) {
        const VideoFormat& f = formats[i];
        if (f.width <= 0 || f.height <= 0) continue;
        const bool fits = f.width <= wantW && f.height <= wantH;
        const long long area = static_cast<long long>(f.width) * f.height;
        // Among sizes that fit, bigger is better. If nothing fits, the
        // smallest oversize one is the least wasteful.
        const std::tuple<int, long long, double, int> score{
            fits ? 1 : 0,
            fits ? area : -area,
            std::min(f.fps, wantFps),
            f.compressed ? 0 : 1,  // at the same size and speed, raw pixels cost less to show
        };
        if (best < 0 || score > bestScore) {
            best = i;
            bestScore = score;
        }
    }
    return best;
}

}  // namespace cv
