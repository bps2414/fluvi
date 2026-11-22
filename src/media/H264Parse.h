// SPDX-License-Identifier: GPL-3.0-only
// Fluvi media: the little H.264 Annex-B reading the decoder needs before decoding (no entropy decoding).
// Per access unit: IDR/SPS presence, first slice type, first_mb_in_slice, frame_num and the slice payload
// size; frame_num continuity tells a lost reference picture. Plus an Annex-B file splitter for the probe
// tool and the tests.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "fluvi/xhome/H264Sps.h"

namespace fluvi::media::h264 {

struct AuInfo {
    bool hasSlice = false;
    bool hasIdr = false;           // NAL type 5
    bool hasSps = false;           // NAL type 7 (a point where decoding can start, as Veyra's startsDecoding)
    bool reference = false;        // nal_ref_idc != 0 on the first slice
    bool headerOk = false;         // the first slice header parsed (needs a known SPS/PPS)
    int sliceType = -1;            // first slice, 0 P, 1 B, 2 I, 3 SP, 4 SI (slice_type % 5)
    uint32_t firstMb = 0;          // first_mb_in_slice of the first slice (0 unless the head of the AU was lost)
    uint32_t frameNum = 0;
    uint32_t sliceBytes = 0;       // bytes of slice NAL units (no SPS/PPS/SEI/AUD)
    uint32_t sliceCount = 0;
    bool spsChanged = false;       // an SPS in this AU differs in size from the previous one
};

class Parser {
public:
    AuInfo Parse(const uint8_t* au, size_t size);
    // The SPS of the last parsed slice (nullptr before any SPS).
    const xhome::H264SpsInfo* Sps() const { return active_; }
    void Reset() { sps_.clear(); ppsToSps_.clear(); active_ = nullptr; }

private:
    std::map<uint32_t, xhome::H264SpsInfo> sps_;
    std::map<uint32_t, uint32_t> ppsToSps_;
    const xhome::H264SpsInfo* active_ = nullptr;
};

// frame_num continuity (H.264 8.2.5.2): a reference picture is missing when frame_num is neither the
// previous reference frame_num nor the next one. Ignores MMCO 5 (no known console encoder uses it).
class FrameNumTracker {
public:
    // Returns true on a gap. `maxFrameNum` = 1 << log2MaxFrameNum.
    bool Observe(const AuInfo& au, uint32_t maxFrameNum, bool gapsAllowed);
    void Reset() { have_ = false; }

private:
    bool have_ = false;
    uint32_t prevRef_ = 0;
};

// Splits an Annex-B elementary stream into access units (AUD, or SPS/PPS/SEI/first slice after a slice).
std::vector<std::vector<uint8_t>> SplitAccessUnits(const uint8_t* data, size_t size);

} // namespace fluvi::media::h264
