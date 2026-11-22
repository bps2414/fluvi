// SPDX-License-Identifier: GPL-3.0-only
// Jitter buffer keyed by RTP timestamp: reorders, drops duplicates and late packets, and tells the decoder
// when a frame is missing (so it can run Opus PLC, or FEC when the next packet is already here).
// Pure class (no libopus, no Windows, not thread-safe): the owner locks around Insert/Pull. Packet storage is
// preallocated, so Insert never allocates.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fluvi::audio {

class JitterBuffer {
public:
    struct Config {
        size_t maxPackets = 64;
        size_t maxPacketBytes = 1500;        // Opus packets are <= 1275 bytes
        uint32_t maxGapSamples = 9600;       // a bigger hole (200 ms at 48 kHz) is a resync, not concealment
        uint32_t defaultFrameSamples = 960;  // frame size assumed for concealment before any packet was seen
        uint32_t frameGranularity = 120;     // 2.5 ms at 48 kHz: every Opus frame size is a multiple
    };

    enum class InsertResult : uint8_t { Accepted, AcceptedDroppedOldest, Duplicate, Late, Rejected };

    enum class PullKind : uint8_t {
        Empty,    // nothing buffered
        Packet,   // decode `data` normally
        Lost,     // conceal `samples`; if hasFec, `data` is the following packet (decode with FEC), else PLC
        Resync,   // timeline jumped (overflow drop, huge gap): reset the decoder, no concealment
    };
    struct Pulled {
        PullKind kind = PullKind::Empty;
        uint32_t samples = 0;     // Packet: its duration. Lost: samples to conceal now.
        size_t size = 0;          // bytes in the caller's buffer (Packet, or Lost with FEC)
        bool hasFec = false;
        uint32_t lostPackets = 0; // Lost: 1. Resync: estimate of packets skipped
    };

    struct Counters {
        uint64_t accepted = 0, duplicates = 0, late = 0, rejected = 0, droppedOldest = 0, lost = 0, resyncs = 0;
    };

    explicit JitterBuffer(const Config& cfg = {});

    InsertResult Insert(uint32_t rtpTimestamp, const uint8_t* data, size_t size, uint32_t samples);
    // `out` must hold at least maxPacketBytes.
    Pulled Pull(uint8_t* out);

    // Drops the oldest packet (overflow policy); the next Pull reports a Resync. Returns false when empty.
    bool DropOldest();
    void Reset();

    size_t PacketCount() const { return order_.size(); }
    uint64_t BufferedSamples() const { return bufferedSamples_; }
    const Counters& counters() const { return counters_; }
    // Next timestamp the decoder expects (extended, 64-bit); only meaningful once a packet was inserted.
    int64_t NextTimestamp() const { return nextTs_; }

private:
    struct Slot {
        int64_t ts = 0;
        uint32_t samples = 0;
        uint32_t size = 0;
    };
    int64_t Unwrap(uint32_t ts) const;
    void Release(uint16_t slot);
    uint8_t* Data(uint16_t slot) { return pool_.data() + size_t(slot) * cfg_.maxPacketBytes; }

    Config cfg_;
    std::vector<uint8_t> pool_;
    std::vector<Slot> slots_;
    std::vector<uint16_t> freeList_;
    std::vector<uint16_t> order_;      // slot indices sorted by ts
    bool started_ = false, consumed_ = false, resyncPending_ = false;
    int64_t nextTs_ = 0;
    uint32_t lastSamples_ = 0;
    uint64_t bufferedSamples_ = 0;
    Counters counters_;
};

} // namespace fluvi::audio
