// The window: drawing the picture and the text on top of it, and collecting
// key presses and "a device was plugged in" notices for the main loop.
#include "app.h"

#include <dbt.h>

#include <algorithm>
#include <cwchar>

namespace cvwin {
namespace {

const wchar_t* const kClassName = L"ConsoleViewerWindow";

const COLORREF kBackground = RGB(11, 13, 16);
const COLORREF kPanel = RGB(18, 20, 24);
const COLORREF kText = RGB(242, 244, 247);
const COLORREF kMuted = RGB(163, 172, 185);
const COLORREF kAccent = RGB(79, 140, 255);
const COLORREF kWarn = RGB(255, 176, 32);

const wchar_t* const kHelpLines[] = {
    L"F  full screen        M  mute           + / -  volume",
    L"S  screenshot         Z  fit or fill    H  this help",
    L"D  next video device  A  next sound     Q or Esc  quit",
};
const wchar_t* const kHdcpLines[] = {
    L"The picture is black.",
    L"Check the console is on. On a PlayStation, turn off Enable HDCP:",
    L"PS5: Settings > System > HDMI.   PS4: Settings > System.",
};

UINT dpiOf(HWND hwnd) {
    // GetDpiForWindow exists from Windows 10 1607; fall back for older systems.
    using Getter = UINT(WINAPI*)(HWND);
    static const Getter getter = reinterpret_cast<Getter>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    const UINT dpi = getter ? getter(hwnd) : 96;
    return dpi ? dpi : 96;
}

void fill(HDC dc, const RECT& area, COLORREF colour) {
    SetDCBrushColor(dc, colour);
    FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

int textHeight(HDC dc, HFONT font) {
    const HGDIOBJ old = SelectObject(dc, font);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, old);
    return metrics.tmHeight;
}

SIZE textSize(HDC dc, HFONT font, const std::wstring& text) {
    const HGDIOBJ old = SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, old);
    return size;
}

// Draws text centred in `area`, wrapping long lines. Returns the height used.
int drawCentered(HDC dc, HFONT font, const std::wstring& text, RECT area, COLORREF colour) {
    if (text.empty()) return 0;
    const HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, colour);
    const int height = DrawTextW(dc, text.c_str(), -1, &area, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    return height;
}

// A panel of left-aligned lines, centred horizontally at `top`.
template <std::size_t N>
void drawBox(HDC dc, HFONT font, const wchar_t* const (&lines)[N], int width, int height, int top, COLORREF edge) {
    const int lineHeight = textHeight(dc, font) + 6;
    int widest = 0;
    for (const wchar_t* line : lines) widest = std::max(widest, static_cast<int>(textSize(dc, font, line).cx));
    const int boxW = widest + 40;
    const int boxH = static_cast<int>(N) * lineHeight + 24;
    const int left = std::max((width - boxW) / 2, 0);
    top = std::min(std::max(top, 0), std::max(height - boxH, 0));
    const RECT outer{left, top, left + boxW, top + boxH};
    const RECT inner{left + 1, top + 1, left + boxW - 1, top + boxH - 1};
    fill(dc, outer, edge);
    fill(dc, inner, kPanel);
    const HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, kText);
    for (std::size_t i = 0; i < N; ++i) {
        TextOutW(dc, left + 20, top + 12 + static_cast<int>(i) * lineHeight, lines[i],
                 static_cast<int>(std::wcslen(lines[i])));
    }
    SelectObject(dc, old);
}

}  // namespace

Window::Window() = default;

Window::~Window() {
    if (deviceNotify_) UnregisterDeviceNotification(deviceNotify_);
    freeFonts();
    if (backDc_) {
        SelectObject(backDc_, backOld_);
        DeleteObject(backBitmap_);
        DeleteDC(backDc_);
    }
    if (hwnd_) DestroyWindow(hwnd_);
}

bool Window::create(int width, int height) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.style = CS_DBLCLKS;
    cls.lpfnWndProc = wndProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    cls.lpszClassName = kClassName;
    RegisterClassExW(&cls);

    RECT area{0, 0, width, height};
    AdjustWindowRect(&area, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd_ = CreateWindowExW(0, kClassName, L"Console Viewer", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            area.right - area.left, area.bottom - area.top, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;

    // Ask to be told when any device is plugged in or removed.
    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    deviceNotify_ = RegisterDeviceNotificationW(
        hwnd_, &filter, DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);

    ShowWindow(hwnd_, SW_SHOW);
    return true;
}

LRESULT CALLBACK Window::wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->hwnd_ == nullptr) self->hwnd_ = hwnd;
    return self ? self->handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT Window::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_KEYDOWN: {
            cv::Key key = cv::Key::None;
            switch (wParam) {
                case VK_ESCAPE: key = cv::Key::Escape; break;
                case 'Q': key = cv::Key::Quit; break;
                case 'H': key = cv::Key::Help; break;
                case 'F': case VK_F11: key = cv::Key::Fullscreen; break;
                case 'Z': key = cv::Key::Fill; break;
                case 'M': key = cv::Key::Mute; break;
                case 'S': key = cv::Key::Screenshot; break;
                case 'D': key = cv::Key::NextVideo; break;
                case 'A': key = cv::Key::NextAudio; break;
                case VK_OEM_PLUS: case VK_ADD: key = cv::Key::VolumeUp; break;
                case VK_OEM_MINUS: case VK_SUBTRACT: key = cv::Key::VolumeDown; break;
                default: break;
            }
            // Ignore the repeats Windows sends while a key is held down.
            const bool repeat = (lParam & (1 << 30)) != 0;
            const bool repeatable = key == cv::Key::VolumeUp || key == cv::Key::VolumeDown;
            if (key != cv::Key::None && (!repeat || repeatable)) keys_.push_back(key);
            return 0;
        }
        case WM_LBUTTONDBLCLK:
            keys_.push_back(cv::Key::Fullscreen);
            return 0;
        case WM_DEVICECHANGE:
            if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE || wParam == DBT_DEVNODES_CHANGED) {
                deviceChanged_ = true;
            }
            return TRUE;
        case WM_ERASEBKGND:
            return 1;  // the whole window is redrawn each time, so no flicker
        case WM_PAINT:
            ValidateRect(hwnd_, nullptr);  // the main loop draws
            return 0;
        case WM_CLOSE:
            closed_ = true;
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
    }
}

