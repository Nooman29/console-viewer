// Console Viewer, C++ version: the command line and the main loop.
#include "app.h"

#include <mfapi.h>
#include <mmsystem.h>
#include <shellapi.h>

#include <algorithm>

#include <cstdio>
#include <cstdlib>
#include <cwchar>

namespace {

using namespace cvwin;

struct Arguments {
    cv::Controller::Options options;
    CaptureSettings capture;
    bool list = false;
    bool demo = false;
    bool version = false;
    bool help = false;
    double exitAfter = -1;         // seconds; for automated checks
    double screenshotAfter = -1;   // seconds; for automated checks
    std::wstring screenshotFolder; // where screenshots go instead of Downloads; for automated checks
    std::string error;
};

const char* const kUsage =
    "Console Viewer: show a games console on your PC through an HDMI capture card.\n"
    "\n"
    "  console-viewer [options]\n"
    "\n"
    "  --list            list cameras and sound inputs, then exit\n"
    "  --device NAME     open the video device whose name contains NAME\n"
    "  --audio NAME      use the sound input whose name contains NAME\n"
    "  --no-audio        picture only\n"
    "  --mute            start with the sound muted\n"
    "  --fullscreen      start in full screen\n"
    "  --fill            fill the window, cutting off the edges\n"
    "  --width N         picture width to ask for (default 1920)\n"
    "  --height N        picture height to ask for (default 1080)\n"
    "  --fps N           frames per second to ask for (default 60)\n"
    "  --demo            show a moving test picture; needs no hardware\n"
    "  --version         print the version\n";

// This is a windowed program, so it has no console of its own. Write to the
// one it was started from, or to a file or pipe if output was redirected.
void say(const std::string& text) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
        out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (out == INVALID_HANDLE_VALUE) return;
    }
    DWORD written = 0;
    DWORD mode = 0;
    if (GetConsoleMode(out, &mode)) {
        const std::wstring wide = widen(text);
        WriteConsoleW(out, wide.c_str(), static_cast<DWORD>(wide.size()), &written, nullptr);
    } else {
        WriteFile(out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    }
}

Arguments parse() {
    Arguments args;
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!argv) return args;
    for (int i = 1; i < count && args.error.empty(); ++i) {
        const std::wstring arg = argv[i];
        auto value = [&]() -> std::wstring {
            if (i + 1 < count) return argv[++i];
            args.error = narrow(arg) + " needs a value.";
            return L"";
        };
        auto number = [&]() -> double {
            const std::wstring text = value();
            wchar_t* end = nullptr;
            const double parsed = std::wcstod(text.c_str(), &end);
            if (args.error.empty() && (text.empty() || *end != L'\0' || parsed <= 0)) {
                args.error = narrow(arg) + " needs a number above zero.";
            }
            return parsed;
        };
        if (arg == L"--list") args.list = true;
        else if (arg == L"--demo") args.demo = true;
        else if (arg == L"--version") args.version = true;
        else if (arg == L"--help" || arg == L"-h" || arg == L"/?") args.help = true;
        else if (arg == L"--no-audio") args.options.noAudio = true;
        else if (arg == L"--mute") args.options.muted = true;
        else if (arg == L"--screenshot-after") args.screenshotAfter = number();
        else if (arg == L"--fullscreen") args.options.fullscreen = true;
        else if (arg == L"--fill") args.options.fill = true;
        else if (arg == L"--device") args.options.wantedVideo = narrow(value());
        else if (arg == L"--audio") args.options.wantedAudio = narrow(value());
        else if (arg == L"--width") args.capture.width = static_cast<int>(number());
        else if (arg == L"--height") args.capture.height = static_cast<int>(number());
        else if (arg == L"--fps") args.capture.fps = number();
        else if (arg == L"--exit-after") args.exitAfter = number();
        else if (arg == L"--screenshot-folder") args.screenshotFolder = value();
        else args.error = "Unknown option: " + narrow(arg);
    }
    LocalFree(argv);
    return args;
}

