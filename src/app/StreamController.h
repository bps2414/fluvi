// SPDX-License-Identifier: GPL-3.0-only
// Fluvi app: starts and ends a console stream. The interface is what the app (and later the main session) codes
// against; XhomeStreamController implements it now with the xhome signalling + WebRTC start only. Decode and the
// quality chain are NOT here: they plug in through StreamHooks (see HOOK markers).
//
// Threading: start() / cancel() return at once. Everything else runs on the controller's own thread, and the
// StreamEvents callbacks fire there: keep them short and thread-safe. After cancel() (or a newer start()) the old
// attempt's events are dropped.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "fluvi/common/Gamepad.h"
#include "fluvi/common/Settings.h"
#include "fluvi/xhome/StreamApi.h"
#include "fluvi/xhome/WebRtcSession.h"

namespace fluvi::app {

// The stages the Connecting screen shows (the failed state is the `failed` event).
enum class StreamStage : uint8_t { Provisioning, ReadyToConnect, Connecting, Connected };

struct StreamEvents {
    std::function<void(StreamStage)> stage;
    // The attempt ended before the stream was up (or the console refused). `reason` is a sentence for the user.
    std::function<void(const std::string& reason)> failed;
    // The stream was up and ended (the console closed it, the link dropped, the session expired). `reason` is the
    // protocol reason ("connection lost", "KickForServerShutdown", "session expired", ...). See xhome/DisconnectPolicy.h.
    std::function<void(const std::string& reason)> ended;
};

// HOOK(decode) and HOOK(chain): the main session fills these before start(). All of them run on xhome / controller
// threads and must return fast (queue the work).
struct StreamHooks {
    // Forwarded to the WebRtcSession as is: video (Annex-B AU, RTP ts, arrival QPC) goes to src/media's decoder,
    // audio to fluvi_audio, rumble to the pad, serverVideoSize to the chain's Configure(). `ended` is wrapped by the
    // controller (use StreamEvents::ended instead); any value set here is called too.
    xhome::WebRtcCallbacks media;
    // The session is up and the handshake is done (before the Connected stage is reported). Create the decoder and
    // the QualityChain here (QualityChain::Create + ResolveChainConfig(settings.video, ...), then Configure(srcW, srcH,
    // outW, outH) once the stream size is known) and keep the session for requestKeyframe() / videoPresented().
    std::function<void(xhome::WebRtcSession& session, const xhome::StreamConfig& config)> connected;
    // The session is about to be closed (cancel, end or failure): stop feeding the decoder.
    std::function<void()> closing;
};

class StreamController {
public:
    virtual ~StreamController() = default;

    virtual void setEvents(StreamEvents events) = 0;
    virtual void setHooks(StreamHooks hooks) = 0;

    // Starts a session on the console: asks for the resolution / bitrate in `settings.video`, waits for the console,
    // connects, opens the WebRTC link. A running attempt is cancelled first.
    virtual void start(const std::string& consoleId, const AppSettings& settings) = 0;
    // Stops the attempt or the stream (closes the link, tells the service). Safe to call at any time.
    virtual void cancel() = 0;

    virtual bool active() const = 0;      // an attempt is running or the stream is up
    virtual bool streaming() const = 0;   // the stream is up (Connected reported)
    // The pad toward the console; dropped when no stream is up.
    virtual void sendGamepad(const GamepadState& state, uint8_t index = 0) = 0;
};

struct XhomeSessionDeps {
    // A StreamApi on the user's region host (refreshes the streaming token as needed). Throws like xhome does.
    std::function<std::shared_ptr<xhome::StreamApi>()> api;
    // Account::transferToken()
    std::function<std::string()> transferToken;
};

std::unique_ptr<StreamController> MakeXhomeStreamController(XhomeSessionDeps deps);

} // namespace fluvi::app
