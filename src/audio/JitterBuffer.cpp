// SPDX-License-Identifier: GPL-3.0-only
#include "JitterBuffer.h"
#include <algorithm>
#include <cstring>

namespace fluvi::audio {

JitterBuffer::JitterBuffer(const Config& cfg) : cfg_(cfg) {
    pool_.resize(cfg_.maxPackets * cfg_.maxPacketBytes);
    slots_.resize(cfg_.maxPackets);
    freeList_.reserve(cfg_.maxPackets);
    order_.reserve(cfg_.maxPackets);
    Reset();
}

void JitterBuffer::Reset() {
    freeList_.clear();
    for (size_t i = cfg_.maxPackets; i-- > 0;) freeList_.push_back(static_cast<uint16_t>(i));
    order_.clear();
    started_ = consumed_ = resyncPending_ = false;
    nextTs_ = 0;
    lastSamples_ = 0;
    bufferedSamples_ = 0;
    counters_ = {};
}

// 32-bit RTP timestamp -> 64-bit, relative to the playout position (wrap-safe within +-2^31 samples).
int64_t JitterBuffer::Unwrap(uint32_t ts) const {
    if (!started_) return ts;
    return nextTs_ + static_cast<int32_t>(ts - static_cast<uint32_t>(nextTs_));
}

void JitterBuffer::Release(uint16_t slot) {
    bufferedSamples_ -= slots_[slot].samples;
    freeList_.push_back(slot);
}

JitterBuffer::InsertResult JitterBuffer::Insert(uint32_t rtpTimestamp, const uint8_t* data, size_t size,
                                                uint32_t samples) {
    if (!data || size == 0 || size > cfg_.maxPacketBytes || samples == 0) { ++counters_.rejected; return InsertResult::Rejected; }
    const int64_t ts = Unwrap(rtpTimestamp);
    if (!started_) {
        started_ = true;
        nextTs_ = ts;
    } else if (!consumed_ && ts < nextTs_) {
        nextTs_ = ts;                       // reordered at the very start: nothing was played yet
    } else if (ts < nextTs_) {
        ++counters_.late;                   // its slot was already concealed (or played)
        return InsertResult::Late;
    }

    // sorted position; equal ts is a duplicate
    auto it = std::lower_bound(order_.begin(), order_.end(), ts,
                               [&](uint16_t s, int64_t v) { return slots_[s].ts < v; });
    if (it != order_.end() && slots_[*it].ts == ts) { ++counters_.duplicates; return InsertResult::Duplicate; }

    InsertResult result = InsertResult::Accepted;
    if (freeList_.empty()) {
        // Full: the oldest packet goes (overflow). The timeline is no longer contiguous -> resync on next Pull.
        const uint16_t victim = order_.front();
        order_.erase(order_.begin());
        Release(victim);
        ++counters_.droppedOldest;
        resyncPending_ = true;
        result = InsertResult::AcceptedDroppedOldest;
        it = std::lower_bound(order_.begin(), order_.end(), ts,
                              [&](uint16_t s, int64_t v) { return slots_[s].ts < v; });
    }
    const uint16_t slot = freeList_.back();
    freeList_.pop_back();
    slots_[slot] = {ts, samples, static_cast<uint32_t>(size)};
    std::memcpy(Data(slot), data, size);
    order_.insert(it, slot);
    bufferedSamples_ += samples;
    ++counters_.accepted;
    return result;
}

bool JitterBuffer::DropOldest() {
    if (order_.empty()) return false;
    const uint16_t victim = order_.front();
    order_.erase(order_.begin());
    Release(victim);
    ++counters_.droppedOldest;
    resyncPending_ = true;
    return true;
}

JitterBuffer::Pulled JitterBuffer::Pull(uint8_t* out) {
    Pulled p;
    for (;;) {
        if (order_.empty()) return p;
        const uint16_t head = order_.front();
        const Slot& h = slots_[head];

        if (resyncPending_) {
            resyncPending_ = false;
            if (h.ts != nextTs_) {
                nextTs_ = h.ts;
                ++counters_.resyncs;
                p.kind = PullKind::Resync;
                return p;
            }
        }
        if (h.ts < nextTs_) {                       // stale overlap, should not happen; discard
            order_.erase(order_.begin());
            Release(head);
            continue;
        }
        if (h.ts == nextTs_) {
            std::memcpy(out, Data(head), h.size);
            p.kind = PullKind::Packet;
            p.samples = h.samples;
            p.size = h.size;
            nextTs_ = h.ts + h.samples;
            lastSamples_ = h.samples;
            consumed_ = true;
            order_.erase(order_.begin());
            Release(head);
            return p;
        }

        // Hole between the playout position and the oldest packet we have.
        const int64_t gap = h.ts - nextTs_;
        const uint32_t frame = lastSamples_ ? lastSamples_ : cfg_.defaultFrameSamples;
        if (gap > cfg_.maxGapSamples || gap % cfg_.frameGranularity != 0) {
            p.lostPackets = static_cast<uint32_t>((gap + frame - 1) / frame);
            counters_.lost += p.lostPackets;
            nextTs_ = h.ts;
            ++counters_.resyncs;
            p.kind = PullKind::Resync;
            return p;
        }
        const uint32_t step = static_cast<uint32_t>(std::min<int64_t>(gap, frame));
        p.kind = PullKind::Lost;
        p.samples = step;
        p.lostPackets = 1;
        ++counters_.lost;
        nextTs_ += step;
        if (gap == step) {                          // the packet right after the hole is here: its FEC covers it
            std::memcpy(out, Data(head), h.size);
            p.size = h.size;
            p.hasFec = true;
        }
        consumed_ = true;
        return p;
    }
}

} // namespace fluvi::audio