double seconds() {
    static LARGE_INTEGER frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / static_cast<double>(frequency.QuadPart);
}

const cv::VideoDevice kDemoDevice{"Demo HDMI Capture", "demo"};

// The real hardware, as the controller sees it.
class WindowsBackend : public cv::Backend {
public:
    WindowsBackend(FrameStore& store, const Arguments& args) : store_(store), args_(args) {}

    std::vector<cv::VideoDevice> videos() const {
        return args_.demo ? std::vector<cv::VideoDevice>{kDemoDevice} : listVideoDevices();
    }
    std::vector<cv::AudioDevice> audios() const {
        return args_.demo ? std::vector<cv::AudioDevice>{} : listAudioInputs();
    }

    std::unique_ptr<cv::VideoSource> openVideo(const cv::VideoDevice& device, std::string& error) override {
        if (args_.demo) return openDemo(args_.capture, store_);
        return openCapture(device, args_.capture, store_, error);
    }
    std::unique_ptr<cv::AudioOutput> openAudio(const cv::AudioDevice& device) override {
        return openPassthrough(device);
    }
    void keepAwake(bool on) override {
        // A controller sends no keyboard or mouse input to the PC, so without
        // this Windows would dim or lock the screen mid-game.
        SetThreadExecutionState(ES_CONTINUOUS | (on ? ES_DISPLAY_REQUIRED : 0));
    }
    std::string saveScreenshot() override {
        Frame frame;
        std::uint64_t seen = ~static_cast<std::uint64_t>(0);
        if (!store_.copyIfNewer(frame, seen)) return "";
        return saveScreenshotPng(frame, nullptr, args_.screenshotFolder);
    }

private:
    FrameStore& store_;
    const Arguments& args_;
};

int brightestPixel(const Frame& frame) {
    int brightest = 0;
    const std::size_t step = 4 * 16;  // every 16th pixel is plenty
    for (std::size_t i = 0; i + 2 < frame.pixels.size(); i += step) {
        const int value = std::max({frame.pixels[i], frame.pixels[i + 1], frame.pixels[i + 2]});
        if (value > brightest) brightest = value;
    }
    return brightest;
}

const char* stateName(cv::Controller::State state) {
    using State = cv::Controller::State;
    switch (state) {
        case State::Starting: return "starting";
        case State::Connecting: return "connecting";
        case State::Live: return "live";
        case State::Waiting: return "waiting";
        case State::Disconnected: return "disconnected";
        case State::Error: return "error";
    }
    return "?";
}

void listDevices(const WindowsBackend& backend) {
    const auto videos = backend.videos();
    const auto audios = backend.audios();
    std::string out = "Video devices:\n";
    for (const auto& device : videos) {
        out += "  " + device.name + (cv::looksLikeCapture(device.name) ? "  (capture card)\n" : "  (camera)\n");
        if (const auto sound = cv::chooseAudio(device, audios)) out += "      sound: " + sound->name + "\n";
    }
    if (videos.empty()) out += "  none found\n";
    out += "Sound inputs:\n";
    for (const auto& device : audios) out += "  " + device.name + "\n";
    if (audios.empty()) out += "  none found\n";
    say(out);
}

