// Playing the capture card's sound through the PC's speakers with WASAPI.
//
// One thread reads small blocks of sound from the card and writes them
// straight to the default output. Windows converts the format if the two
// devices disagree about it.
#include "app.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <cstring>
#include <future>
#include <memory>
#include <thread>

namespace cvwin {
namespace {

// The "samples are floating point" marker, spelled out so no extra import
// library is needed for one constant.
const GUID kFloatSubtype = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kPcmSubtype = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

enum class SampleKind { Float32, Int16, Other };

SampleKind sampleKind(const WAVEFORMATEX* format) {
    WORD tag = format->wFormatTag;
    GUID subtype = GUID_NULL;
    if (tag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
        subtype = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format)->SubFormat;
        if (subtype == kFloatSubtype) tag = WAVE_FORMAT_IEEE_FLOAT;
        if (subtype == kPcmSubtype) tag = WAVE_FORMAT_PCM;
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32) return SampleKind::Float32;
    if (tag == WAVE_FORMAT_PCM && format->wBitsPerSample == 16) return SampleKind::Int16;
    return SampleKind::Other;
}

class Passthrough : public cv::AudioOutput {
public:
    ~Passthrough() override {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }

    void setVolume(float volume, bool muted) override { gain_ = muted ? 0.0f : volume; }
    bool failed() const override { return failed_; }

    bool start(const cv::AudioDevice& device) {
        // Shared, so the answer outlives whichever side finishes first.
        auto ready = std::make_shared<std::promise<bool>>();
        std::future<bool> started = ready->get_future();
        thread_ = std::thread([this, id = widen(device.id), ready] { run(id, *ready); });
        return started.get();
    }

private:
    void run(const std::wstring& inputId, std::promise<bool>& ready) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* input = nullptr;
        IMMDevice* output = nullptr;
        IAudioClient* captureClient = nullptr;
        IAudioClient* renderClient = nullptr;
        IAudioCaptureClient* capture = nullptr;
        IAudioRenderClient* render = nullptr;
        WAVEFORMATEX* format = nullptr;

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
        if (SUCCEEDED(hr)) hr = enumerator->GetDevice(inputId.c_str(), &input);
        if (SUCCEEDED(hr)) hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &output);
        if (SUCCEEDED(hr)) {
            hr = input->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**>(&captureClient));
        }
        if (SUCCEEDED(hr)) {
            hr = output->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(&renderClient));
        }
        if (SUCCEEDED(hr)) hr = captureClient->GetMixFormat(&format);
        // 20 ms in, 60 ms out: short enough to stay in step with the picture,
        // long enough not to crackle.
        if (SUCCEEDED(hr)) hr = captureClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 200000, 0, format, nullptr);
        if (SUCCEEDED(hr)) {
            // Play it in the card's own format and let Windows convert.
            hr = renderClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                          AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                          600000, 0, format, nullptr);
        }
        if (SUCCEEDED(hr)) {
            hr = captureClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture));
        }
        if (SUCCEEDED(hr)) {
            hr = renderClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
        }
        UINT32 renderFrames = 0;
        if (SUCCEEDED(hr)) hr = renderClient->GetBufferSize(&renderFrames);
        if (SUCCEEDED(hr)) hr = captureClient->Start();
        if (SUCCEEDED(hr)) hr = renderClient->Start();
        ready.set_value(SUCCEEDED(hr));

        if (SUCCEEDED(hr)) {
            const SampleKind kind = sampleKind(format);
            const UINT32 blockAlign = format->nBlockAlign;
            const UINT32 channels = format->nChannels;
            // More than this waiting to be played means we have drifted
            // behind the picture; skip a block to catch up.
            const UINT32 tooMuch = renderFrames * 2 / 3;
            bool broken = false;
            while (!stop_ && !broken) {
                Sleep(4);
                UINT32 packet = 0;
                while (!stop_ && !broken) {
                    // Any failure here means a device went away: the card's
                    // sound input, or the speakers or headphones.
                    if (FAILED(capture->GetNextPacketSize(&packet))) { broken = true; break; }
                    if (packet == 0) break;
                    BYTE* data = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;
                    if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) { broken = true; break; }
                    UINT32 padding = 0;
                    if (FAILED(renderClient->GetCurrentPadding(&padding))) broken = true;
                    const UINT32 room = renderFrames > padding ? renderFrames - padding : 0;
                    const UINT32 count = (broken || padding > tooMuch) ? 0 : (frames < room ? frames : room);
                    BYTE* out = nullptr;
                    if (count > 0 && FAILED(render->GetBuffer(count, &out))) {
                        broken = true;
                        out = nullptr;
                    }
                    if (out) {
                        const float gain = gain_;
                        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) || gain <= 0.0f;
                        if (silent) {
                            std::memset(out, 0, static_cast<std::size_t>(count) * blockAlign);
                        } else if (gain >= 0.999f || kind == SampleKind::Other) {
                            std::memcpy(out, data, static_cast<std::size_t>(count) * blockAlign);
                        } else if (kind == SampleKind::Float32) {
                            const float* from = reinterpret_cast<const float*>(data);
                            float* to = reinterpret_cast<float*>(out);
                            for (UINT32 i = 0; i < count * channels; ++i) to[i] = from[i] * gain;
                        } else {
                            const short* from = reinterpret_cast<const short*>(data);
                            short* to = reinterpret_cast<short*>(out);
                            for (UINT32 i = 0; i < count * channels; ++i) to[i] = static_cast<short>(from[i] * gain);
                        }
                        render->ReleaseBuffer(count, 0);
                    }
                    capture->ReleaseBuffer(frames);
                }
            }
            captureClient->Stop();
            renderClient->Stop();
            if (broken) failed_ = true;  // the controller drops this and starts sound again
        }

        if (format) CoTaskMemFree(format);
        safeRelease(render);
        safeRelease(capture);
        safeRelease(renderClient);
        safeRelease(captureClient);
        safeRelease(output);
        safeRelease(input);
        safeRelease(enumerator);
        if (SUCCEEDED(com)) CoUninitialize();
    }

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<float> gain_{1.0f};
    std::atomic<bool> failed_{false};
};

}  // namespace

std::unique_ptr<cv::AudioOutput> openPassthrough(const cv::AudioDevice& device) {
    auto passthrough = std::make_unique<Passthrough>();
    if (!passthrough->start(device)) return nullptr;
    return passthrough;
}

}  // namespace cvwin
