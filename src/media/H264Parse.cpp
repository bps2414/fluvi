// SPDX-License-Identifier: GPL-3.0-only
#include "H264Parse.h"

#include <algorithm>

namespace fluvi::media::h264 {
namespace {

size_t FindStart(const uint8_t* d, size_t n, size_t from) {
    for (size_t k = from; k + 2 < n; ++k)
        if (d[k] == 0 && d[k + 1] == 0 && d[k + 2] == 1) return k;
    return n;
}

// Calls f(nal, size) for every NAL unit (nal points at the NAL header byte).
template <class F>
void ForEachNal(const uint8_t* d, size_t n, F&& f) {
    size_t sc = FindStart(d, n, 0);
    while (sc < n) {
        const size_t begin = sc + 3;
        const size_t next = FindStart(d, n, begin);
        size_t end = next;
        while (end > begin && d[end - 1] == 0) --end;   // the leading zero of a 4-byte start code
        if (end > begin) f(d + begin, end - begin);
        sc = next;
    }
}

// The first `limit` RBSP bytes after the NAL header (emulation prevention removed).
std::vector<uint8_t> Rbsp(const uint8_t* nal, size_t size, size_t limit) {
    std::vector<uint8_t> out;
    out.reserve(std::min(size, limit));
    for (size_t k = 1; k < size && out.size() < limit; ++k) {
        if (nal[k] == 3 && k >= 3 && nal[k - 1] == 0 && nal[k - 2] == 0) continue;
        out.push_back(nal[k]);
    }
    return out;
}

} // namespace

AuInfo Parser::Parse(const uint8_t* au, size_t size) {
    AuInfo info;
    ForEachNal(au, size, [&](const uint8_t* nal, size_t n) {
        const uint8_t type = nal[0] & 0x1F;
        if (type == 7) {
            info.hasSps = true;
            // parseH264Sps scans for a start code: hand it this NAL with one.
            std::vector<uint8_t> withCode(n + 3);
            withCode[2] = 1;
            std::copy(nal, nal + n, withCode.begin() + 3);
            xhome::H264SpsInfo sps;
            if (xhome::parseH264Sps(withCode.data(), withCode.size(), &sps)) {
                auto it = sps_.find(sps.spsId);
                if (it != sps_.end() && (it->second.width != sps.width || it->second.height != sps.height)) info.spsChanged = true;
                if (it == sps_.end() && !sps_.empty()) info.spsChanged = true;
                sps_[sps.spsId] = sps;
                active_ = nullptr;   // re-resolved by the next slice (map nodes are stable, but the content changed)
            }
        } else if (type == 8) {
            const auto rbsp = Rbsp(nal, n, 16);
            xhome::detail::BitReader r(rbsp.data(), rbsp.size());
            const uint32_t ppsId = r.ue(), spsId = r.ue();
            if (!r.bad()) ppsToSps_[ppsId] = spsId;
        } else if (type == 1 || type == 5) {
            info.sliceBytes += uint32_t(n);
            ++info.sliceCount;
            if (type == 5) info.hasIdr = true;
            if (info.hasSlice) return;
            info.hasSlice = true;
            info.reference = (nal[0] & 0x60) != 0;
            const auto rbsp = Rbsp(nal, n, 32);
            xhome::detail::BitReader r(rbsp.data(), rbsp.size());
            info.firstMb = r.ue();
            const uint32_t sliceType = r.ue();
            const uint32_t ppsId = r.ue();
            if (r.bad()) return;
            info.sliceType = int(sliceType % 5);
            const auto pps = ppsToSps_.find(ppsId);
            if (pps == ppsToSps_.end()) return;
            const auto sps = sps_.find(pps->second);
            if (sps == sps_.end()) return;
            active_ = &sps->second;
            if (sps->second.separateColourPlane) r.bits(2);
            info.frameNum = r.bits(unsigned(sps->second.log2MaxFrameNum));
            info.headerOk = !r.bad();
        }
    });
    return info;
}

bool FrameNumTracker::Observe(const AuInfo& au, uint32_t maxFrameNum, bool gapsAllowed) {
    if (!au.headerOk) return false;
    bool gap = false;
    if (au.hasIdr) {
        have_ = true;
        prevRef_ = 0;   // an IDR has frame_num 0 and starts the chain
        return false;
    }
    if (have_ && !gapsAllowed && maxFrameNum) {
        const uint32_t next = (prevRef_ + 1) % maxFrameNum;
        gap = au.frameNum != prevRef_ && au.frameNum != next;
    }
    if (au.reference) { prevRef_ = au.frameNum; have_ = true; }
    return gap;
}

std::vector<std::vector<uint8_t>> SplitAccessUnits(const uint8_t* data, size_t size) {
    std::vector<std::vector<uint8_t>> units;
    std::vector<uint8_t> current;
    bool currentHasSlice = false;
    ForEachNal(data, size, [&](const uint8_t* nal, size_t n) {
        const uint8_t type = nal[0] & 0x1F;
        const bool slice = type == 1 || type == 5;
        const bool firstSlice = slice && n > 1 && (nal[1] & 0x80) != 0;   // first_mb_in_slice == 0 (ue "1")
        const bool starts = type == 9 || type == 7 || type == 8 || type == 6 || firstSlice;
        if (currentHasSlice && starts) {
            units.push_back(std::move(current));
            current.clear();
            currentHasSlice = false;
        }
        static constexpr uint8_t kCode[4] = {0, 0, 0, 1};
        current.insert(current.end(), kCode, kCode + 4);
        current.insert(current.end(), nal, nal + n);
        currentHasSlice = currentHasSlice || slice;
    });
    if (!current.empty()) units.push_back(std::move(current));
    return units;
}

} // namespace fluvi::media::h264
