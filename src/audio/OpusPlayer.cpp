// SPDX-License-Identifier: GPL-3.0-only
#include "fluvi/audio/OpusPlayer.h"
#include "PlayoutEngine.h"
#include "WasapiRenderer.h"
#include "fluvi/core/Log.h"

namespace fluvi::audio {
namespace {

class OpusPlayerImpl final : public OpusPlayer {
public:
    ~OpusPlayerImpl() override { renderer_.reset(); }   // stop the render thread before the engine goes

    Status Init(const OpusPlayerConfig& cfg) {
        PlayoutConfig pc;
        pc.streamRate = cfg.sampleRate;
        pc.targetBufferMs = cfg.targetBufferMs;
        Status st;
        engine_ = PlayoutEngine::Create(pc, st);
        if (!engine_) return st;
        engine_->SetVolume(cfg.volume);
        PlayoutEngine* engine = engine_.get();
        renderer_ = std::make_unique<WasapiRenderer>(cfg.deviceId, [engine](float* out, size_t frames, uint32_t rate) {
            engine->Render(out, frames, rate);
        });
        // The device may be missing right now (the renderer keeps retrying), so a failed first open is not fatal.
        const Status open = renderer_->Start();
        if (!open) FLUVI_WARN("audio", "audio output not open yet: {}", open.message);
        return Status::Ok();
    }

    void Push(const uint8_t* data, size_t size, uint32_t rtpTimestamp) override { engine_->Push(data, size, rtpTimestamp); }
    void SetVolume(float volume) override { engine_->SetVolume(volume); }
    void Flush() override { engine_->Flush(); }
    AudioStats Stats() const override {
        AudioStats s = engine_->Stats();
        s.deviceLatencyMs = renderer_->LatencyMs();
        return s;
    }

private:
    std::unique_ptr<PlayoutEngine> engine_;
    std::unique_ptr<WasapiRenderer> renderer_;
};

} // namespace

std::unique_ptr<OpusPlayer> OpusPlayer::Create(const OpusPlayerConfig& config, Status& status) {
    if (config.channels != 2) {
        status = Status::Fail("only stereo Opus streams are supported");
        return nullptr;
    }
    auto p = std::make_unique<OpusPlayerImpl>();
    status = p->Init(config);
    if (!status) return nullptr;
    return p;
}

} // namespace fluvi::audio
