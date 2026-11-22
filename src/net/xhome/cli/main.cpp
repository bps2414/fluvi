// SPDX-License-Identifier: GPL-3.0-only
// fluvi_xhome_cli: hand-testing tool for the xhome module (not shipped). Talks to Microsoft and a real console.
//   signin                          device-code sign-in (prints a code and a URL, polls until done)
//   consoles                        lists the consoles of the signed-in account
//   wake <serverId>                 XCCS power-on, then polls the power state
//   probe <serverId> [720|1080|1440] [kbps]
//                                   starts a session, connects WebRTC, counts video/audio for 10 s, prints the
//                                   negotiated size and bitrate, stops the session
//   signout                         forgets the saved tokens
// Tokens are never printed. Data dir: %LOCALAPPDATA%\Fluvi\xhome (FLUVI_XHOME_DATA overrides).
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "fluvi/core/Log.h"
#include "fluvi/core/Time.h"
#include "fluvi/xhome/Account.h"
#include "fluvi/xhome/H264Sps.h"
#include "fluvi/xhome/Sdp.h"
#include "fluvi/xhome/StreamApi.h"
#include "fluvi/xhome/WebRtcSession.h"

using namespace fluvi::xhome;
using namespace std::chrono_literals;

namespace {

std::atomic<bool> g_cancel{false};

BOOL WINAPI onCtrl(DWORD) { g_cancel = true; return TRUE; }

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

void sleepCancellable(std::chrono::milliseconds total) {
    const auto end = std::chrono::steady_clock::now() + total;
    while (!g_cancel && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(100ms);
}

std::filesystem::path tokenFile() { return dataDirectory() / "account.bin"; }

const char* profileName(uint8_t idc) {
    switch (idc) {
    case 66: return "Baseline"; case 77: return "Main"; case 88: return "Extended";
    case 100: return "High"; case 110: return "High 10"; case 122: return "High 4:2:2"; case 244: return "High 4:4:4";
    default: return "other";
    }
}

struct Ctx {
    std::shared_ptr<HttpsTransport> http = makeWinHttpTransport(&g_cancel);
    Account account{http, tokenFile()};
};

int cmdSignIn(Ctx& c) {
    const DeviceCode code = c.account.beginSignIn();
    std::printf("Open  %s\nand type the code  %s\n(valid for %d s; waiting...)\n", code.verificationUri.c_str(), code.userCode.c_str(), code.expiresInSeconds);
    std::fflush(stdout);
    int interval = code.intervalSeconds;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(code.expiresInSeconds);
    while (!g_cancel && std::chrono::steady_clock::now() < deadline) {
        sleepCancellable(std::chrono::seconds(interval));
        std::string detail;
        switch (c.account.pollSignIn(code, &detail)) {
        case SignInPoll::Pending: break;
        case SignInPoll::SlowDown: interval += 5; break;
        case SignInPoll::Done: std::printf("Signed in. Tokens saved (DPAPI) in %s\n", narrow(tokenFile().wstring()).c_str()); return 0;
        case SignInPoll::Expired: std::printf("The code expired. Run signin again.\n"); return 1;
        case SignInPoll::Declined: std::printf("Sign-in was declined.\n"); return 1;
        case SignInPoll::Failed: std::printf("Sign-in failed (%s).\n", detail.c_str()); return 1;
        }
    }
    std::printf("Cancelled or timed out.\n");
    return 1;
}

StreamApi makeApi(Ctx& c) {
    if (!c.account.signedIn()) throw std::runtime_error("not signed in; run: fluvi_xhome_cli signin");
    const StreamingAccess access = c.account.streamingAccess();
    std::printf("region %s (%s)\n", access.regionName.c_str(), access.host.c_str());
    StreamApi api(c.http, access.host, access.gsToken);
    api.setWakeAuthorization([&c] { return c.account.webAuthorization(); });
    return api;
}

int cmdConsoles(Ctx& c) {
    StreamApi api = makeApi(c);
    const auto list = api.consoles();
    if (list.empty()) std::printf("no consoles on this account\n");
    for (const auto& k : list)
        std::printf("%s  %-24s %-18s %s%s\n", k.serverId.c_str(), k.name.c_str(), k.powerState.c_str(), k.consoleType.c_str(), k.outOfHomeWarning ? "  (out-of-home warning)" : "");
    return 0;
}

int cmdWake(Ctx& c, const std::string& id) {
    StreamApi api = makeApi(c);
    api.wake(id);
    std::printf("wake command accepted; watching the power state (60 s)...\n");
    std::string last;
    for (int i = 0; i < 20 && !g_cancel; ++i) {
        for (const auto& k : api.consoles()) {
            if (k.serverId != id) continue;
            if (k.powerState != last) { std::printf("  %s: %s\n", id.c_str(), k.powerState.c_str()); std::fflush(stdout); last = k.powerState; }
        }
        if (last == "On") return 0;
        sleepCancellable(3s);
    }
    std::printf("the console did not report On within 60 s (last: %s)\n", last.empty() ? "unknown" : last.c_str());
    return 1;
}

class ApiSignaling final : public Signaling {
public:
    ApiSignaling(StreamApi& api, std::string session) : api_(api), session_(std::move(session)) {}
    std::string exchangeSdp(const std::string& offer) override { return api_.exchangeSdp(session_, offer); }
    std::vector<IceCandidate> exchangeIce(const std::vector<IceCandidate>& local) override { return api_.exchangeIce(session_, local); }
private:
    StreamApi& api_;
    std::string session_;
};

int cmdProbe(Ctx& c, const std::string& id, uint32_t height, uint32_t kbps) {
    StreamConfig cfg;
    cfg.height = height;
    cfg.width = height * 16 / 9;
    cfg.maxBitrateKbps = kbps;
    StreamApi api = makeApi(c);
    std::printf("requesting %ux%u, b=AS %u kbps%s\n", cfg.width, cfg.height, effectiveBitrateKbps(cfg), kbps ? "" : " (default: highest Greenlight offers)");

    const std::string session = api.play(id, cfg);
    std::printf("session %s started; waiting for the console...\n", session.c_str());
    WebRtcSession rtc;
    struct Stop {   // always stop the session, even on an early return
        StreamApi& api; std::string id; WebRtcSession& rtc;
        ~Stop() { rtc.close(); try { api.stop(id); std::printf("session stopped\n"); } catch (const std::exception& e) { std::printf("stop failed: %s\n", e.what()); } }
    } stop{api, session, rtc};

    bool connectSent = false;
    const auto deadline = std::chrono::steady_clock::now() + 120s;
    for (;;) {
        if (g_cancel) return 1;
        if (std::chrono::steady_clock::now() > deadline) { std::printf("the console did not get ready in 120 s\n"); return 1; }
        const SessionState st = api.state(session);
        std::printf("  state %s\n", st.state.c_str());
        if (st.state == "Provisioned") break;
        if (st.state == "Failed") { std::printf("session failed: %s / %s\n", st.errorCode.c_str(), st.errorMessage.c_str()); return 1; }
        if (st.state == "ReadyToConnect" && !connectSent) { api.connect(session, c.account.transferToken()); connectSent = true; continue; }
        sleepCancellable(1s);
    }

    std::atomic<uint64_t> units{0}, bytes{0}, audio{0}, rumbles{0};
    std::mutex m;
    H264SpsInfo sps;
    bool haveSps = false;
    uint32_t serverW = 0, serverH = 0;
    std::string endedReason;
    WebRtcCallbacks cb;
    cb.video = [&](std::vector<uint8_t>&& u, uint32_t, int64_t) {
        ++units; bytes += u.size();
        std::lock_guard l(m);
        H264SpsInfo info;
        if (parseH264Sps(u.data(), u.size(), &info)) { sps = info; haveSps = true; }
    };
    cb.audio = [&](const uint8_t*, size_t, uint32_t) { ++audio; };
    cb.rumble = [&](const fluvi::Rumble&) { ++rumbles; };
    cb.serverVideoSize = [&](uint32_t w, uint32_t h) { std::lock_guard l(m); serverW = w; serverH = h; };
    cb.ended = [&](const std::string& r) { std::lock_guard l(m); endedReason = r; };
    rtc.setCallbacks(cb);

    ApiSignaling signaling(api, session);
    std::string error;
    if (!rtc.start(signaling, cfg, &error)) { std::printf("WebRTC failed: %s\n", error.c_str()); return 1; }
    const bool ready = rtc.waitReady(10s);
    std::printf("connected; handshake %s\n", ready ? "done" : "NOT done");
    rtc.requestKeyframe();

    const auto t0 = std::chrono::steady_clock::now();
    auto lastKeepalive = t0;
    while (!g_cancel && std::chrono::steady_clock::now() - t0 < 10s) {
        rtc.sendGamepad({});   // neutral pad keeps the input channel exercised
        if (std::chrono::steady_clock::now() - lastKeepalive > 5s) { try { api.keepalive(session); } catch (...) {} lastKeepalive = std::chrono::steady_clock::now(); }
        std::this_thread::sleep_for(100ms);
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const WebRtcStats st = rtc.stats();
    std::lock_guard l(m);
    std::printf("---- probe result (%.1f s) ----\n", secs);
    std::printf("requested      %ux%u, offer b=AS %u kbps\n", cfg.width, cfg.height, mediaBitrate(rtc.offerSdp(), "video"));
    std::printf("answer b=AS    %u kbps (0 = the console sent none)\n", mediaBitrate(rtc.answerSdp(), "video"));
    if (haveSps) std::printf("encoded size   %ux%u (SPS), %s profile, level %u.%u\n", sps.width, sps.height, profileName(sps.profileIdc), sps.levelIdc / 10, sps.levelIdc % 10);
    else std::printf("encoded size   no SPS seen\n");
    std::printf("server reports %ux%u\n", serverW, serverH);
    std::printf("video          %llu access units (%.1f fps), %.2f Mbit/s measured\n", (unsigned long long)units.load(), double(units.load()) / secs, double(bytes.load()) * 8.0 / secs / 1e6);
    std::printf("audio packets  %llu, rumble reports %llu, keyframe requests %llu, rtt %.0f ms\n", (unsigned long long)audio.load(), (unsigned long long)rumbles.load(), (unsigned long long)st.keyframeRequests, st.rttMs);
    if (!endedReason.empty()) std::printf("the console ended the stream: %s\n", endedReason.c_str());
    return units.load() > 0 && audio.load() > 0 ? 0 : 1;
}

void usage() {
    std::printf("usage: fluvi_xhome_cli signin | consoles | wake <id> | probe <id> [720|1080|1440] [kbps] | signout\n");
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(onCtrl, TRUE);
    if (argc < 2) { usage(); return 2; }
    const std::string cmd = argv[1];
    int rc = 2;
    try {
        Ctx ctx;
        if (cmd == "signin") rc = cmdSignIn(ctx);
        else if (cmd == "consoles") rc = cmdConsoles(ctx);
        else if (cmd == "wake" && argc >= 3) rc = cmdWake(ctx, argv[2]);
        else if (cmd == "probe" && argc >= 3) {
            const uint32_t h = argc >= 4 ? uint32_t(std::atoi(argv[3])) : 1440;
            if (h != 720 && h != 1080 && h != 1440) { std::printf("resolution must be 720, 1080 or 1440\n"); return 2; }
            rc = cmdProbe(ctx, argv[2], h, argc >= 5 ? uint32_t(std::atoi(argv[4])) : 0);
        } else if (cmd == "signout") { ctx.account.signOut(); std::printf("signed out; saved tokens removed\n"); rc = 0; }
        else usage();
    } catch (const ServiceError& e) {
        std::printf("service error %d: %s\n", e.status, e.what());
        const std::wstring hint = describeXboxError(e.body);
        if (!hint.empty()) std::printf("%s\n", narrow(hint).c_str());
        rc = 1;
    } catch (const std::exception& e) {
        std::printf("error: %s\n", e.what());
        rc = 1;
    }
    shutdownWebRtc();
    return rc;
}
