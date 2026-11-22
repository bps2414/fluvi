// SPDX-License-Identifier: GPL-3.0-only
// Test helper: a real Opus stream (stereo sine, 20 ms packets) encoded with libopus.
#pragma once
#include <opus/opus.h>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fluvi::test {

inline constexpr uint32_t kFrame = 960;   // 20 ms at 48 kHz

struct OpusStream {
    std::vector<std::vector<uint8_t>> packets;
    std::vector<float> pcm;               // the source signal, interleaved stereo
};

inline OpusStream EncodeSine(int packetCount, bool fec = false, double freq = 440.0, float amp = 0.5f) {
    OpusStream s;
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &err);
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(96000));
    if (fec) {
        opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(25));
        opus_encoder_ctl(enc, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND));
        opus_encoder_ctl(enc, OPUS_SET_FORCE_MODE(MODE_SILK_ONLY));
    }
    const double w = 2.0 * 3.14159265358979323846 * freq / 48000.0;
    s.pcm.resize(size_t(packetCount) * kFrame * 2);
    for (size_t n = 0; n < size_t(packetCount) * kFrame; ++n) {
        s.pcm[n * 2] = amp * static_cast<float>(std::sin(w * double(n)));
        s.pcm[n * 2 + 1] = amp * static_cast<float>(std::sin(w * double(n) + 1.0));
    }
    for (int i = 0; i < packetCount; ++i) {
        std::vector<uint8_t> pkt(1500);
        const int n = opus_encode_float(enc, s.pcm.data() + size_t(i) * kFrame * 2, int(kFrame), pkt.data(), int(pkt.size()));
        pkt.resize(n > 0 ? size_t(n) : 0);
        s.packets.push_back(std::move(pkt));
    }
    opus_encoder_destroy(enc);
    return s;
}

// Decodes every packet (nullptr entries are concealed) with a fresh decoder, like a perfect receiver would.
inline std::vector<float> ReferenceDecode(const OpusStream& s) {
    int err = 0;
    OpusDecoder* dec = opus_decoder_create(48000, 2, &err);
    std::vector<float> out;
    std::vector<float> frame(kFrame * 2);
    for (const auto& p : s.packets) {
        const int n = opus_decode_float(dec, p.data(), int(p.size()), frame.data(), int(kFrame), 0);
        out.insert(out.end(), frame.begin(), frame.begin() + n * 2);
    }
    opus_decoder_destroy(dec);
    return out;
}

} // namespace fluvi::test
