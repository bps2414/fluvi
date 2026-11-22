// SPDX-License-Identifier: GPL-3.0-only
// Everything between "Opus packet arrived" and "PCM for the device" except the device itself: jitter buffer,
// libopus decode with PLC/FEC, drift controller and fractional resampler. No Windows audio here, so it is testable
// with a simulated clock. Push() is called from any thread (short lock, a memcpy); Render() from one consumer thread.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include "DriftController.h"
#include "DriftResampler.h"
#include "JitterBuffer.h"
#include "fluvi/audio/OpusPlayer.h"
#include "fluvi/core/Status.h"

struct OpusDecoder;

namespace fluvi::audio {

struct PlayoutConfig {
    uint32_t streamRate = 48000;       // Opus rate (48 kHz stereo)
    uint32_t targetBufferMs = 30;
    bool driftCorrection = true;       // tests turn it off for bit-exact checks
    size_t maxPackets = 64;
};

class PlayoutEngine {
public:
    static std::unique_ptr<PlayoutEngine> Create(const PlayoutConfig& cfg, Status& status);
    ~PlayoutEngine();

    void Push(const uint8_t* data, size_t size, uint32_t rtpTimestamp);          // any thread
    // Fills `frames` stereo float frames at outRate (volume applied). Always writes all of them.
    void Render(float* stereoOut, size_t frames, uint32_t outRate);               // consumer thread
    void SetVolume(float volume) { volume_.store(volume < 0 ? 0 : (volume > 1 ? 1 : volume)); }
    void Flush() { flush_.store(true); }
    AudioStats Stats() const;

private:
    PlayoutEngine() = default;
    bool DecodeOne();
    void ApplyFlush();
    void Discontinuity();              // fade the pending tail out and the next decoded audio in

    PlayoutConfig cfg_;
    OpusDecoder* decoder_ = nullptr;

    // shared with Push()
    mutable std::mutex mutex_;
    JitterBuffer jitter_;

    // consumer-thread only
    DriftController drift_;
    DriftResampler resampler_;
    std::vector<uint8_t> packetBuf_;
    std::vector<float> pcm_;
    uint32_t outRate_ = 0;
    bool priming_ = true;
    size_t fadeInFrames_ = 0;
    float gain_ = 1.0f;

    std::atomic<float> volume_{1.0f};
    std::atomic<bool> flush_{false};

    // stats
    std::atomic<uint64_t> packets_{0}, lost_{0}, concealed_{0}, underruns_{0}, overflow_{0};
    std::atomic<double> bufferedMs_{0}, driftPpm_{0};
};

} // namespace fluvi::audio
