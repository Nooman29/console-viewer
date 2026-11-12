// Shared declarations for the Windows side of the app.
//
// core/ decides what to do. The files in win/ do it: Media Foundation reads
// the capture card, WASAPI plays its sound, Win32 and GDI draw the window.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/controller.h"

namespace cvwin {

// ---- Text ----
std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& utf16);

// Releases a COM pointer and clears it.
template <typename T>
void safeRelease(T*& pointer) {
    if (pointer) {
        pointer->Release();
        pointer = nullptr;
    }
}

// ---- The newest picture, shared between the capture thread and the window ----
struct Frame {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;  // 32 bits per pixel, blue-green-red-unused, top row first
};

class FrameStore {
public:
    FrameStore();
    ~FrameStore();
    FrameStore(const FrameStore&) = delete;
    FrameStore& operator=(const FrameStore&) = delete;

    // Called by the capture thread. `stride` may be negative (bottom row first in memory).
    void put(const std::uint8_t* topRow, int width, int height, int stride);
    // Copies the newest frame into `out` if it is newer than `seenCount`.
    bool copyIfNewer(Frame& out, std::uint64_t& seenCount);
    std::uint64_t count() const { return count_.load(); }
    // Forget the picture, so a closed device's last frame is not shown again.
    void clear();
    // Signalled every time a new frame arrives.
    HANDLE newFrameEvent() const { return event_; }

private:
    std::mutex mutex_;
    Frame frame_;
    std::atomic<std::uint64_t> count_{0};
    HANDLE event_;
};

// ---- Devices ----
std::vector<cv::VideoDevice> listVideoDevices();
std::vector<cv::AudioDevice> listAudioInputs();

// ---- Picture ----
struct CaptureSettings {
    int width = 1920;
    int height = 1080;
    double fps = 60;
};
// A real camera or capture card. Returns null and fills `error` on failure.
std::unique_ptr<cv::VideoSource> openCapture(const cv::VideoDevice& device, const CaptureSettings& settings,
                                             FrameStore& store, std::string& error);
// A moving test picture that needs no hardware.
std::unique_ptr<cv::VideoSource> openDemo(const CaptureSettings& settings, FrameStore& store);

// ---- Sound ----
// Plays a sound input through the default speakers. Returns null on failure.
std::unique_ptr<cv::AudioOutput> openPassthrough(const cv::AudioDevice& device);

// ---- Screenshots ----
// Saves the frame as a PNG in Downloads. Returns the folder's name, or "" on failure.
// `savedPath`, if given, receives the full path of the file.
std::string saveScreenshotPng(const Frame& frame, std::wstring* savedPath = nullptr,
                              const std::wstring& folderOverride = L"");

// ---- Window ----
class Window {
public:
    Window();
    ~Window();
    bool create(int width, int height);
    HWND handle() const { return hwnd_; }

    // Draws the picture (or the status screen) and everything on top of it.
    void paint(const Frame* frame, const cv::Controller& controller);
    void setFullscreen(bool on);

    // What happened since last asked.
    std::vector<cv::Key> takeKeys();
    bool takeDeviceChange();
    bool closed() const { return closed_; }

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    void makeFonts();
    void freeFonts();
    void ensureBackBuffer(HDC screen, int width, int height);

    HWND hwnd_ = nullptr;
    HDEVNOTIFY deviceNotify_ = nullptr;
    std::vector<cv::Key> keys_;
    bool deviceChanged_ = false;
    bool closed_ = false;

    bool fullscreen_ = false;
    WINDOWPLACEMENT placement_{};
    LONG_PTR savedStyle_ = 0;

    HDC backDc_ = nullptr;
    HBITMAP backBitmap_ = nullptr;
    HBITMAP backOld_ = nullptr;
    int backW_ = 0, backH_ = 0;

    HFONT titleFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    UINT fontDpi_ = 0;
};

}  // namespace cvwin
