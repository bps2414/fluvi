// SPDX-License-Identifier: GPL-3.0-only
// WASAPI shared-mode render: IAudioClient3::InitializeSharedAudioStream at the smallest engine period, event-driven
// thread (MMCSS "Pro Audio"). Follows default-device changes when no device id is given and reopens after device
// loss. It pulls stereo float audio from a callback and maps it to the device's channel layout.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "fluvi/core/Status.h"

namespace fluvi::audio {

class WasapiRenderer {
public:
    // Called on the render thread, once per device period: fill `frames` stereo float frames at `rate`.
    using RenderFn = std::function<void(float* stereo, size_t frames, uint32_t rate)>;

    WasapiRenderer(std::wstring deviceId, RenderFn render);
    ~WasapiRenderer();
    WasapiRenderer(const WasapiRenderer&) = delete;
    WasapiRenderer& operator=(const WasapiRenderer&) = delete;

    // Starts the thread and waits for the first open attempt. A failed attempt is reported but the thread keeps
    // retrying (a device may appear later), so the Status is informational.
    Status Start();
    void Stop();

    double LatencyMs() const { return latencyMs_.load(); }     // 0 while no device is open
    uint32_t OpenCount() const { return opens_.load(); }       // successful (re)opens so far
    uint32_t DeviceRate() const { return rate_.load(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<double> latencyMs_{0};
    std::atomic<uint32_t> opens_{0}, rate_{0};
    friend struct Impl;
};

} // namespace fluvi::audio
