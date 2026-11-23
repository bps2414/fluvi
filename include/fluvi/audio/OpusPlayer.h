// SPDX-License-Identifier: GPL-3.0-only
// Fluvi audio: Opus packets from the stream -> WASAPI shared mode with the smallest engine period
// (IAudioClient3), a small jitter buffer and drift correction.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include "fluvi/core/Status.h"

namespace fluvi::audio {

struct OpusPlayerConfig {
    std::wstring deviceId;            // empty = default render endpoint (follows default-device changes)
    float volume = 1.0f;              // 0..1
    uint32_t targetBufferMs = 30;     // jitter buffer target; the player keeps the fill around it
    uint32_t sampleRate = 48000;      // the stream's Opus rate
    uint32_t channels = 2;
};

struct AudioStats {
    double bufferedMs = 0;
    double deviceLatencyMs = 0;       // engine period + stream latency reported by WASAPI
    double driftPpm = 0;              // correction currently applied
    uint64_t packets = 0, lostPackets = 0, concealedFrames = 0, underruns = 0, overflowDrops = 0;
};

class OpusPlayer {
public:
    static std::unique_ptr<OpusPlayer> Create(const OpusPlayerConfig& config, Status& status);
    virtual ~OpusPlayer() = default;

    // Any thread, never blocks. rtpTimestamp (48 kHz) detects loss (-> Opus PLC/FEC) and reordering.
    virtual void Push(const uint8_t* data, size_t size, uint32_t rtpTimestamp) = 0;
    virtual void SetVolume(float volume) = 0;
    virtual void Flush() = 0;          // drop everything buffered (reconnect, seek)
    virtual AudioStats Stats() const = 0;
};

} // namespace fluvi::audio
