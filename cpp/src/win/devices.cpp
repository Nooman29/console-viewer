// Finding cameras and sound inputs, plus the small helpers the other files share.
#include "app.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <propsys.h>

#include <cstring>

namespace cvwin {

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &out[0], size);
    return out;
}

std::string narrow(const std::wstring& utf16) {
    if (utf16.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), &out[0], size, nullptr, nullptr);
    return out;
}

// ---- FrameStore ----

FrameStore::FrameStore() : event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}

FrameStore::~FrameStore() {
    if (event_) CloseHandle(event_);
}

void FrameStore::put(const std::uint8_t* topRow, int width, int height, int stride) {
    if (!topRow || width <= 0 || height <= 0) return;
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frame_.width = width;
        frame_.height = height;
        frame_.pixels.resize(rowBytes * static_cast<std::size_t>(height));
        for (int y = 0; y < height; ++y) {
            // `stride` is signed: a negative one walks backwards through memory.
            std::memcpy(&frame_.pixels[rowBytes * static_cast<std::size_t>(y)],
                        topRow + static_cast<std::ptrdiff_t>(stride) * y, rowBytes);
        }
        ++count_;
    }
    SetEvent(event_);
}

bool FrameStore::copyIfNewer(Frame& out, std::uint64_t& seenCount) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t now = count_.load();
    if (now == seenCount || frame_.width == 0) return false;
    out = frame_;
    seenCount = now;
    return true;
}

void FrameStore::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    frame_ = Frame{};
    ++count_;
}

// ---- Cameras and capture cards ----

std::vector<cv::VideoDevice> listVideoDevices() {
    std::vector<cv::VideoDevice> devices;
    IMFAttributes* attributes = nullptr;
    if (FAILED(MFCreateAttributes(&attributes, 1))) return devices;
    attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** found = nullptr;
    UINT32 count = 0;
    if (SUCCEEDED(MFEnumDeviceSources(attributes, &found, &count))) {
        for (UINT32 i = 0; i < count; ++i) {
            WCHAR* name = nullptr;
            WCHAR* link = nullptr;
            UINT32 length = 0;
            if (SUCCEEDED(found[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &length)) &&
                SUCCEEDED(found[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                                       &link, &length))) {
                devices.push_back({narrow(name), narrow(link)});
            }
            CoTaskMemFree(name);
            CoTaskMemFree(link);
            found[i]->Release();
        }
        CoTaskMemFree(found);
    }
    attributes->Release();
    return devices;
}

// ---- Sound inputs ----

std::vector<cv::AudioDevice> listAudioInputs() {
    // Spelled out here so no extra import library is needed for one constant.
    static const PROPERTYKEY friendlyName = {
        {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

    std::vector<cv::AudioDevice> devices;
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)))) {
        return devices;
    }
    IMMDeviceCollection* collection = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection))) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(i, &device))) continue;
            WCHAR* id = nullptr;
            IPropertyStore* store = nullptr;
            if (SUCCEEDED(device->GetId(&id)) && SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(store->GetValue(friendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
                    devices.push_back({narrow(value.pwszVal), narrow(id)});
                }
                PropVariantClear(&value);
            }
            safeRelease(store);
            CoTaskMemFree(id);
            device->Release();
        }
        collection->Release();
    }
    enumerator->Release();
    return devices;
}

}  // namespace cvwin
