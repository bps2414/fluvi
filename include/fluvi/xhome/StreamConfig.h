// SPDX-License-Identifier: GPL-3.0-only
// Fluvi xhome: what the client asks the console for. Owner: agent B (src/net/xhome, include/fluvi/xhome).
//
// The rest of the xhome public API is the Veyra port (third_party/veyra-import/include/veyra/xbox) moved to
// namespace fluvi::xhome with these fixed points, which later waves code against:
//   Https.h         HttpsTransport (WinHTTP) + ServiceError/NetworkError
//   Account.h       Account: beginSignIn / pollSignIn / signedIn / signOut / streamingAccess / transferToken
//                   tokens DPAPI-protected under %LOCALAPPDATA%\Fluvi\xhome (FLUVI_XHOME_DATA overrides, tests)
//   StreamApi.h     StreamApi: consoles / play / state / connect / exchangeSdp / exchangeIce / keepalive / stop,
//                   plus wake(serverId) (power on from ConnectedStandby)
//   WebRtcSession.h WebRtcSession: start(Signaling&, StreamConfig) / waitReady / sendGamepad(const GamepadState&)
//                   / requestKeyframe / videoPresented / close / stats; callbacks video (Annex-B AU, RTP ts,
//                   arrival QPC), audio (Opus packet, RTP ts), rumble (fluvi::Rumble), serverVideoSize, ended
//   Protocol.h      pure encoders/decoders of the binary input channel, on fluvi::GamepadState / fluvi::Rumble
// Times that cross into other modules are fluvi::time::NowQpc() values (not Veyra's 100 ns ticks).
#pragma once
#include <cstdint>
#include <string>

namespace fluvi::xhome {

struct StreamConfig {
    // Requested stream size. 1440p needs the console output at 1440p or 4K and the three changes of
    // Greenlight PR #1752 (VideoPreference, userRequestedResolutionUpdate, device-info 2560x1440).
    uint32_t width = 2560, height = 1440;
    // Video bitrate written as b=AS (kbps) in the offer, as Greenlight's sdp.ts does. 0 = the highest
    // value the console is known to accept (agent B documents the number it picked and why).
    uint32_t maxBitrateKbps = 0;
    std::string locale = "pt-BR";
};

} // namespace fluvi::xhome
