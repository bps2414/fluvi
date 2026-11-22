// SPDX-License-Identifier: GPL-3.0-only
// Ported from Veyra (GPL-3.0-only), include/veyra/xbox/WebRtcSession.h @ c6633121. Changes: namespace, StreamConfig (size + b=AS), GamepadState/Rumble, QPC arrival times.
// The media side of an Xbox home stream, natively with libdatachannel (no browser): one Opus audio
// track, one H.264 video track (received as Annex-B access units for the shared hardware decoder) and the
// four data channels the console expects (chat, control, input, message). The signalling is an interface,
// so a test can connect two local peers; the real one goes through StreamApi.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "fluvi/common/Gamepad.h"
#include "fluvi/xhome/Protocol.h"
#include "fluvi/xhome/StreamApi.h"
#include "fluvi/xhome/StreamConfig.h"

namespace rtc {
class PeerConnection;
class DataChannel;
class Track;
}

namespace fluvi::xhome {

class Signaling {
public:
    virtual ~Signaling() = default;
    virtual std::string exchangeSdp(const std::string& offerSdp) = 0;
    virtual std::vector<IceCandidate> exchangeIce(const std::vector<IceCandidate>& local) = 0;
};

// Callbacks run on libdatachannel threads; keep them short.
struct WebRtcCallbacks {
    // One access unit, Annex-B, with the RTP timestamp (90 kHz) and the arrival time (fluvi::time::NowQpc()).
    std::function<void(std::vector<uint8_t>&& unit, uint32_t rtpTimestamp, int64_t arrivalQpc)> video;
    // One Opus packet, 48 kHz timestamp.
    std::function<void(const uint8_t* data, size_t size, uint32_t rtpTimestamp)> audio;
    std::function<void(const Rumble&)> rumble;
    std::function<void(uint32_t width, uint32_t height)> serverVideoSize;
    // The console ended the stream (serverInitiatedDisconnect) or the connection dropped.
    std::function<void(const std::string& reason)> ended;
};

struct WebRtcStats {
    uint64_t bytesReceived = 0;      // data channels (libdatachannel's count)
    uint64_t videoBytes = 0;         // access units handed to the decoder
    uint64_t videoUnits = 0, audioPackets = 0, keyframeRequests = 0;
    double rttMs = 0;
    bool handshakeDone = false;
};

class WebRtcSession {
public:
    WebRtcSession();
    ~WebRtcSession();
    WebRtcSession(const WebRtcSession&) = delete;
    WebRtcSession& operator=(const WebRtcSession&) = delete;

    void setCallbacks(WebRtcCallbacks callbacks) { callbacks_ = std::move(callbacks); }
    // Tests: gather candidates on this address only (e.g. "127.0.0.1"). Empty: every interface.
    void setBindAddress(std::string address) { bindAddress_ = std::move(address); }

    // Offer, signalling, ICE; returns once connected, false (with `error`) on failure or timeout.
    // The config sets the requested size (VideoPreference + userRequestedResolutionUpdate on the control
    // channel, dimensionschanged on the message channel) and the b=AS video bitrate in the offer.
    bool start(Signaling& signaling, const StreamConfig& config, std::string* error,
               std::chrono::milliseconds timeout = std::chrono::seconds(20));
    // True once the console acknowledged the message-channel handshake (input is accepted from then on).
    bool ready() const { return handshakeDone_.load(); }
    bool waitReady(std::chrono::milliseconds timeout);

    // Sent when it changes, and at least every 33 ms by the caller.
    void sendGamepad(const GamepadState& state, uint8_t index = 0);
    void requestKeyframe();
    // Feedback of a frame shown, all times fluvi::time::NowQpc() values.
    void videoPresented(uint32_t rtpTimestamp, int64_t arrivalQpc, int64_t submittedQpc, int64_t decodedQpc, int64_t presentedQpc);
    void close();

    WebRtcStats stats() const;

    // The offer SDP sent to the console and the answer received (tokens are not part of either); empty before start().
    std::string offerSdp() const;
    std::string answerSdp() const;

private:
    void onMessageChannel(const std::string& text);
    void onInputChannel(const uint8_t* data, size_t size);
    bool sendMessageJson(const std::string& text);
    bool sendControl(const std::string& text);
    bool sendClientConfig();
    bool sendBinary(const std::shared_ptr<rtc::DataChannel>& channel, const uint8_t* data, size_t size);
    void sendFailure(const std::exception& error);
    double nowMs() const;

    WebRtcCallbacks callbacks_;
    StreamConfig config_;
    // Shared with asynchronous callbacks so a queued callback can check the
    // gate after destruction without ever dereferencing its captured this.
    struct CallbackGate { std::recursive_mutex mutex; bool open = true; };
    std::shared_ptr<CallbackGate> gate_ = std::make_shared<CallbackGate>();
    bool sendErrorLogged_ = false, feedbackSent_ = false;
    uint32_t lastFeedbackRtp_ = 0;
    std::shared_ptr<rtc::PeerConnection> pc_;
    std::shared_ptr<rtc::Track> audio_, video_;
    std::shared_ptr<rtc::DataChannel> chat_, control_, input_, message_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::atomic<bool> connected_{false}, failed_{false}, closed_{false}, handshakeDone_{false}, handshakeSent_{false}, resolutionSent_{false};
    std::atomic<uint32_t> inputSequence_{0};
    std::atomic<uint64_t> videoUnits_{0}, audioPackets_{0}, keyframeRequests_{0}, videoBytes_{0};
    int64_t originQpc_ = 0;
    std::string failure_;
    std::string bindAddress_;
    mutable std::mutex sdpMutex_;
    std::string offerSdp_, answerSdp_;
};

// Releases libdatachannel's global threads; call once at process exit, after every session is closed.
void shutdownWebRtc();

} // namespace fluvi::xhome
