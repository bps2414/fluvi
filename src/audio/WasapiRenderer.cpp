// SPDX-License-Identifier: GPL-3.0-only
#include "WasapiRenderer.h"
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include "fluvi/core/Log.h"

using Microsoft::WRL::ComPtr;

namespace fluvi::audio {
namespace {

std::string HrText(HRESULT hr) { return std::format("0x{:08x}", static_cast<unsigned>(hr)); }

// Watches the endpoint list; only ever sets an event (called on arbitrary COM threads).
class NotificationClient final : public IMMNotificationClient {
public:
    NotificationClient(HANDLE reopen, bool followDefault, std::wstring explicitId)
        : reopen_(reopen), followDefault_(followDefault), explicitId_(std::move(explicitId)) {}

    void SetCurrent(std::wstring id) {
        std::lock_guard lock(mutex_);
        current_ = std::move(id);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }     // owned by the renderer, never freed by COM
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (followDefault_ && flow == eRender && role == eConsole) SetEvent(reopen_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
        if (state != DEVICE_STATE_ACTIVE && IsOurs(id)) SetEvent(reopen_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {
        if (IsOurs(id)) SetEvent(reopen_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    bool IsOurs(LPCWSTR id) {
        if (!id) return false;
        std::lock_guard lock(mutex_);
        return id == current_ || id == explicitId_;
    }
    HANDLE reopen_;
    bool followDefault_;
    std::wstring explicitId_;
    std::mutex mutex_;
    std::wstring current_;
};

struct Stream {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    HANDLE event = nullptr;
    UINT32 bufferFrames = 0, channels = 2, rate = 48000;
    std::wstring id;
    ~Stream() { if (event) CloseHandle(event); }
};

enum class LoopEnd { Stop, Reopen, Lost };

} // namespace

struct WasapiRenderer::Impl {
    WasapiRenderer* owner = nullptr;
    std::wstring deviceId;
    RenderFn renderFn;
    HANDLE stopEvent = nullptr, reopenEvent = nullptr, firstAttempt = nullptr;
    std::thread thread;
    std::mutex statusMutex;
    Status firstStatus;
    std::atomic<bool> firstDone{false};
    std::vector<float> scratch;

    void ReportFirst(const Status& st) {
        if (firstDone.exchange(true)) return;
        { std::lock_guard lock(statusMutex); firstStatus = st; }
        SetEvent(firstAttempt);
    }

    Status Open(IMMDeviceEnumerator* enumerator, Stream& s) {
        HRESULT hr;
        if (deviceId.empty()) hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &s.device);
        else hr = enumerator->GetDevice(deviceId.c_str(), &s.device);
        if (FAILED(hr)) return Status::Fail("audio endpoint not found: " + HrText(hr));
        LPWSTR id = nullptr;
        if (SUCCEEDED(s.device->GetId(&id)) && id) { s.id = id; CoTaskMemFree(id); }

        ComPtr<IAudioClient3> client3;
        hr = s.device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client3.GetAddressOf()));
        if (SUCCEEDED(hr)) s.client = client3;
        else {
            hr = s.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(s.client.GetAddressOf()));
            if (FAILED(hr)) return Status::Fail("IAudioClient activation failed: " + HrText(hr));
        }

        WAVEFORMATEX* mix = nullptr;
        hr = s.client->GetMixFormat(&mix);
        if (FAILED(hr) || !mix) return Status::Fail("GetMixFormat failed: " + HrText(hr));
        struct FreeFmt { WAVEFORMATEX* p; ~FreeFmt() { CoTaskMemFree(p); } } freeFmt{mix};
        bool isFloat = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
            isFloat = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        if (!isFloat || mix->wBitsPerSample != 32) return Status::Fail("shared-mode mix format is not float32");
        s.channels = mix->nChannels;
        s.rate = mix->nSamplesPerSec;

