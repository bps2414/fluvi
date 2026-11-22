// SPDX-License-Identifier: GPL-3.0-only
// XhomeStreamController: play -> poll state -> connect -> WebRTC start -> keepalive, on one thread per attempt.
// Flow ported from fluvi_xhome_cli `probe` (src/net/xhome/cli/main.cpp); keepalive every 30 s as Veyra does.
#include "StreamController.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "Models.h"
#include "fluvi/core/Log.h"
#include "fluvi/xhome/Sdp.h"

namespace fluvi::app {

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

class ApiSignaling final : public xhome::Signaling {
public:
    ApiSignaling(xhome::StreamApi& api, std::string session) : api_(api), session_(std::move(session)) {}
    std::string exchangeSdp(const std::string& offer) override { return api_.exchangeSdp(session_, offer); }
    std::vector<xhome::IceCandidate> exchangeIce(const std::vector<xhome::IceCandidate>& local) override { return api_.exchangeIce(session_, local); }
private:
    xhome::StreamApi& api_;
    std::string session_;
};

xhome::StreamConfig ConfigFrom(const AppSettings& settings) {
    xhome::StreamConfig cfg;
    cfg.width = StreamWidth(settings.video.resolution);
    cfg.height = StreamHeight(settings.video.resolution);
    cfg.maxBitrateKbps = settings.video.bitrateKbps;
    return cfg;
}

// One start() .. end of stream. Shared between the controller and its thread.
struct Attempt {
    std::atomic<bool> cancelled{false}, done{false};
    std::mutex mutex;                 // guards the fields below and pairs with cv
    std::condition_variable cv;
    std::shared_ptr<xhome::WebRtcSession> rtc;   // set while the session exists (readable by sendGamepad)
    bool ended = false;               // the console / link ended the stream
    std::string endReason;
};

class XhomeStreamController final : public StreamController {
public:
    explicit XhomeStreamController(XhomeSessionDeps deps) : deps_(std::move(deps)) {}
    ~XhomeStreamController() override {
        cancel();
        std::vector<Worker> workers;
        { std::lock_guard lock(mutex_); workers.swap(workers_); }
        for (auto& w : workers) if (w.thread.joinable()) w.thread.join();
    }

    void setEvents(StreamEvents events) override { std::lock_guard lock(mutex_); events_ = std::move(events); }
    void setHooks(StreamHooks hooks) override { std::lock_guard lock(mutex_); hooks_ = std::move(hooks); }

    void start(const std::string& consoleId, const AppSettings& settings) override {
        std::lock_guard lock(mutex_);
        CancelLocked();
        ReapLocked();
        auto attempt = std::make_shared<Attempt>();
        current_ = attempt;
        streaming_ = false;
        workers_.push_back({attempt, std::thread([this, attempt, consoleId, settings, events = events_, hooks = hooks_] {
            Run(attempt, consoleId, settings, events, hooks);
        })});
    }

    void cancel() override {
        std::lock_guard lock(mutex_);
        CancelLocked();
    }

    bool active() const override {
        std::lock_guard lock(mutex_);
        return current_ && !current_->done.load();
    }
    bool streaming() const override { return streaming_.load(); }

    void sendGamepad(const GamepadState& state, uint8_t index) override {
        std::shared_ptr<xhome::WebRtcSession> rtc;
        {
            std::lock_guard lock(mutex_);
            if (!current_ || !streaming_) return;
            std::lock_guard attemptLock(current_->mutex);
            rtc = current_->rtc;
        }
        if (rtc) rtc->sendGamepad(state, index);
    }

private:
    void CancelLocked() {
        if (!current_) return;
        // The thread does the cleanup itself (close, stop). A WebRtcSession::start() in progress cannot be
        // interrupted: the thread notices the flag when it returns (<= 20 s) and tears the session down then.
        current_->cancelled = true;
        { std::lock_guard lock(current_->mutex); }
        current_->cv.notify_all();
        current_.reset();
        streaming_ = false;
    }

