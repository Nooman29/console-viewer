// The decisions the viewer makes, with no hardware and no Windows involved.
//
// Everything in core/ is plain C++17 so it can be unit tested on any machine:
// which device to open, which sound source belongs to it, which picture
// format to ask a device for, when a black picture means copy protection,
// and how to fit a picture into a window.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace cv {

// A camera or capture card. `key` stays the same across unplug and replug.
struct VideoDevice {
    std::string name;
    std::string key;
};

// A sound input.
struct AudioDevice {
    std::string name;
    std::string id;
};

inline bool operator==(const VideoDevice& a, const VideoDevice& b) { return a.key == b.key; }
inline bool operator==(const AudioDevice& a, const AudioDevice& b) { return a.id == b.id; }

// True when a device name looks like a capture card and not a webcam.
bool looksLikeCapture(const std::string& name);

// Every video device, best guess first: the one just asked for (`preferKey`),
// the one named on the command line (`wantedName`), anything that looks like
// a capture card, then the rest in their original order.
std::vector<VideoDevice> rankVideos(const std::vector<VideoDevice>& devices,
                                    const std::string& preferKey = "",
                                    const std::string& wantedName = "");

// The sound input that belongs to a capture card, if any. Sound is only
// picked automatically for capture cards: playing a webcam's microphone
// through the speakers would howl with feedback.
std::optional<AudioDevice> chooseAudio(const VideoDevice& video,
                                       const std::vector<AudioDevice>& audios);

// The first sound input whose name contains `wanted`, ignoring case.
std::optional<AudioDevice> findAudioByName(const std::string& wanted,
                                           const std::vector<AudioDevice>& audios);

// Decides when a picture has been black long enough to mention HDCP. A dark
// game scene still has some bright pixels; a blocked signal has none. Blocked
// signals are often dark grey with a little noise, so the limit is above zero.
class BlackDetector {
public:
    explicit BlackDetector(int limit = 24, double seconds = 10.0) : limit_(limit), seconds_(seconds) {}
    void reset() { darkSince_.reset(); }
    // Feed the brightest pixel value of a frame. Returns true to show the tip.
    bool update(int brightest, double now);

private:
    int limit_;
    double seconds_;
    std::optional<double> darkSince_;
};

struct Rect {
    int x, y, w, h;
};
inline bool operator==(const Rect& a, const Rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// Where a picture goes in a window. With fill off the whole picture is
// visible with bars around it. With fill on it covers the window and the
// overflow is cut off, so x or y can be negative.
Rect fitRect(int srcW, int srcH, int dstW, int dstH, bool fill = false);

// One picture format a device offers.
struct VideoFormat {
    int width = 0;
    int height = 0;
    double fps = 0;
    bool compressed = false;  // MJPEG or H.264, as opposed to raw pixels
};

// Which of a device's formats to use, as an index into `formats`, or -1 if
// there are none. Picture size comes first, then smoothness: the largest size
// that does not exceed what was asked for, at the highest frame rate up to
// the one asked for. A USB 2.0 card offers 1080p as raw pixels at 5 frames a
// second and as MJPEG at 30 or 60, so this picks the MJPEG one.
int pickFormat(const std::vector<VideoFormat>& formats, int wantW, int wantH, double wantFps);

}  // namespace cv
