// SPDX-License-Identifier: GPL-3.0-only
// Ported from Veyra (GPL-3.0-only), src/xbox/WebRtcSession.cpp @ c6633121. Changes: namespace, FLUVI logging, StreamConfig (1440p messages, b=AS, High profile first), GamepadState/Rumble, QPC times.
// Channel set-up, handshake and messages follow Greenlight's player (unknownskl/greenlight
// packages/player/src/client/lib, MIT): channel.ts, control.ts, message.ts, input.ts, sdp.ts; the
// resolution messages follow Greenlight PR #1752 (renderer/lib/streamresolution.ts).
#include "fluvi/xhome/WebRtcSession.h"

#include <windows.h>

#include <rtc/rtc.hpp>

#include <nlohmann/json.hpp>
#include <mutex>

#include "fluvi/core/Log.h"
#include "fluvi/core/Time.h"
#include "fluvi/xhome/Sdp.h"

namespace fluvi::xhome {
namespace {

using json = nlohmann::json;

std::string message(const std::string& target, const json& content) {
    return wrapMessage(target, content.dump(), newUuid4());
}

bool rtcDebug() {
    return GetEnvironmentVariableA("FLUVI_XHOME_RTC_DEBUG", nullptr, 0) > 0;
}

void logOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        // FLUVI_XHOME_RTC_DEBUG=1 logs libdatachannel's ICE/DTLS/SCTP details (for field diagnosis).
        rtc::InitLogger(rtcDebug() ? rtc::LogLevel::Verbose : rtc::LogLevel::Info, [](rtc::LogLevel level, std::string text) {
            if (level <= rtc::LogLevel::Error) FLUVI_WARN("xhome-rtc", "{}", text);
            else FLUVI_INFO("xhome-rtc", "{}", text);
        });
    });
}

} // namespace

WebRtcSession::WebRtcSession() : originQpc_(time::NowQpc()) { logOnce(); }

WebRtcSession::~WebRtcSession() { close(); }

double WebRtcSession::nowMs() const { return time::ElapsedMs(originQpc_, time::NowQpc()); }

std::string WebRtcSession::offerSdp() const { std::lock_guard lock(sdpMutex_); return offerSdp_; }
std::string WebRtcSession::answerSdp() const { std::lock_guard lock(sdpMutex_); return answerSdp_; }