    // Joins the threads of attempts that are over (`done` is the thread's last act).
    void ReapLocked() {
        for (auto it = workers_.begin(); it != workers_.end();) {
            if (it->attempt->done.load()) {
                if (it->thread.joinable()) it->thread.join();
                it = workers_.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool Live(const Attempt& a) const { return !a.cancelled.load(); }

    void Run(std::shared_ptr<Attempt> a, std::string consoleId, AppSettings settings, StreamEvents events, StreamHooks hooks) {
        std::shared_ptr<xhome::StreamApi> api;
        std::string sessionId;
        bool hooked = false;
        const auto fail = [&](const std::string& reason) {
            FLUVI_WARN("app", "stream attempt failed: {}", reason);
            if (Live(*a) && events.failed) events.failed(reason);
        };
        const auto wait = [&](std::chrono::milliseconds d) {   // false when cancelled
            std::unique_lock lock(a->mutex);
            a->cv.wait_for(lock, d, [&] { return a->cancelled.load() || a->ended; });
            return !a->cancelled.load();
        };
        const xhome::StreamConfig cfg = ConfigFrom(settings);
        try {
            if (events.stage) events.stage(StreamStage::Provisioning);
            api = deps_.api();
            sessionId = api->play(consoleId, cfg);
            FLUVI_INFO("app", "session {} started ({}x{})", sessionId, cfg.width, cfg.height);

            // Provisioning -> ReadyToConnect -> connect() -> Provisioned.
            bool connectSent = false;
            const auto deadline = Clock::now() + 120s;
            for (;;) {
                if (!Live(*a)) break;
                if (Clock::now() > deadline) { fail("O console demorou demais para ficar pronto. Tente de novo."); goto cleanup; }
                const xhome::SessionState st = api->state(sessionId);
                if (st.state == "Provisioned") break;
                if (st.state == "Failed") { fail(DescribeSessionFailure(st.errorCode, st.errorMessage)); goto cleanup; }
                if (st.state == "ReadyToConnect" && !connectSent) {
                    if (Live(*a) && events.stage) events.stage(StreamStage::ReadyToConnect);
                    api->connect(sessionId, deps_.transferToken());
                    connectSent = true;
                    if (Live(*a) && events.stage) events.stage(StreamStage::Connecting);
                    continue;
                }
                if (!wait(1s)) break;
            }
            if (!Live(*a)) goto cleanup;
            if (!connectSent && events.stage) events.stage(StreamStage::Connecting);
            if (!Live(*a)) goto cleanup;

            {
                auto rtc = std::make_shared<xhome::WebRtcSession>();
                xhome::WebRtcCallbacks cb = hooks.media;   // HOOK(decode): video / audio / rumble / serverVideoSize
                cb.ended = [a, userEnded = hooks.media.ended](const std::string& reason) {
                    if (userEnded) userEnded(reason);
                    { std::lock_guard lock(a->mutex); if (!a->ended) { a->ended = true; a->endReason = reason; } }
                    a->cv.notify_all();
                };
                rtc->setCallbacks(std::move(cb));
                { std::lock_guard lock(a->mutex); a->rtc = rtc; }

                ApiSignaling signaling(*api, sessionId);
                std::string error;
                if (!rtc->start(signaling, cfg, &error)) {
                    if (Live(*a)) fail("Não foi possível abrir a conexão de vídeo: " + error);
                    goto cleanup;
                }
                if (!Live(*a)) goto cleanup;
                if (!rtc->waitReady(10s)) { fail("O console conectou, mas não terminou de abrir a sessão. Tente de novo."); goto cleanup; }
                rtc->requestKeyframe();

                // HOOK(chain): the decoder and the QualityChain are created here by the main session.
                if (hooks.connected) hooks.connected(*rtc, cfg);
                hooked = true;
                streaming_ = Live(*a);
                if (Live(*a) && events.stage) events.stage(StreamStage::Connected);

                // Keepalive every 30 s until cancelled or ended.
                auto next = Clock::now() + 30s;
                for (;;) {
                    if (!wait(1s)) break;
                    { std::lock_guard lock(a->mutex); if (a->ended) break; }
                    if (Clock::now() < next) continue;
                    next = Clock::now() + 30s;
                    try {
                        api->keepalive(sessionId);
                    } catch (const xhome::ServiceError& e) {
                        FLUVI_WARN("app", "keepalive HTTP {}", e.status);
                        if (e.status == 404) { std::lock_guard lock(a->mutex); a->ended = true; a->endReason = "session expired"; }
                    } catch (const std::exception& e) {
                        FLUVI_WARN("app", "keepalive: {}", e.what());
                    }
                }
                std::string endReason;
                bool ended = false;
                { std::lock_guard lock(a->mutex); ended = a->ended; endReason = a->endReason; }
                if (ended && Live(*a)) {
                    streaming_ = false;
                    if (events.ended) events.ended(endReason);
                }
            }
        } catch (const std::exception& e) {
            if (Live(*a)) fail(DescribeError(e));
            else FLUVI_WARN("app", "cancelled attempt: {}", e.what());
        }

    cleanup:
        std::shared_ptr<xhome::WebRtcSession> rtc;
        { std::lock_guard lock(a->mutex); rtc = std::move(a->rtc); a->rtc.reset(); }
        if (hooked && hooks.closing) hooks.closing();
        if (rtc) rtc->close();
        rtc.reset();
        if (api && !sessionId.empty()) {
            try { api->stop(sessionId); FLUVI_INFO("app", "session {} stopped", sessionId); }
            catch (const std::exception& e) { FLUVI_WARN("app", "stop failed: {}", e.what()); }
        }
        a->done = true;
    }

    XhomeSessionDeps deps_;
    mutable std::mutex mutex_;
    StreamEvents events_;
    StreamHooks hooks_;
    std::shared_ptr<Attempt> current_;
    struct Worker {
        std::shared_ptr<Attempt> attempt;
        std::thread thread;
    };
    std::vector<Worker> workers_;
    std::atomic<bool> streaming_{false};
};

} // namespace

std::unique_ptr<StreamController> MakeXhomeStreamController(XhomeSessionDeps deps) {
    return std::make_unique<XhomeStreamController>(std::move(deps));
}

} // namespace fluvi::app