        s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        UINT32 periodFrames = 0;
        bool lowLatency = false;
        if (client3) {
            UINT32 def = 0, fund = 0, minp = 0, maxp = 0;
            hr = client3->GetSharedModeEnginePeriod(mix, &def, &fund, &minp, &maxp);
            if (SUCCEEDED(hr)) {
                hr = client3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minp, mix, nullptr);
                if (SUCCEEDED(hr)) { periodFrames = minp; lowLatency = true; }
                else FLUVI_WARN("audio", "InitializeSharedAudioStream failed ({}), using the default period", HrText(hr));
            }
            if (!lowLatency) {   // an initialised-and-failed client cannot be reused: take a fresh one
                s.client.Reset();
                hr = s.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(s.client.GetAddressOf()));
                if (FAILED(hr)) return Status::Fail("IAudioClient activation failed: " + HrText(hr));
            }
        }
        if (!lowLatency) {
            hr = s.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mix, nullptr);
            if (FAILED(hr)) return Status::Fail("IAudioClient::Initialize failed: " + HrText(hr));
            REFERENCE_TIME def = 0, minT = 0;
            if (SUCCEEDED(s.client->GetDevicePeriod(&def, &minT)))
                periodFrames = static_cast<UINT32>(def * s.rate / 10'000'000);
        }
        hr = s.client->SetEventHandle(s.event);
        if (FAILED(hr)) return Status::Fail("SetEventHandle failed: " + HrText(hr));
        hr = s.client->GetBufferSize(&s.bufferFrames);
        if (FAILED(hr)) return Status::Fail("GetBufferSize failed: " + HrText(hr));
        hr = s.client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(s.render.GetAddressOf()));
        if (FAILED(hr)) return Status::Fail("IAudioRenderClient failed: " + HrText(hr));

        REFERENCE_TIME streamLatency = 0;
        s.client->GetStreamLatency(&streamLatency);
        const double latency = double(streamLatency) / 10000.0 + double(periodFrames) * 1000.0 / double(s.rate);
        owner->latencyMs_ = latency;
        owner->rate_ = s.rate;
        FLUVI_INFO("audio", "WASAPI open: {} Hz, {} ch, period {} frames ({:.2f} ms){}, buffer {} frames, latency {:.2f} ms",
                   s.rate, s.channels, periodFrames, double(periodFrames) * 1000.0 / double(s.rate),
                   lowLatency ? " [IAudioClient3 minimum]" : " [default period]", s.bufferFrames, latency);
        return Status::Ok();
    }

    // Renders `frames` into the device buffer (stereo scratch -> device channel layout).
    bool Fill(Stream& s, UINT32 frames) {
        BYTE* data = nullptr;
        if (FAILED(s.render->GetBuffer(frames, &data))) return false;
        if (scratch.size() < size_t(frames) * 2) scratch.resize(size_t(frames) * 2);
        renderFn(scratch.data(), frames, s.rate);
        float* dst = reinterpret_cast<float*>(data);
        const float* src = scratch.data();
        if (s.channels == 2) {
            std::memcpy(dst, src, size_t(frames) * 2 * sizeof(float));
        } else if (s.channels == 1) {
            for (UINT32 i = 0; i < frames; ++i) dst[i] = 0.5f * (src[i * 2] + src[i * 2 + 1]);
        } else {   // surround: L/R go to the first two channels (front pair), the rest stays silent
            for (UINT32 i = 0; i < frames; ++i) {
                float* f = dst + size_t(i) * s.channels;
                std::fill(f, f + s.channels, 0.0f);
                f[0] = src[i * 2];
                f[1] = src[i * 2 + 1];
            }
        }
        return SUCCEEDED(s.render->ReleaseBuffer(frames, 0));
    }

    LoopEnd RunLoop(Stream& s) {
        scratch.assign(size_t(s.bufferFrames) * 2, 0.0f);
        if (!Fill(s, s.bufferFrames)) return LoopEnd::Lost;       // first buffer before Start
        if (FAILED(s.client->Start())) return LoopEnd::Lost;
        HANDLE waits[3] = {s.event, stopEvent, reopenEvent};
        for (;;) {
            const DWORD w = WaitForMultipleObjects(3, waits, FALSE, 2000);
            if (w == WAIT_OBJECT_0 + 1) return LoopEnd::Stop;
            if (w == WAIT_OBJECT_0 + 2) return LoopEnd::Reopen;
            if (w != WAIT_OBJECT_0) { FLUVI_WARN("audio", "render event stalled, reopening"); return LoopEnd::Lost; }
            UINT32 padding = 0;
            if (FAILED(s.client->GetCurrentPadding(&padding))) return LoopEnd::Lost;
            if (padding >= s.bufferFrames) continue;
            if (!Fill(s, s.bufferFrames - padding)) return LoopEnd::Lost;
        }
    }

    void Run() {
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        DWORD task = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
        if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);

        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        NotificationClient notify(reopenEvent, deviceId.empty(), deviceId);
        if (FAILED(hr)) {
            ReportFirst(Status::Fail("MMDeviceEnumerator failed: " + HrText(hr)));
        } else {
            enumerator->RegisterEndpointNotificationCallback(&notify);
            for (;;) {
                ResetEvent(reopenEvent);
                LoopEnd end = LoopEnd::Lost;
                {
                    Stream s;
                    Status st = Open(enumerator.Get(), s);
                    ReportFirst(st);
                    if (st) {
                        notify.SetCurrent(s.id);
                        ++owner->opens_;
                        end = RunLoop(s);
                        if (s.client) s.client->Stop();
                    } else {
                        FLUVI_WARN("audio", "cannot open the audio device: {}", st.message);
                    }
                    owner->latencyMs_ = 0;
                    notify.SetCurrent({});
                }
                if (end == LoopEnd::Stop) break;
                if (end == LoopEnd::Lost) {          // device gone or open failed: retry shortly
                    FLUVI_WARN("audio", "audio device lost, retrying");
                    HANDLE waits[2] = {stopEvent, reopenEvent};
                    if (WaitForMultipleObjects(2, waits, FALSE, 500) == WAIT_OBJECT_0) break;
                } else if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
                    break;
                }
            }
            enumerator->UnregisterEndpointNotificationCallback(&notify);
        }
        ReportFirst(Status::Fail("stopped"));
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        if (com) CoUninitialize();
    }
};

WasapiRenderer::WasapiRenderer(std::wstring deviceId, RenderFn render) : impl_(std::make_unique<Impl>()) {
    impl_->owner = this;
    impl_->deviceId = std::move(deviceId);
    impl_->renderFn = std::move(render);
    impl_->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl_->reopenEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl_->firstAttempt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

WasapiRenderer::~WasapiRenderer() {
    Stop();
    CloseHandle(impl_->stopEvent);
    CloseHandle(impl_->reopenEvent);
    CloseHandle(impl_->firstAttempt);
}

Status WasapiRenderer::Start() {
    if (impl_->thread.joinable()) return Status::Ok();
    impl_->thread = std::thread([p = impl_.get()] { p->Run(); });
    if (WaitForSingleObject(impl_->firstAttempt, 5000) != WAIT_OBJECT_0) return Status::Fail("audio device open timed out");
    std::lock_guard lock(impl_->statusMutex);
    return impl_->firstStatus;
}

void WasapiRenderer::Stop() {
    if (!impl_->thread.joinable()) return;
    SetEvent(impl_->stopEvent);
    impl_->thread.join();
}

} // namespace fluvi::audio