bool WebRtcSession::start(Signaling& signaling, const StreamConfig& config, std::string* error, std::chrono::milliseconds timeout) {
    const auto fail = [&](const std::string& why) {
        if (error) *error = why;
        FLUVI_WARN("xhome", "webrtc start failed: {}", why);
        return false;
    };
    config_ = config;
    rtc::Configuration rtcConfig;
    rtcConfig.disableAutoNegotiation = true;
    rtcConfig.maxMessageSize = 256 * 1024;
    if (!bindAddress_.empty()) rtcConfig.bindAddress = bindAddress_;
    pc_ = std::make_shared<rtc::PeerConnection>(rtcConfig);

    pc_->onStateChange([this, gate = gate_](rtc::PeerConnection::State state) {
        std::lock_guard callbackLock(gate->mutex); if (!gate->open) return;
        FLUVI_INFO("xhome", "peer connection state {}", int(state));
        if (state == rtc::PeerConnection::State::Connected) connected_ = true;
        if (state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Closed ||
            state == rtc::PeerConnection::State::Disconnected) {
            const bool wasConnected = connected_.exchange(false);
            failed_ = true;
            if (wasConnected && !closed_ && callbacks_.ended) callbacks_.ended("connection lost");
        }
        changed_.notify_all();
    });
    pc_->onGatheringStateChange([this, gate = gate_](rtc::PeerConnection::GatheringState) { std::lock_guard lock(gate->mutex); if (gate->open) changed_.notify_all(); });
    pc_->onIceStateChange([](rtc::PeerConnection::IceState state) { FLUVI_INFO("xhome", "ICE state {}", int(state)); });

    // Audio (send-receive, as the reference client; nothing is sent) and video (receive only, H.264).
    // Order is preference: High first (ours; the reference client does not offer it), then the reference
    // client's Main, Constrained Baseline, Baseline. The console answers with what it encodes.
    rtc::Description::Audio audio("0", rtc::Description::Direction::SendRecv);
    audio.addOpusCodec(111, std::string("minptime=10;useinbandfec=1;stereo=1"));
    audio_ = pc_->addTrack(audio);
    rtc::Description::Video video("1", rtc::Description::Direction::RecvOnly);
    if (config_.offerHighProfile)
        video.addH264Codec(100, std::string("level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=640032"));
    video.addH264Codec(102, std::string("level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=4d001f"));
    video.addH264Codec(104, std::string("level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f"));
    video.addH264Codec(106, std::string("level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f"));
    video_ = pc_->addTrack(video);

    auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::LongStartSequence);
    video_->setMediaHandler(depacketizer);
    video_->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
    video_->onFrame([this, gate = gate_](rtc::binary data, rtc::FrameInfo info) {
        std::lock_guard callbackLock(gate->mutex); if (!gate->open) return;
        ++videoUnits_;
        videoBytes_ += data.size();
        if (!callbacks_.video) return;
        std::vector<uint8_t> unit(data.size());
        std::memcpy(unit.data(), data.data(), data.size());
        callbacks_.video(std::move(unit), info.timestamp, time::NowQpc());
    });
    audio_->setMediaHandler(std::make_shared<rtc::OpusRtpDepacketizer>());
    audio_->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
    audio_->onFrame([this, gate = gate_](rtc::binary data, rtc::FrameInfo info) {
        std::lock_guard callbackLock(gate->mutex); if (!gate->open) return;
        ++audioPackets_;
        if (callbacks_.audio) callbacks_.audio(reinterpret_cast<const uint8_t*>(data.data()), data.size(), info.timestamp);
    });

    // The channels, in the reference client's creation order.
    const auto channel = [&](const char* label, const char* protocol) {
        rtc::DataChannelInit init;
        init.protocol = protocol;
        return pc_->createDataChannel(label, init);
    };
    chat_ = channel("chat", "chatV1");
    control_ = channel("control", "controlV1");
    input_ = channel("input", "1.0");
    message_ = channel("message", "messageV1");

    // The handshake goes out once message and control are open (its acknowledgement sends on control). It must
    // not wait for chat or input: the console may only acknowledge those after the handshake, and waiting for all
    // four stalled the session ("conectou, mas não terminou de abrir"). Input's metadata report follows its open.
    const auto maybeHandshake = [this, gate = gate_] {
        std::lock_guard callbackLock(gate->mutex); if (!gate->open) return;
        if (!message_ || !control_ || !message_->isOpen() || !control_->isOpen()) return;
        if (handshakeSent_.exchange(true)) return;
        FLUVI_INFO("xhome", "message and control open; sending handshake");
        sendMessageJson(json{{"type", "Handshake"}, {"version", "messageV1"}, {"id", "be0bfc6d-1e83-4c8a-90ed-fa8601c5a179"}, {"cv", "0"}}.dump());
    };
    // PR #1752 wraps the control channel's start(): as soon as it opens, before the message handshake,
    // VideoPreference and userRequestedResolutionUpdate go out. onOpen() keeps ONE callback per channel (a second
    // registration replaces the first), so the control channel's callback runs this and then maybeHandshake.
    const auto sendResolution = [this, gate = gate_] {
        std::lock_guard callbackLock(gate->mutex); if (!gate->open) return;
        if (resolutionSent_.exchange(true)) return;
        const bool sent = sendControl(videoPreferenceMessage(config_.width, config_.height)) &&
                          sendControl(userRequestedResolutionUpdateMessage(config_.height));
        FLUVI_INFO("xhome", "requested {}x{} on the control channel ({})", config_.width, config_.height, sent ? "sent" : "NOT sent");
    };
    chat_->onOpen([] { FLUVI_INFO("xhome", "channel chat open"); });
    message_->onOpen([maybeHandshake] {
        FLUVI_INFO("xhome", "channel message open");
        maybeHandshake();
    });
    input_->onOpen([this, gate = gate_] {
        FLUVI_INFO("xhome", "channel input open");
        std::lock_guard callbackLock(gate->mutex); if (!gate->open) return;
        if (handshakeAcked_) sendInputReport();
    });
    control_->onOpen([sendResolution, maybeHandshake] {
        FLUVI_INFO("xhome", "channel control open");
        sendResolution();
        maybeHandshake();
    });
    message_->onMessage([this, gate = gate_](rtc::message_variant data) {
        std::lock_guard callbackLock(gate->mutex); if (!gate->open) return;
        if (const auto* text = std::get_if<std::string>(&data)) onMessageChannel(*text);
        else if (const auto* bin = std::get_if<rtc::binary>(&data))
            onMessageChannel(std::string(reinterpret_cast<const char*>(bin->data()), bin->size()));
    });
    input_->onMessage([this, gate = gate_](rtc::message_variant data) {
        std::lock_guard callbackLock(gate->mutex); if (!gate->open) return;
        if (const auto* bin = std::get_if<rtc::binary>(&data)) onInputChannel(reinterpret_cast<const uint8_t*>(bin->data()), bin->size());
    });
    control_->onMessage([](rtc::message_variant data) {
        if (const auto* text = std::get_if<std::string>(&data)) FLUVI_INFO("xhome-control", "{}", *text);
    });

    // Offer: gather every local candidate first (no trickle on this service).
    pc_->setLocalDescription(rtc::Description::Type::Offer);
    {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, std::chrono::seconds(5), [&] { return pc_->gatheringState() == rtc::PeerConnection::GatheringState::Complete; });
    }
    const auto local = pc_->localDescription();
    if (!local) return fail("no local description");
    // Greenlight sdp.ts: the video bitrate ceiling as b=AS in the offer.
    const uint32_t bitrate = effectiveBitrateKbps(config_);
    const std::string offer = setMediaBitrate(withoutCandidates(std::string(*local)), "video", bitrate);
    {
        std::lock_guard lock(sdpMutex_);
        offerSdp_ = offer;
    }
    std::vector<IceCandidate> candidates;
    for (const auto& c : local->candidates()) {
        std::string text = c.candidate();
        if (text.rfind("a=", 0) == 0) text = text.substr(2);
        candidates.push_back({text, c.mid(), c.mid() == "0" ? 0 : (c.mid() == "1" ? 1 : 2)});
        if (rtcDebug()) FLUVI_INFO("xhome", "local candidate [{}] mid {}", text, c.mid());
    }
    FLUVI_INFO("xhome", "offer {}x{}, video b=AS:{} kbps, codecs:\n{}", config_.width, config_.height, bitrate, describeCodecs(offer));
    // The whole offer (no candidates, no account data) for field comparison with a client that connects.
    FLUVI_DEBUG("xhome", "offer sdp:\n{}", offer);

    std::string answer;
    try {
        answer = signaling.exchangeSdp(offer);
    } catch (const std::exception& e) {
        return fail(std::string("SDP exchange: ") + e.what());
    }
    {
        std::lock_guard lock(sdpMutex_);
        answerSdp_ = answer;
    }
    FLUVI_INFO("xhome", "answer codecs:\n{}", describeCodecs(answer));
    FLUVI_DEBUG("xhome", "answer sdp:\n{}", answer);
    try {
        pc_->setRemoteDescription(rtc::Description(answer, rtc::Description::Type::Answer));
    } catch (const std::exception& e) {
        return fail(std::string("the console's answer was not accepted: ") + e.what());
    }
    std::vector<IceCandidate> remote;
    try {
        remote = signaling.exchangeIce(candidates);
    } catch (const std::exception& e) {
        return fail(std::string("ICE exchange: ") + e.what());
    }
    int added = 0;
    for (const auto& c : remote) {
        std::string text = c.candidate;
        if (text.rfind("a=", 0) == 0) text = text.substr(2);
        while (!text.empty() && text.back() == ' ') text.pop_back();
        if (rtcDebug()) FLUVI_INFO("xhome", "remote candidate [{}] mid {}", text, c.sdpMid);
        try {
            pc_->addRemoteCandidate(rtc::Candidate(text, c.sdpMid.empty() ? "0" : c.sdpMid));
            ++added;
        } catch (const std::exception& e) {
            FLUVI_WARN("xhome", "remote candidate ignored: {} [{}]", e.what(), text);
        }
    }
    FLUVI_INFO("xhome", "local candidates {}, remote {}", candidates.size(), added);
    if (added == 0) return fail("the console sent no usable network candidates");

    std::unique_lock lock(mutex_);
    if (!changed_.wait_for(lock, timeout, [&] { return connected_.load() || failed_.load(); }) || !connected_)
        return fail(failed_ ? "the connection to the console failed (network or firewall)" : "timed out connecting to the console");
    return true;
}