int run(const Arguments& args) {
    FrameStore store;
    WindowsBackend backend(store, args);
    if (args.list) {
        listDevices(backend);
        return 0;
    }

    Window window;
    if (!window.create(1280, 720)) return 1;
    cv::Controller controller(backend, args.options);

    // Windows normally wakes a waiting program only every 15 ms or so, which
    // is too coarse to show 60 pictures a second smoothly. Ask for 1 ms.
    timeBeginPeriod(1);

    Frame frame;
    std::uint64_t seenFrames = 0;
    bool haveFrame = false;
    bool isFullscreen = false;
    const double started = seconds();
    double nextScan = 0;        // look for devices straight away
    double nextBlackCheck = 0;
    double lastPaint = 0;
    int painted = 0;
    bool running = true;
    bool screenshotTaken = false;
    int newPictures = 0;

    while (running) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        const double now = seconds();
        if (window.closed()) break;
        for (cv::Key key : window.takeKeys()) {
            if (!controller.handleKey(key, now)) running = false;
        }
        if (!running) break;
        if (args.screenshotAfter > 0 && !screenshotTaken && now - started >= args.screenshotAfter) {
            screenshotTaken = true;
            controller.handleKey(cv::Key::Screenshot, now);
        }

        // Listing devices takes a moment, so while a picture is showing it is
        // only done when Windows says something was plugged in or removed.
        const bool live = controller.state() == cv::Controller::State::Live;
        if (window.takeDeviceChange()) nextScan = std::min(nextScan, now + 0.5);
        if (now >= nextScan) {
            controller.updateDevices(backend.videos(), backend.audios(), now);
            nextScan = live ? 1e18 : now + 2.0;
        } else if (!live && nextScan > now + 2.0) {
            nextScan = now + 2.0;
        }

        // Opening a device blocks for a second or two. Draw "Connecting..." first.
        if (controller.state() == cv::Controller::State::Connecting) {
            window.paint(nullptr, controller);
            haveFrame = false;
        }
        controller.tick(now);

        if (controller.fullscreen() != isFullscreen) {
            isFullscreen = controller.fullscreen();
            window.setFullscreen(isFullscreen);
        }

        const bool nowLive = controller.state() == cv::Controller::State::Live;
        bool fresh = false;
        if (nowLive) {
            fresh = store.copyIfNewer(frame, seenFrames);
            haveFrame = haveFrame || fresh;
            if (fresh) ++newPictures;
            if (fresh && now >= nextBlackCheck) {
                nextBlackCheck = now + 1.5;
                controller.reportBrightness(brightestPixel(frame), now);
            }
        } else {
            haveFrame = false;
        }

        if (fresh || controller.takeDirty() || now - lastPaint > 0.25) {
            window.paint(nowLive && haveFrame ? &frame : nullptr, controller);
            lastPaint = now;
            ++painted;
        }

        if (args.exitAfter > 0 && now - started >= args.exitAfter) break;

        // Sleep until a new picture, a key press or a short timeout.
        HANDLE newFrame = store.newFrameEvent();
        MsgWaitForMultipleObjects(1, &newFrame, FALSE, nowLive ? 50 : 100, QS_ALLINPUT);
    }

    const cv::Controller::State lastState = controller.state();
    const std::string device = controller.current() ? controller.current()->name : "none";
    const std::string sound = controller.audioDevice() ? controller.audioDevice()->name : "none";
    const std::string lastToast = controller.toast();
    controller.close();
    timeEndPeriod(1);
    if (args.exitAfter > 0) {
        char line[160];
        std::snprintf(line, sizeof(line), "drew %d frames (%d new pictures, %dx%d) in %.1f s; last state: %s\n",
                      painted, newPictures, frame.width, frame.height, seconds() - started, stateName(lastState));
        say(std::string(line) + "device: " + device + "\nsound: " + sound + "\nlast message: " + lastToast + "\n");
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const Arguments args = parse();
    if (!args.error.empty()) {
        say(args.error + "\n\n" + kUsage);
        return 2;
    }
    if (args.help) {
        say(kUsage);
        return 0;
    }
    if (args.version) {
        say(std::string("console-viewer ") + CV_VERSION + "\n");
        return 0;
    }

    // Sharp text on high-resolution screens (Windows 10 1703 and later).
    using Setter = BOOL(WINAPI*)(HANDLE);
    if (const auto setter = reinterpret_cast<Setter>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")))) {
        setter(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));  // per-monitor, version 2
    }

    // Multithreaded on purpose: the capture thread and this one share the
    // device objects, and with a single-threaded apartment here every call
    // from the capture thread would wait for this thread to pump messages,
    // which deadlocks the moment this thread waits for that one to finish.
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    int result = 1;
    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        result = run(args);
        MFShutdown();
    }
    CoUninitialize();
    return result;
}
