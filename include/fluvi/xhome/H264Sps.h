// SPDX-License-Identifier: GPL-3.0-only
// Reads the coded picture size and profile from the first SPS of an Annex-B access unit. The console's
// server-metadata report can claim a size; this says what the encoder actually produced.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fluvi::xhome {

struct H264SpsInfo {
    uint32_t width = 0, height = 0;   // after frame cropping
    uint8_t profileIdc = 0, levelIdc = 0;
    // Additive (agent F, media): what a slice-header reader needs (frame_num width, colour planes).
    uint32_t spsId = 0;
    uint32_t log2MaxFrameNum = 4;            // frame_num is u(log2MaxFrameNum)
    bool frameMbsOnly = true;
    bool separateColourPlane = false;        // slices then carry colour_plane_id u(2)
    bool gapsInFrameNumAllowed = false;
};

namespace detail {
class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : d_(data), n_(size) {}
    uint32_t bit() { if (pos_ >= n_ * 8) { bad_ = true; return 0; } const uint32_t v = (d_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1u; ++pos_; return v; }
    uint32_t bits(unsigned count) { uint32_t v = 0; while (count--) v = (v << 1) | bit(); return v; }
    uint32_t ue() {
        unsigned zeros = 0;
        while (!bad_ && bit() == 0) { if (++zeros > 31) { bad_ = true; return 0; } }
        return zeros ? ((1u << zeros) - 1u) + bits(zeros) : 0u;
    }
    int32_t se() { const uint32_t k = ue(); return (k & 1u) ? int32_t((k + 1) / 2) : -int32_t(k / 2); }
    bool bad() const { return bad_; }
private:
    const uint8_t* d_;
    size_t n_;
    size_t pos_ = 0;
    bool bad_ = false;
};
} // namespace detail

// `au` is Annex-B (00 00 01 / 00 00 00 01 start codes). False when there is no parsable SPS.
inline bool parseH264Sps(const uint8_t* au, size_t size, H264SpsInfo* out) {
    for (size_t i = 0; i + 4 < size; ++i) {
        if (!(au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) || (au[i + 3] & 0x1F) != 7) continue;
        size_t end = i + 3;
        while (end + 2 < size && !(au[end] == 0 && au[end + 1] == 0 && (au[end + 2] == 1 || au[end + 2] == 0))) ++end;
        if (end + 2 >= size) end = size;
        // RBSP: drop the NAL header byte and the emulation-prevention bytes (00 00 03).
        std::vector<uint8_t> rbsp;
        for (size_t k = i + 4; k < end; ++k) {
            if (k >= i + 6 && au[k] == 3 && au[k - 1] == 0 && au[k - 2] == 0) continue;
            rbsp.push_back(au[k]);
        }
        detail::BitReader r(rbsp.data(), rbsp.size());
        H264SpsInfo info;
        info.profileIdc = uint8_t(r.bits(8));
        r.bits(8);                                   // constraint flags + reserved
        info.levelIdc = uint8_t(r.bits(8));
        info.spsId = r.ue();                         // seq_parameter_set_id
        uint32_t chromaFormat = 1;
        switch (info.profileIdc) {
        case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128: case 138: case 139: case 134: case 135: {
            chromaFormat = r.ue();
            if (chromaFormat == 3) info.separateColourPlane = r.bit() != 0;
            r.ue(); r.ue(); r.bit();                 // bit depths, qpprime_y_zero_transform_bypass
            if (r.bit()) {                           // seq_scaling_matrix_present_flag
                for (unsigned list = 0; list < (chromaFormat != 3 ? 8u : 12u); ++list) {
                    if (!r.bit()) continue;
                    const unsigned count = list < 6 ? 16u : 64u;
                    int32_t last = 8, next = 8;
                    for (unsigned j = 0; j < count; ++j) {
                        if (next != 0) { next = (last + r.se() + 256) % 256; }
                        last = next ? next : last;
                    }
                }
            }
            break;
        }
        default: break;
        }
        info.log2MaxFrameNum = r.ue() + 4;           // log2_max_frame_num_minus4
        const uint32_t pocType = r.ue();
        if (pocType == 0) r.ue();
        else if (pocType == 1) {
            r.bit(); r.se(); r.se();
            const uint32_t cycle = r.ue();
            for (uint32_t j = 0; j < cycle && !r.bad(); ++j) r.se();
        }
        r.ue();                                      // max_num_ref_frames
        info.gapsInFrameNumAllowed = r.bit() != 0;   // gaps_in_frame_num_value_allowed_flag
        const uint32_t widthMbs = r.ue() + 1, heightMapUnits = r.ue() + 1;
        const uint32_t frameMbsOnly = r.bit();
        info.frameMbsOnly = frameMbsOnly != 0;
        if (!frameMbsOnly) r.bit();                  // mb_adaptive_frame_field_flag
        r.bit();                                     // direct_8x8_inference_flag
        uint32_t cropL = 0, cropR = 0, cropT = 0, cropB = 0;
        if (r.bit()) { cropL = r.ue(); cropR = r.ue(); cropT = r.ue(); cropB = r.ue(); }
        if (r.bad()) return false;
        const uint32_t cropUnitX = chromaFormat == 0 || chromaFormat == 3 ? 1u : 2u;
        const uint32_t cropUnitY = (chromaFormat == 1 ? 2u : 1u) * (frameMbsOnly ? 1u : 2u);
        info.width = widthMbs * 16 - cropUnitX * (cropL + cropR);
        info.height = heightMapUnits * 16 * (frameMbsOnly ? 1u : 2u) - cropUnitY * (cropT + cropB);
        *out = info;
        return true;
    }
    return false;
}

} // namespace fluvi::xhome