bool WebRtcSession::waitReady(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return handshakeDone_.load() || failed_.load(); }) && handshakeDone_;
}

void WebRtcSession::sendFailure(const std::exception& error) {
    if (!sendErrorLogged_) { sendErrorLogged_ = true; FLUVI_WARN("xhome", "transport send stopped: {}", error.what()); }
}

bool WebRtcSession::sendBinary(const std::shared_ptr<rtc::DataChannel>& channel, const uint8_t* data, size_t size) {
    if (closed_ || !channel || !channel->isOpen()) return false;
    // isOpen() is only a snapshot. The transport may close before send().
    try { channel->send(reinterpret_cast<const std::byte*>(data), size); return true; }
    catch (const std::exception& error) { sendFailure(error); return false; }
}

bool WebRtcSession::sendMessageJson(const std::string& text) {
    return sendBinary(message_, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

bool WebRtcSession::sendControl(const std::string& text) {
    return sendBinary(control_, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

void WebRtcSession::sendInputReport() {
    if (inputReportSent_.exchange(true)) return;
    const auto report = clientMetadataReport(0, nowMs(), 1);
    if (!sendBinary(input_, report.data(), report.size())) inputReportSent_ = false;
}

bool WebRtcSession::sendClientConfig() {
    bool sent = true;
    sent &= sendMessageJson(message("/streaming/systemUi/configuration", {{"version", {0, 2, 0}}, {"systemUis", json::array()}}));
    sent &= sendMessageJson(message("/streaming/properties/clientappinstallidchanged", {{"clientAppInstallId", "c97d7ee0-73b2-4239-bf1d-9d805a338429"}}));
    sent &= sendMessageJson(message("/streaming/characteristics/orientationchanged", {{"orientation", 0}}));
    sent &= sendMessageJson(message("/streaming/characteristics/touchinputenabledchanged", {{"touchInputEnabled", false}}));
    sent &= sendMessageJson(message("/streaming/characteristics/clientdevicecapabilities", json::object()));
    // PR #1752: dimensionschanged carries the requested size, not a fixed 1920x1080.
    sent &= sendMessageJson(wrapMessage("/streaming/characteristics/dimensionschanged",
                                        dimensionsContent(config_.width, config_.height), newUuid4()));
    return sent;
}

void WebRtcSession::onMessageChannel(const std::string& text) {
    json j;
    try { j = json::parse(text); } catch (const json::exception&) { FLUVI_WARN("xhome", "message channel: not JSON"); return; }
    try {
    const std::string type = j.value("type", "");
    if (type == "HandshakeAck") {
        FLUVI_INFO("xhome", "handshake acknowledged");
        // Control: authorise, then announce pad 0 (the reference client adds, removes and re-adds it).
        bool initialized = sendControl(json{{"message", "authorizationRequest"}, {"accessKey", "4BDB3609-C1F1-4195-9B37-FEFF45DA8B8E"}}.dump());
        initialized &= sendControl(json{{"message", "gamepadChanged"}, {"gamepadIndex", 0}, {"wasAdded", true}}.dump());
        initialized &= sendControl(json{{"message", "gamepadChanged"}, {"gamepadIndex", 0}, {"wasAdded", false}}.dump());
        initialized &= sendControl(json{{"message", "gamepadChanged"}, {"gamepadIndex", 0}, {"wasAdded", true}}.dump());
        handshakeAcked_ = true;
        if (input_ && input_->isOpen()) sendInputReport();   // else input's onOpen sends it
        initialized &= sendClientConfig();
        if (!initialized) FLUVI_WARN("xhome", "handshake acknowledged but a set-up message was not sent");
        handshakeDone_ = initialized;
        changed_.notify_all();
        return;
    }
    if (type == "TransactionStart" || type == "Message") {
        const std::string target = j.value("target", "");
        const std::string id = j.value("id", "");
        FLUVI_INFO("xhome-message", "{} {}", target, j.value("content", std::string()).substr(0, 300));
        if (target == "/streaming/sessionLifetimeManagement/serverInitiatedDisconnect") {
            sendMessageJson(json{{"type", "TransactionComplete"}, {"content", "\"\""}, {"id", id}, {"cv", ""}}.dump());
            const auto content = json::parse(j.value("content", std::string()), nullptr, false);
            const std::string reason = content.is_object() && content.contains("reason") && content["reason"].is_string()
                ? content["reason"].get<std::string>() : "unknown server disconnect";
            if (callbacks_.ended) callbacks_.ended(reason);
        } else if (target == "/streaming/systemUi/messages/ShowMessageDialog") {
            // A system dialog meant for the player; acknowledge it so the console does not wait.
            sendMessageJson(json{{"type", "TransactionComplete"}, {"content", json{{"Result", 0}}.dump()}, {"id", id}, {"cv", ""}}.dump());
        } else if (type == "TransactionStart") {
            sendMessageJson(json{{"type", "TransactionComplete"}, {"content", "\"\""}, {"id", id}, {"cv", ""}}.dump());
        }
        return;
    }
    FLUVI_INFO("xhome-message", "unhandled {}", type);
    } catch (const json::exception&) { FLUVI_WARN("xhome", "message channel: invalid field type"); }
}

void WebRtcSession::onInputChannel(const uint8_t* data, size_t size) {
    if (size < 2) return;
    uint8_t pad = 0;
    if (auto v = parseRumble(data, size, &pad)) {
        // Only pad 0 is announced to the console; Rumble carries no pad index.
        if (pad == 0 && callbacks_.rumble) callbacks_.rumble(*v);
        return;
    }
    uint32_t w = 0, h = 0;
    if (parseServerMetadata(data, size, &w, &h)) {
        FLUVI_INFO("xhome", "server video {}x{}", w, h);
        if (callbacks_.serverVideoSize) callbacks_.serverVideoSize(w, h);
    }
}

void WebRtcSession::sendGamepad(const GamepadState& state, uint8_t index) {
    std::lock_guard lock(gate_->mutex);
    if (!gate_->open || !handshakeDone_) return;
    const auto report = gamepadReport(++inputSequence_, nowMs(), state, index);
    sendBinary(input_, report.data(), report.size());
}

void WebRtcSession::videoPresented(uint32_t rtp, int64_t arrival, int64_t submitted, int64_t decoded, int64_t presented) {
    std::lock_guard lock(gate_->mutex);
    if (!gate_->open || !handshakeDone_ || (feedbackSent_ && rtp == lastFeedbackRtp_)) return;
    const auto ms = [this](int64_t qpc) { const double v = time::ElapsedMs(originQpc_, qpc); return uint32_t(v > 0 ? v : 0); };
    const auto report = videoFeedbackReport(++inputSequence_, nowMs(), {rtp, ms(arrival), ms(submitted), ms(decoded), ms(presented)});
    if (sendBinary(input_, report.data(), report.size())) {
        if (!feedbackSent_) FLUVI_INFO("xhome", "video presentation feedback started");
        feedbackSent_ = true; lastFeedbackRtp_ = rtp;
    }
}

void WebRtcSession::requestKeyframe() {
    std::lock_guard lock(gate_->mutex);
    if (!gate_->open || closed_) return;
    ++keyframeRequests_;
    try { if (video_) video_->requestKeyframe(); } catch (const std::exception& error) { sendFailure(error); }
    if (handshakeDone_) {
        const std::string s = json{{"message", "videoKeyframeRequested"}, {"ifrRequested", true}}.dump();
        sendControl(s);
    }
}

void WebRtcSession::close() {
    if (closed_.exchange(true)) return;
    { std::lock_guard lock(gate_->mutex); gate_->open = false; handshakeDone_ = false; connected_ = false; }
    // Wait for current callbacks via the gate, then detach callbacks before
    // releasing channels. Do not hold the gate while library callbacks reset.
    for (const auto& dc : {chat_, control_, input_, message_}) if (dc) dc->resetCallbacks();
    if (audio_) audio_->resetCallbacks();
    if (video_) video_->resetCallbacks();
    if (pc_) pc_->resetCallbacks();
    if (pc_) {
        try { pc_->close(); } catch (...) {}
    }
    changed_.notify_all();
    chat_.reset(); control_.reset(); input_.reset(); message_.reset();
    audio_.reset(); video_.reset();
    pc_.reset();
}

WebRtcStats WebRtcSession::stats() const {
    std::lock_guard lock(gate_->mutex);
    WebRtcStats s;
    s.videoUnits = videoUnits_.load();
    s.audioPackets = audioPackets_.load();
    s.keyframeRequests = keyframeRequests_.load();
    s.videoBytes = videoBytes_.load();
    s.handshakeDone = handshakeDone_.load();
    if (const auto pc = gate_->open ? pc_ : nullptr) {
        s.bytesReceived = pc->bytesReceived();
        if (const auto rtt = pc->rtt()) s.rttMs = double(rtt->count());
    }
    return s;
}

void shutdownWebRtc() { rtc::Cleanup(); }

} // namespace fluvi::xhome
