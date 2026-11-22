// SPDX-License-Identifier: GPL-3.0-only
// Ported from Veyra (GPL-3.0-only), include/veyra/xbox/StreamApi.h @ c6633121. Changes: namespace, StreamConfig-sized play, wake.
// The Xbox home-streaming (xHome) session API on the region host the streaming sign-in returned.
// Ported from Greenlight (unknownskl/greenlight, MIT): xcloudapi.ts and streammanager.ts. The signalling
// goes through Microsoft's servers even when the console is on the same network; the media then flows
// over WebRTC (see WebRtcSession). Failures throw ServiceError / NetworkError / std::runtime_error.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "fluvi/xhome/Https.h"
#include "fluvi/xhome/StreamConfig.h"

namespace fluvi::xhome {

struct Console {
    std::string serverId;       // "F4001234ABCD5678"
    std::string name;
    std::string powerState;     // "On", "ConnectedStandby", "Off" ...
    std::string consoleType;    // "XboxSeriesX", "XboxOne" ...
    bool outOfHomeWarning = false;
};

struct SessionState {
    std::string state;          // Provisioning, ReadyToConnect, Provisioned, WaitingForResources, Failed
    std::string errorCode, errorMessage;
};

struct IceCandidate {
    std::string candidate;      // "a=candidate:..." as the service sends it
    std::string sdpMid;
    int sdpMLineIndex = 0;
};

class StreamApi {
public:
    StreamApi(std::shared_ptr<HttpsTransport> transport, std::string host, std::string gsToken);

    std::vector<Console> consoles();
    // Starts a session on a console; returns the session id. The config's size goes into X-MS-Device-Info
    // (Greenlight PR #1752 does the same); its locale into the session settings.
    std::string play(const std::string& serverId, const StreamConfig& config = {});
    SessionState state(const std::string& sessionId);
    void connect(const std::string& sessionId, const std::string& transferToken);
    // Sends the local offer, returns the console's answer SDP.
    std::string exchangeSdp(const std::string& sessionId, const std::string& offerSdp);
    // Sends local candidates, returns the console's (plus the IPv4 ones hidden in Teredo addresses).
    std::vector<IceCandidate> exchangeIce(const std::string& sessionId, const std::vector<IceCandidate>& local);
    void keepalive(const std::string& sessionId);
    void stop(const std::string& sessionId);

    // Power on a console in ConnectedStandby (or off with Instant-On): the XCCS "Power/WakeUp" remote
    // command the Xbox app and xbox-webapi-python (OpenXbox) use, POST https://xccs.xboxlive.com/commands.
    // It needs the Xbox Live web authorization (Account::webAuthorization), set with setWakeAuthorization;
    // without one it throws. Returns when the service accepted the command (the console then takes
    // seconds to come up: poll consoles() for powerState "On").
    void wake(const std::string& serverId);
    void setWakeAuthorization(std::function<std::string()> provider) { wakeAuthorization_ = std::move(provider); }

    // Exposed for tests.
    static std::vector<Console> parseConsoles(const std::string& body);
    static std::vector<IceCandidate> parseIce(const std::string& exchangeResponse);
    // A Teredo address (2001:0:...) carries the client's public IPv4 and port, obfuscated.
    static bool teredoIpv4(const std::string& ipv6, std::string* ipv4, int* port);

    void setRetryDelayMs(int ms) { retryDelayMs_ = ms; }

private:
    HttpsResponse call(const std::string& method, const std::string& path, const std::string& body,
                       const std::map<std::string, std::string>& extraHeaders = {});
    std::string getExchange(const std::string& path);

    std::shared_ptr<HttpsTransport> http_;
    std::string host_, token_;
    std::function<std::string()> wakeAuthorization_;
    int retryDelayMs_ = 750;
};

} // namespace fluvi::xhome