std::vector<cv::Key> Window::takeKeys() {
    std::vector<cv::Key> keys;
    keys.swap(keys_);
    return keys;
}

bool Window::takeDeviceChange() {
    const bool changed = deviceChanged_;
    deviceChanged_ = false;
    return changed;
}

void Window::setFullscreen(bool on) {
    if (on == fullscreen_ || !hwnd_) return;
    fullscreen_ = on;
    if (on) {
        placement_.length = sizeof(placement_);
        GetWindowPlacement(hwnd_, &placement_);
        savedStyle_ = GetWindowLongPtrW(hwnd_, GWL_STYLE);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, (savedStyle_ & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW)) | WS_POPUP);
        SetWindowPos(hwnd_, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                     monitor.rcMonitor.right - monitor.rcMonitor.left,
                     monitor.rcMonitor.bottom - monitor.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, savedStyle_);
        SetWindowPlacement(hwnd_, &placement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

void Window::makeFonts() {
    const UINT dpi = dpiOf(hwnd_);
    if (dpi == fontDpi_ && titleFont_) return;
    freeFonts();
    fontDpi_ = dpi;
    auto make = [dpi](int points, int weight) {
        return CreateFontW(-MulDiv(points, static_cast<int>(dpi), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    };
    titleFont_ = make(22, FW_SEMIBOLD);
    bodyFont_ = make(13, FW_NORMAL);
    smallFont_ = make(10, FW_NORMAL);
}

void Window::freeFonts() {
    for (HFONT* font : {&titleFont_, &bodyFont_, &smallFont_}) {
        if (*font) DeleteObject(*font);
        *font = nullptr;
    }
}

void Window::ensureBackBuffer(HDC screen, int width, int height) {
    if (backDc_ && width == backW_ && height == backH_) return;
    if (backDc_) {
        SelectObject(backDc_, backOld_);
        DeleteObject(backBitmap_);
        DeleteDC(backDc_);
    }
    backDc_ = CreateCompatibleDC(screen);
    backBitmap_ = CreateCompatibleBitmap(screen, width, height);
    backOld_ = static_cast<HBITMAP>(SelectObject(backDc_, backBitmap_));
    backW_ = width;
    backH_ = height;
    SetBkMode(backDc_, TRANSPARENT);
}

void Window::paint(const Frame* frame, const cv::Controller& controller) {
    if (!hwnd_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int width = client.right, height = client.bottom;
    if (width <= 0 || height <= 0) return;  // minimised

    const HDC screen = GetDC(hwnd_);
    ensureBackBuffer(screen, width, height);
    makeFonts();
    const HDC dc = backDc_;

    const bool live = controller.state() == cv::Controller::State::Live;
    if (live && frame && frame->width > 0) {
        fill(dc, client, RGB(0, 0, 0));
        const cv::Rect where = cv::fitRect(frame->width, frame->height, width, height, controller.fill());
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = frame->width;
        info.bmiHeader.biHeight = -frame->height;  // negative: the top row comes first
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, where.x, where.y, where.w, where.h, 0, 0, frame->width, frame->height,
                      frame->pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    } else {
        fill(dc, client, kBackground);
        const int margin = 40;
        int y = height * 36 / 100 - textHeight(dc, titleFont_);
        y += drawCentered(dc, titleFont_, L"Console Viewer", RECT{margin, y, width - margin, height}, kText) + 14;
        const std::wstring message = controller.message().empty() ? L"Connecting..." : widen(controller.message());
        y += drawCentered(dc, bodyFont_, message, RECT{margin, y, width - margin, height}, kText) + 10;
        drawCentered(dc, smallFont_, widen(controller.detail()), RECT{margin, y, width - margin, height}, kMuted);
        const int hintTop = height - textHeight(dc, smallFont_) - 16;
        drawCentered(dc, smallFont_, L"Press H for help", RECT{margin, hintTop, width - margin, height}, kMuted);
    }

    if (live && controller.showHdcpTip()) drawBox(dc, smallFont_, kHdcpLines, width, height, height - 170, kWarn);
    if (controller.showHelp()) drawBox(dc, smallFont_, kHelpLines, width, height, height / 2 - 60, kAccent);
    if (!controller.toast().empty()) {
        const std::wstring toast = widen(controller.toast());
        const SIZE size = textSize(dc, bodyFont_, toast);
        const int textW = static_cast<int>(size.cx), textH = static_cast<int>(size.cy);
        const int left = std::max((width - textW) / 2 - 16, 0);
        const int top = std::max(height - textH - 48, 0);
        fill(dc, RECT{left, top, left + textW + 32, top + textH + 16}, kPanel);
        const HGDIOBJ old = SelectObject(dc, bodyFont_);
        SetTextColor(dc, kText);
        TextOutW(dc, left + 16, top + 8, toast.c_str(), static_cast<int>(toast.size()));
        SelectObject(dc, old);
    }

    BitBlt(screen, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    ReleaseDC(hwnd_, screen);
}

}  // namespace cvwin
