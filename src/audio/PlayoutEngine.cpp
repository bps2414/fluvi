// SPDX-License-Identifier: GPL-3.0-only
#include "PlayoutEngine.h"
#include <opus/opus.h>
#include <algorithm>
#include <cmath>
#include "fluvi/core/Log.h"

namespace fluvi::audio {
namespace {
constexpr int kMaxFrame = 5760;      // 120 ms at 48 kHz, the longest Opus frame set
constexpr size_t kFadeFrames = 64;   // ~1.3 ms: removes the click at a discontinuity
}

std::unique_ptr<PlayoutEngine> PlayoutEngine::Create(const PlayoutConfig& cfg, Status& status) {
    if (cfg.streamRate != 48000 && cfg.streamRate != 24000 && cfg.streamRate != 16000 && cfg.streamRate != 12000 &&
        cfg.streamRate != 8000) {
        status = Status::Fail("unsupported Opus sample rate");
        return nullptr;
    }
    std::unique_ptr<PlayoutEngine> e(new PlayoutEngine());
    e->cfg_ = cfg;
    JitterBuffer::Config jc;
    jc.maxPackets = cfg.maxPackets;
    jc.maxGapSamples = cfg.streamRate / 5;
    jc.defaultFrameSamples = cfg.streamRate / 50;
    jc.frameGranularity = cfg.streamRate / 400;
    e->jitter_ = JitterBuffer(jc);
    DriftConfig dc;
    dc.targetMs = cfg.targetBufferMs;
    e->drift_ = DriftController(dc);
    int err = 0;
    e->decoder_ = opus_decoder_create(static_cast<opus_int32>(cfg.streamRate), 2, &err);
    if (!e->decoder_ || err != OPUS_OK) {
        status = Status::Fail(std::string("opus_decoder_create failed: ") + opus_strerror(err));
        return nullptr;
    }
    e->packetBuf_.resize(jc.maxPacketBytes);
    e->pcm_.resize(size_t(kMaxFrame) * 2);
    status = Status::Ok();
    return e;
}

PlayoutEngine::~PlayoutEngine() {
    if (decoder_) opus_decoder_destroy(decoder_);
}

void PlayoutEngine::Push(const uint8_t* data, size_t size, uint32_t rtpTimestamp) {
    if (!data || size == 0) return;
    const int samples = opus_packet_get_nb_samples(data, static_cast<opus_int32>(size), static_cast<opus_int32>(cfg_.streamRate));
    if (samples <= 0) return;
    JitterBuffer::InsertResult r;
    {
        std::lock_guard lock(mutex_);
        r = jitter_.Insert(rtpTimestamp, data, size, static_cast<uint32_t>(samples));
    }
    if (r == JitterBuffer::InsertResult::Accepted || r == JitterBuffer::InsertResult::AcceptedDroppedOldest) ++packets_;
    if (r == JitterBuffer::InsertResult::AcceptedDroppedOldest) ++overflow_;
}

void PlayoutEngine::ApplyFlush() {
    {
        std::lock_guard lock(mutex_);
        jitter_.Reset();
    }
    resampler_.Reset();
    drift_.Reset();
    opus_decoder_ctl(decoder_, OPUS_RESET_STATE);
    priming_ = true;
    fadeInFrames_ = 0;
}

void PlayoutEngine::Discontinuity() {
    resampler_.FadeTail(kFadeFrames);
    fadeInFrames_ = kFadeFrames;
}

bool PlayoutEngine::DecodeOne() {
    JitterBuffer::Pulled p;
    {
        std::lock_guard lock(mutex_);
        p = jitter_.Pull(packetBuf_.data());
    }
    int n = 0;
    switch (p.kind) {
        case JitterBuffer::PullKind::Empty:
            return false;
        case JitterBuffer::PullKind::Packet:
            n = opus_decode_float(decoder_, packetBuf_.data(), static_cast<opus_int32>(p.size), pcm_.data(), kMaxFrame, 0);
            if (n <= 0) {   // corrupt packet: conceal its slot instead
                n = opus_decode_float(decoder_, nullptr, 0, pcm_.data(), static_cast<int>(p.samples), 0);
                ++lost_;
                if (n > 0) concealed_ += static_cast<uint64_t>(n);
            }
            break;
        case JitterBuffer::PullKind::Lost:
            lost_ += p.lostPackets;
            if (p.hasFec)
                n = opus_decode_float(decoder_, packetBuf_.data(), static_cast<opus_int32>(p.size), pcm_.data(),
                                      static_cast<int>(p.samples), 1);
            else
                n = opus_decode_float(decoder_, nullptr, 0, pcm_.data(), static_cast<int>(p.samples), 0);
            if (n > 0) concealed_ += static_cast<uint64_t>(n);
            break;
        case JitterBuffer::PullKind::Resync:
            lost_ += p.lostPackets;
            opus_decoder_ctl(decoder_, OPUS_RESET_STATE);   // the stream is no longer contiguous
            if (fadeInFrames_ == 0) fadeInFrames_ = kFadeFrames;
            return true;
    }
    if (n <= 0) return true;
    if (fadeInFrames_ > 0) {
        const size_t k = std::min<size_t>(fadeInFrames_, static_cast<size_t>(n));
        for (size_t i = 0; i < k; ++i) {
            const float g = static_cast<float>(kFadeFrames - fadeInFrames_ + i) / static_cast<float>(kFadeFrames);
            pcm_[i * 2] *= g;
            pcm_[i * 2 + 1] *= g;
        }
        fadeInFrames_ -= k;
    }
    resampler_.Feed(pcm_.data(), static_cast<size_t>(n));
    return true;
}

void PlayoutEngine::Render(float* out, size_t frames, uint32_t outRate) {
    if (flush_.exchange(false)) ApplyFlush();
    if (frames == 0) return;
    outRate_ = outRate;
    const double rateKhz = cfg_.streamRate / 1000.0;
    auto fillMs = [&] {
        uint64_t jb;
        {
            std::lock_guard lock(mutex_);
            jb = jitter_.BufferedSamples();
        }
        return (double(jb) + double(resampler_.PendingInput())) / rateKhz;
    };

    if (priming_) {
        const double have = fillMs();
        if (have < cfg_.targetBufferMs) {
            std::fill(out, out + frames * 2, 0.0f);
            bufferedMs_ = have;
            return;
        }
        priming_ = false;
        fadeInFrames_ = kFadeFrames;
    }

    // Drift correction: small ratio change keeps the fill around the target.
    double fill = fillMs();
    double ppm = cfg_.driftCorrection ? drift_.Update(fill, double(frames) / outRate) : 0.0;
    resampler_.SetRatio(double(cfg_.streamRate) / double(outRate) * (1.0 + ppm * 1e-6));

    // Overflow: far above the target (a burst after a stall): drop the oldest packets, fade around the cut.
    const double highMs = cfg_.targetBufferMs + std::max(100.0, double(cfg_.targetBufferMs));
    if (fill > highMs) {
        bool dropped = false;
        std::lock_guard lock(mutex_);
        while ((double(jitter_.BufferedSamples()) + double(resampler_.PendingInput())) / rateKhz > cfg_.targetBufferMs + 20.0 &&
               jitter_.DropOldest()) {
            ++overflow_;
            dropped = true;
        }
        if (dropped) Discontinuity();
    }

    size_t produced = 0;
    while (produced < frames) {
        const size_t avail = resampler_.OutputsAvailable();
        if (avail == 0) {
            if (!DecodeOne()) break;
            continue;
        }
        const size_t got = resampler_.Read(out + produced * 2, std::min(avail, frames - produced));
        if (got == 0) break;
        produced += got;
    }

    if (produced < frames) {
        // Underrun: fade the audible end, silence for the rest, refill to the target before resuming.
        const size_t k = std::min(produced, kFadeFrames);
        for (size_t d = 0; d < k; ++d) {
            const float g = static_cast<float>(d) / static_cast<float>(k);
            out[(produced - 1 - d) * 2] *= g;
            out[(produced - 1 - d) * 2 + 1] *= g;
        }
        std::fill(out + produced * 2, out + frames * 2, 0.0f);
        ++underruns_;
        priming_ = true;
        resampler_.Reset();
    }

    // Volume with a linear ramp across the block (no zipper noise on changes).
    const float g1 = volume_.load();
    const float g0 = gain_;
    if (g0 == g1) {
        if (g1 != 1.0f)
            for (size_t i = 0; i < frames * 2; ++i) out[i] *= g1;
    } else {
        for (size_t f = 0; f < frames; ++f) {
            const float g = g0 + (g1 - g0) * (static_cast<float>(f + 1) / static_cast<float>(frames));
            out[f * 2] *= g;
            out[f * 2 + 1] *= g;
        }
    }
    gain_ = g1;

    bufferedMs_ = fillMs();
    driftPpm_ = ppm;
}

AudioStats PlayoutEngine::Stats() const {
    AudioStats s;
    s.bufferedMs = bufferedMs_.load();
    s.driftPpm = driftPpm_.load();
    s.packets = packets_.load();
    s.lostPackets = lost_.load();
    s.concealedFrames = concealed_.load();
    s.underruns = underruns_.load();
    s.overflowDrops = overflow_.load();
    return s;
}

} // namespace fluvi::audio
