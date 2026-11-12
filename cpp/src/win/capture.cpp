// Reading pictures from a capture card with Media Foundation.
//
// A background thread asks the device for one frame after another and hands
// each to the FrameStore, which keeps only the newest. Nothing queues up, so
// the picture on screen is as fresh as the device can deliver.
#include "app.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace cvwin {
namespace {

const DWORD kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

// "Keep the delay as short as possible." Spelled out because not every
// compiler's Windows headers declare it (it is MF_LOW_LATENCY).
const GUID kLowLatency = {0x9c27891a, 0xed7a, 0x40e1, {0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee}};

std::string hresultText(const char* what, HRESULT hr) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%s (error 0x%08lX).", what, static_cast<unsigned long>(hr));
    return buffer;
}

class MediaFoundationSource : public cv::VideoSource {
public:
    MediaFoundationSource(FrameStore& store) : store_(store) {}

    ~MediaFoundationSource() override {
        stop_ = true;
        // A working device hands over its next frame within a few
        // milliseconds and the thread then sees the flag. Only if it has
        // stalled is the device shut down to break the read.
        if (thread_.joinable()) {
            if (WaitForSingleObject(finished_, 500) != WAIT_OBJECT_0 && source_) source_->Shutdown();
            thread_.join();
        }
        if (finished_) CloseHandle(finished_);
        if (firstAnswer_) CloseHandle(firstAnswer_);
        // Releasing the reader shuts the device down too, but if the reader
        // was never created the device would otherwise stay open.
        if (source_) source_->Shutdown();
        safeRelease(reader_);
        safeRelease(source_);
        store_.clear();
    }

    bool failed() const override {
        // Either the device reported an error, or it has stayed plugged in
        // but sent nothing for several seconds.
        return failed_ || GetTickCount64() - lastFrameTick_.load() > kStallMilliseconds;
    }

    bool open(const cv::VideoDevice& device, const CaptureSettings& settings, std::string& error) {
        IMFAttributes* attributes = nullptr;
        HRESULT hr = MFCreateAttributes(&attributes, 2);
        if (SUCCEEDED(hr)) {
            attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
            attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                  widen(device.key).c_str());
            hr = MFCreateDeviceSource(attributes, &source_);
        }
        safeRelease(attributes);
        if (FAILED(hr)) {
            error = "Could not open " + device.name + ". Another app may be using it.";
            return false;
        }

        // Let Windows convert whatever the device sends (MJPEG, YUY2, NV12)
        // into plain 32-bit pixels we can draw.
        IMFAttributes* readerAttributes = nullptr;
        hr = MFCreateAttributes(&readerAttributes, 2);
        if (SUCCEEDED(hr)) {
            readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
            readerAttributes->SetUINT32(kLowLatency, TRUE);
            hr = MFCreateSourceReaderFromMediaSource(source_, readerAttributes, &reader_);
        }
        safeRelease(readerAttributes);
        if (FAILED(hr)) {
            error = hresultText("Could not read from the device", hr);
            return false;
        }

        chooseNativeFormat(settings);

        IMFMediaType* wanted = nullptr;
        hr = MFCreateMediaType(&wanted);
        if (SUCCEEDED(hr)) {
            wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            wanted->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            hr = reader_->SetCurrentMediaType(kVideoStream, nullptr, wanted);
        }
        safeRelease(wanted);
        if (FAILED(hr)) {
            error = hresultText("The device's picture format is not supported", hr);
            return false;
        }
        reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        reader_->SetStreamSelection(kVideoStream, TRUE);
        if (!readOutputSize()) {
            error = "The device did not report a picture size.";
            return false;
        }
        lastFrameTick_ = GetTickCount64();
        thread_ = std::thread([this] { run(); });
        // A device that another app is using opens without complaint and only
        // fails when the first picture is asked for. Wait briefly for that
        // answer, so it is reported as "could not open" and the next device
        // gets a turn, instead of looking like an unplug.
        WaitForSingleObject(firstAnswer_, 3000);
        if (failed_) {
            error = "Could not open " + device.name + ". Another app may be using it.";
            return false;
        }
        return true;
    }

