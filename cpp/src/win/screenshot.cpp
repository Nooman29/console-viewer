// Saving the current picture as a PNG in the Downloads folder.
#include "app.h"

#include <shlobj.h>
#include <wincodec.h>

#include <cwchar>

namespace cvwin {
namespace {

std::wstring downloadsFolder() {
    PWSTR path = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &path)) && path) folder = path;
    CoTaskMemFree(path);
    return folder;
}

// console-viewer-20261111-140259.png, with -1, -2... if that name is taken.
std::wstring freePath(const std::wstring& folder) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t stem[64];
    std::swprintf(stem, 64, L"console-viewer-%04d%02d%02d-%02d%02d%02d", now.wYear, now.wMonth, now.wDay,
                  now.wHour, now.wMinute, now.wSecond);
    std::wstring path = folder + L"\\" + stem + L".png";
    for (int n = 1; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++n) {
        path = folder + L"\\" + stem + L"-" + std::to_wstring(n) + L".png";
    }
    return path;
}

std::wstring leafName(const std::wstring& folder) {
    const std::size_t slash = folder.find_last_of(L"\\/");
    return slash == std::wstring::npos ? folder : folder.substr(slash + 1);
}

}  // namespace

std::string saveScreenshotPng(const Frame& frame, std::wstring* savedPath, const std::wstring& folderOverride) {
    if (frame.width <= 0 || frame.height <= 0 || frame.pixels.empty()) return "";
    const std::wstring folder = folderOverride.empty() ? downloadsFolder() : folderOverride;
    if (folder.empty()) return "";
    const std::wstring path = freePath(folder);

    // The capture leaves the fourth byte of each pixel unset. PNG treats it
    // as transparency, so make every pixel fully solid first.
    std::vector<std::uint8_t> pixels = frame.pixels;
    for (std::size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* target = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IWICImagingFactory), reinterpret_cast<void**>(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&target, nullptr);
    if (SUCCEEDED(hr)) hr = target->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = target->SetSize(static_cast<UINT>(frame.width), static_cast<UINT>(frame.height));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = target->SetPixelFormat(&format);
    if (SUCCEEDED(hr) && format != GUID_WICPixelFormat32bppBGRA) hr = E_FAIL;
    if (SUCCEEDED(hr)) {
        hr = target->WritePixels(static_cast<UINT>(frame.height), static_cast<UINT>(frame.width) * 4,
                                 static_cast<UINT>(pixels.size()), pixels.data());
    }
    if (SUCCEEDED(hr)) hr = target->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();

    safeRelease(target);
    safeRelease(encoder);
    safeRelease(stream);
    safeRelease(factory);

    if (FAILED(hr)) {
        DeleteFileW(path.c_str());  // do not leave half a file behind
        return "";
    }
    if (savedPath) *savedPath = path;
    return narrow(leafName(folder));
}

}  // namespace cvwin