private:
    // Ask the device for the best of the formats it offers.
    void chooseNativeFormat(const CaptureSettings& settings) {
        std::vector<cv::VideoFormat> formats;
        std::vector<IMFMediaType*> types;
        for (DWORD i = 0;; ++i) {
            IMFMediaType* type = nullptr;
            if (FAILED(reader_->GetNativeMediaType(kVideoStream, i, &type))) break;
            cv::VideoFormat format;
            UINT32 width = 0, height = 0, numerator = 0, denominator = 0;
            GUID subtype = GUID_NULL;
            MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height);
            MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &numerator, &denominator);
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            format.width = static_cast<int>(width);
            format.height = static_cast<int>(height);
            format.fps = denominator ? static_cast<double>(numerator) / denominator : 0.0;
            format.compressed = (subtype == MFVideoFormat_MJPG) || (subtype == MFVideoFormat_H264);
            formats.push_back(format);
            types.push_back(type);
        }
        const int best = cv::pickFormat(formats, settings.width, settings.height, settings.fps);
        if (best >= 0) reader_->SetCurrentMediaType(kVideoStream, nullptr, types[static_cast<std::size_t>(best)]);
        for (IMFMediaType* type : types) type->Release();
    }

    bool readOutputSize() {
        IMFMediaType* type = nullptr;
        if (FAILED(reader_->GetCurrentMediaType(kVideoStream, &type))) return false;
        UINT32 width = 0, height = 0;
        MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height);
        // Without a 2D buffer this says whether the rows arrive bottom-first.
        LONG stride = 0;
        if (FAILED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32*>(&stride)))) {
            // Not stated: ask Windows, which answers "bottom row first" for
            // this format unless told otherwise.
            if (FAILED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, width, &stride))) {
                stride = static_cast<LONG>(width) * 4;
            }
        }
        type->Release();
        width_ = static_cast<int>(width);
        height_ = static_cast<int>(height);
        defaultStride_ = stride;
        return width_ > 0 && height_ > 0;
    }

    void run() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (!stop_) {
            DWORD flags = 0;
            IMFSample* sample = nullptr;
            const HRESULT hr = reader_->ReadSample(kVideoStream, 0, nullptr, &flags, nullptr, &sample);
            if (stop_) {
                safeRelease(sample);
                break;
            }
            if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
                safeRelease(sample);
                failed_ = true;  // unplugged, in use elsewhere, or the device gave up
                break;
            }
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) readOutputSize();
            if (sample) {
                deliver(sample);
                sample->Release();
                lastFrameTick_ = GetTickCount64();
                SetEvent(firstAnswer_);
            }
        }
        SetEvent(firstAnswer_);
        if (SUCCEEDED(com)) CoUninitialize();
        SetEvent(finished_);
    }

    void deliver(IMFSample* sample) {
        IMFMediaBuffer* buffer = nullptr;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return;
        IMF2DBuffer* buffer2d = nullptr;
        if (SUCCEEDED(buffer->QueryInterface(__uuidof(IMF2DBuffer), reinterpret_cast<void**>(&buffer2d)))) {
            BYTE* topRow = nullptr;
            LONG pitch = 0;
            if (SUCCEEDED(buffer2d->Lock2D(&topRow, &pitch))) {
                store_.put(topRow, width_, height_, static_cast<int>(pitch));
                buffer2d->Unlock2D();
            }
            buffer2d->Release();
        } else {
            BYTE* data = nullptr;
            DWORD length = 0;
            if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
                const LONG stride = defaultStride_;
                const std::size_t needed = static_cast<std::size_t>(width_) * 4 * static_cast<std::size_t>(height_);
                if (length >= needed) {
                    // A negative stride means the last row in memory is the top row.
                    const BYTE* topRow = stride < 0 ? data + static_cast<std::size_t>(-stride) * (height_ - 1) : data;
                    store_.put(topRow, width_, height_, static_cast<int>(stride));
                }
                buffer->Unlock();
            }
        }
        buffer->Release();
    }

    FrameStore& store_;
    IMFMediaSource* source_ = nullptr;
    IMFSourceReader* reader_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    HANDLE finished_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);     // set when the thread ends
    HANDLE firstAnswer_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // set on the first picture or failure
    std::atomic<ULONGLONG> lastFrameTick_{0};
    static constexpr ULONGLONG kStallMilliseconds = 5000;
    int width_ = 0, height_ = 0;
    LONG defaultStride_ = 0;
};

// A moving test picture, paced like a real device.
class DemoSource : public cv::VideoSource {
public:
    DemoSource(const CaptureSettings& settings, FrameStore& store)
        : store_(store), fps_(settings.fps > 0 ? settings.fps : 60.0) {
        thread_ = std::thread([this] { run(); });
    }
    ~DemoSource() override {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        store_.clear();
    }
    bool failed() const override { return false; }

private:
    void run() {
        const int width = 1280, height = 720;
        std::vector<std::uint8_t> background(static_cast<std::size_t>(width) * height * 4);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                std::uint8_t* p = &background[(static_cast<std::size_t>(y) * width + x) * 4];
                const int ramp = 40 + x * 160 / width;
                p[0] = static_cast<std::uint8_t>(200 - (ramp - 40));  // blue
                p[1] = 90;                                             // green
                p[2] = static_cast<std::uint8_t>(ramp);                // red
                p[3] = 255;
            }
        }
        std::vector<std::uint8_t> frame;
        const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t n = 0; !stop_; ++n) {
            const auto due = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                         std::chrono::duration<double>(static_cast<double>(n) / fps_));
            std::this_thread::sleep_until(due);
            frame = background;
            const int size = height / 6;
            const int left = static_cast<int>((n * 6) % static_cast<std::uint64_t>(width - size));
            const int top = height / 2 - size / 2;
            for (int y = top; y < top + size; ++y) {
                std::uint8_t* row = &frame[(static_cast<std::size_t>(y) * width + left) * 4];
                std::fill(row, row + static_cast<std::size_t>(size) * 4, static_cast<std::uint8_t>(255));
            }
            store_.put(frame.data(), width, height, width * 4);
        }
    }

    FrameStore& store_;
    double fps_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

}  // namespace

std::unique_ptr<cv::VideoSource> openCapture(const cv::VideoDevice& device, const CaptureSettings& settings,
                                             FrameStore& store, std::string& error) {
    auto source = std::make_unique<MediaFoundationSource>(store);
    if (!source->open(device, settings, error)) return nullptr;
    return source;
}

std::unique_ptr<cv::VideoSource> openDemo(const CaptureSettings& settings, FrameStore& store) {
    return std::make_unique<DemoSource>(settings, store);
}

}  // namespace cvwin
