// SPDX-License-Identifier: GPL-3.0-only
// Ported from Veyra (GPL-3.0-only), tests/xbox/XboxTests.cpp @ c6633121. Changes: doctest, fluvi::xhome types, plus tests for
// SDP b=AS, the 1440p messages, DPAPI storage, wake, SPS parsing and GamepadState/Rumble.
//
// Offline tests for the xhome module: the report formats, the session API and the sign-in chain against scripted
// service replies, and the WebRTC session against a second local libdatachannel peer that plays the console
// (real ICE, DTLS, SCTP and RTP on loopback). None of this proves that Microsoft's services or a real console
// accept the client; it proves the client does what the reference client does and survives the replies it can get.
// Nothing here needs the internet or a Microsoft account.
#include <windows.h>
#include <wincrypt.h>

#include <rtc/rtc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <thread>

#include "fluvi/core/Time.h"
#include "fluvi/xhome/Account.h"
#include "fluvi/xhome/DisconnectPolicy.h"
#include "fluvi/xhome/H264Sps.h"
#include "fluvi/xhome/Protocol.h"
#include "fluvi/xhome/Sdp.h"
#include "fluvi/xhome/StreamApi.h"
#include "fluvi/xhome/VideoDecodeRecovery.h"
#include "fluvi/xhome/WebRtcSession.h"

using namespace fluvi::xhome;
using json = nlohmann::json;

namespace {

// libdatachannel keeps global threads until Cleanup; run it once when the test process exits.
struct RtcCleanupAtExit { ~RtcCleanupAtExit() { shutdownWebRtc(); } } g_rtcCleanup;

// ---------------------------------------------------------------- scripted HTTPS

struct FakeTransport final : HttpsTransport {
    struct Reply { std::string host, path; int status; std::string body; };
    std::deque<Reply> script;
    std::vector<HttpsRequest> seen;
    std::mutex mutex;
    HttpsResponse send(const HttpsRequest& r) override {
        std::lock_guard lock(mutex);
        seen.push_back(r);
        if (script.empty()) return {599, "no scripted reply for " + r.host + r.path};
        Reply reply = script.front();
        if (reply.host != r.host || reply.path != r.path) return {598, "unexpected request " + r.method + " " + r.host + r.path + " (wanted " + reply.host + reply.path + ")"};
        script.pop_front();
        return {reply.status, reply.body};
    }
    void add(std::string host, std::string path, int status, std::string body) { script.push_back({std::move(host), std::move(path), status, std::move(body)}); }
};

std::filesystem::path makeTempDir(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() / (std::string("fluvi-xhome-") + tag + "-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

// Sets FLUVI_XHOME_DATA for the scope of a test.
struct DataDirOverride {
    std::filesystem::path dir;
    explicit DataDirOverride(const char* tag) : dir(makeTempDir(tag)) { SetEnvironmentVariableW(L"FLUVI_XHOME_DATA", dir.c_str()); }
    ~DataDirOverride() {
        SetEnvironmentVariableW(L"FLUVI_XHOME_DATA", nullptr);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

int64_t ms(double v) { return fluvi::time::MsToQpc(v); }

} // namespace

// ================================================================ protocol

TEST_CASE("protocol: client metadata report") {
    const auto meta = clientMetadataReport(0, 12.5, 1);
    CHECK(meta.size() == 15);
    CHECK(meta[0] == 8);
    CHECK(meta[1] == 0);
    CHECK(meta[14] == 1);
    double t = 0;
    std::memcpy(&t, meta.data() + 6, 8);
    CHECK(t == 12.5);   // little-endian double at offset 6
}

TEST_CASE("protocol: gamepad report bytes from a GamepadState") {
    fluvi::GamepadState pad;
    pad.buttons = fluvi::BtnA | fluvi::BtnMenu | fluvi::BtnNexus;
    pad.leftX = 1000; pad.leftY = -2; pad.rightX = -32767; pad.rightY = 32767;
    pad.leftTrigger = 65535; pad.rightTrigger = 257;
    const auto r = gamepadReport(7, 0, pad);
    REQUIRE(r.size() == 14 + 1 + 23);
    CHECK(r[0] == 2);
    CHECK(r[2] == 7);
    CHECK(r[14] == 1);   // one pad
    CHECK(r[15] == 0);   // index 0
    CHECK(r[16] == uint8_t((fluvi::BtnA | fluvi::BtnMenu | fluvi::BtnNexus) & 0xFF));
    CHECK(r[17] == 0);
    CHECK((r[18] == 0xE8 && r[19] == 0x03));   // left X 1000
    CHECK((r[20] == 0xFE && r[21] == 0xFF));   // left Y -2
    CHECK((r[22] == 0x01 && r[23] == 0x80));   // right X -32767
    CHECK((r[24] == 0xFF && r[25] == 0x7F));   // right Y 32767
    CHECK((r[26] == 0xFF && r[27] == 0xFF && r[28] == 0x01 && r[29] == 0x01));   // triggers
    // physicality LE 1, then virtual physicality written big-endian, as the reference client does
    CHECK((r[30] == 1 && r[31] == 0 && r[32] == 0 && r[33] == 0 && r[34] == 0 && r[35] == 0 && r[36] == 0 && r[37] == 1));
    // the pad index goes through
    const auto r2 = gamepadReport(1, 0, pad, 3);
    CHECK(r2[15] == 3);
    // several pads
    const PadEntry pads[] = {{0, pad}, {1, fluvi::GamepadState{}}};
    const auto r3 = gamepadReport(2, 0, pads);
    CHECK(r3.size() == 14 + 1 + 46);
    CHECK(r3[14] == 2);
    CHECK(r3[15 + 23] == 1);
}

TEST_CASE("protocol: rumble report") {
    const uint8_t vib[] = {128, 0, 0, 0, 80, 20, 0, 5, 0x10, 0x00, 0x05, 0x00, 1};
    uint8_t pad = 9;
    const auto v = parseRumble(vib, sizeof(vib), &pad);
    REQUIRE(v);
    CHECK(pad == 0);
    CHECK(v->leftMotor == 80);
    CHECK(v->rightMotor == 20);
    CHECK(v->leftTrigger == 0);
    CHECK(v->rightTrigger == 5);
    CHECK(v->durationMs == 16);
    CHECK(v->delayMs == 5);
    CHECK(v->repeat == 1);
    CHECK_FALSE(parseRumble(vib, 5));                       // short report
    const uint8_t wrong[] = {16, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0};
    CHECK_FALSE(parseRumble(wrong, sizeof(wrong)));         // not a vibration report
}

TEST_CASE("protocol: server metadata is height then width") {
    const uint8_t meta[] = {16, 0, 0x38, 0x04, 0, 0, 0x80, 0x07, 0, 0};
    uint32_t w = 0, h = 0;
    CHECK(parseServerMetadata(meta, sizeof(meta), &w, &h));
    CHECK(w == 1920);
    CHECK(h == 1080);
}

TEST_CASE("protocol: video feedback matches Greenlight's seven-field wire format") {
    const auto encoded = videoFeedbackReport(7, 1234, {0xFABCDE12, 101, 102, 103, 104});
    REQUIRE(encoded.size() == 43);
    CHECK(detail::get32(encoded.data() + 2) == 7);
    CHECK(detail::get32(encoded.data() + 15) == 0xFABCDE12);
    CHECK(detail::get32(encoded.data() + 19) == 101);
    CHECK(detail::get32(encoded.data() + 23) == 102);
    CHECK(detail::get32(encoded.data() + 27) == 103);
    CHECK(detail::get32(encoded.data() + 31) == 104);
    CHECK(detail::get32(encoded.data() + 35) == 1234);
    CHECK(detail::get32(encoded.data() + 39) == 1234);
}

// ================================================================ 1440p / resolution messages (Greenlight PR #1752)

TEST_CASE("resolution messages have the shape of Greenlight PR #1752 for 720p, 1080p and 1440p") {
    struct Case { uint32_t w, h; };
    for (const Case c : {Case{1280, 720}, Case{1920, 1080}, Case{2560, 1440}}) {
        CAPTURE(c.h);
        // renderer/lib/streamresolution.ts: control.send(JSON.stringify({...}))
        const json pref = json::parse(videoPreferenceMessage(c.w, c.h));
        const json expectedPref = {{"message", "VideoPreference"}, {"type", "VideoPreference"},
                                   {"resolution", {{"width", c.w}, {"height", c.h}}},
                                   {"dimensions", {{"width", c.w}, {"height", c.h}}}, {"maxFPS", 60}};
        CHECK(pref == expectedPref);
        const json update = json::parse(userRequestedResolutionUpdateMessage(c.h));
        CHECK(update == json{{"message", "userRequestedResolutionUpdate"}, {"resolutionAlias", std::to_string(c.h)}});
        CHECK(update["resolutionAlias"].is_string());   // a string, like String(height)

        // dimensionschanged: horizontal/vertical/preferred*/safeAreaRight/Bottom are the requested size
        const json dim = json::parse(dimensionsContent(c.w, c.h));
        CHECK(dim["horizontal"] == c.w);
        CHECK(dim["vertical"] == c.h);
        CHECK(dim["preferredWidth"] == c.w);
        CHECK(dim["preferredHeight"] == c.h);
        CHECK(dim["safeAreaRight"] == c.w);
        CHECK(dim["safeAreaBottom"] == c.h);
        CHECK(dim["safeAreaLeft"] == 0);
        CHECK(dim["supportsCustomResolution"] == true);

        // X-MS-Device-Info displayInfo
        const json info = json::parse(deviceInfoJson(c.w, c.h));
        CHECK(info["dev"]["displayInfo"]["dimensions"]["widthInPixels"] == c.w);
        CHECK(info["dev"]["displayInfo"]["dimensions"]["heightInPixels"] == c.h);
        CHECK(info["appInfo"]["env"]["clientAppId"] == "Microsoft.GamingApp");
    }
    // the width the PR derives (height * 16 / 9) is exact for the three sizes
    CHECK(1440 * 16 / 9 == 2560);
    CHECK(1080 * 16 / 9 == 1920);
    CHECK(720 * 16 / 9 == 1280);
    // message-channel wrapper
    const json wrapped = json::parse(wrapMessage("/x/y", "{\"a\":1}", "id-1"));
    CHECK(wrapped == json{{"type", "Message"}, {"content", "{\"a\":1}"}, {"id", "id-1"}, {"target", "/x/y"}, {"cv", ""}});
}

// ================================================================ SDP munging (Greenlight sdp.ts b=AS)

namespace {
const char* kOffer =
    "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0 1\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\nc=IN IP4 0.0.0.0\r\na=mid:0\r\na=rtpmap:111 opus/48000/2\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 102 104\r\nc=IN IP4 0.0.0.0\r\na=mid:1\r\na=recvonly\r\n"
    "a=rtpmap:102 H264/90000\r\na=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=4d001f\r\n"
    "a=rtpmap:104 H264/90000\r\na=fmtp:104 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n";
}

TEST_CASE("sdp: b=AS goes after the c= line of the video section only") {
    const std::string out = setMediaBitrate(kOffer, "video", 12345);
    CHECK(mediaBitrate(out, "video") == 12345);
    CHECK(mediaBitrate(out, "audio") == 0);
    CHECK(out.find("m=video 9 UDP/TLS/RTP/SAVPF 102 104\r\nc=IN IP4 0.0.0.0\r\nb=AS:12345\r\na=mid:1") != std::string::npos);
    CHECK(out.find("b=AS") == out.rfind("b=AS"));          // exactly one
    CHECK(out.find("\n") != std::string::npos);
    // CRLF everywhere, never a bare LF
    bool crlfOnly = true;
    for (size_t i = 0; i < out.size(); ++i) if (out[i] == '\n' && (i == 0 || out[i - 1] != '\r')) crlfOnly = false;
    CHECK(crlfOnly);
    // H.264 untouched
    CHECK(out.find("a=rtpmap:102 H264/90000") != std::string::npos);
    CHECK(out.find("profile-level-id=4d001f") != std::string::npos);
}

TEST_CASE("sdp: an existing b= line is replaced, a missing section leaves the SDP alone") {
    const std::string once = setMediaBitrate(kOffer, "video", 5000);
    const std::string twice = setMediaBitrate(once, "video", 7000);
    CHECK(mediaBitrate(twice, "video") == 7000);
    CHECK(twice.find("b=AS:5000") == std::string::npos);
    CHECK(twice.find("b=AS") == twice.rfind("b=AS"));
    CHECK(setMediaBitrate(kOffer, "application", 100) == kOffer);
    // LF-only input is accepted and comes out CRLF
    const std::string lf = "m=video 9 X 96\nc=IN IP4 0.0.0.0\na=mid:1\n";
    CHECK(setMediaBitrate(lf, "video", 1) == "m=video 9 X 96\r\nc=IN IP4 0.0.0.0\r\nb=AS:1\r\na=mid:1\r\n");
    // i= lines are skipped like c= lines
    CHECK(setMediaBitrate("m=video 9 X 96\r\ni=x\r\nc=IN IP4 0.0.0.0\r\na=mid:1\r\n", "video", 2) ==
          "m=video 9 X 96\r\ni=x\r\nc=IN IP4 0.0.0.0\r\nb=AS:2\r\na=mid:1\r\n");
}

TEST_CASE("sdp: bitrate 0 means the highest value Greenlight offers (40960 kbps)") {
    StreamConfig config;
    CHECK(config.maxBitrateKbps == 0);
    CHECK(effectiveBitrateKbps(config) == 40960);
    CHECK(kGreenlightMaxBitrateKbps == 40960);
    config.maxBitrateKbps = 15000;
    CHECK(effectiveBitrateKbps(config) == 15000);
    CHECK(withoutCandidates("a=candidate:1 x\r\nm=video\r\na=end-of-candidates\r\na=mid:1\r\n") == "m=video\r\na=mid:1\r\n");
}

// ================================================================ H.264 SPS

namespace {
struct BitWriter {
    std::vector<uint8_t> bytes;
    int used = 8;   // bits used in the last byte; 8 = start a new one
    void bit(unsigned b) { if (used == 8) { bytes.push_back(0); used = 0; } bytes.back() = uint8_t(bytes.back() | (b << (7 - used))); ++used; }
    void bits(uint32_t v, unsigned n) { while (n--) bit((v >> n) & 1u); }
    void ue(uint32_t v) { ++v; unsigned len = 0; for (uint32_t t = v; t; t >>= 1) ++len; for (unsigned i = 1; i < len; ++i) bit(0); bits(v, len); }
};
std::vector<uint8_t> makeSps(uint8_t profile, uint32_t widthMbs, uint32_t heightMbs, uint32_t cropBottom) {
    BitWriter w;
    w.bits(profile, 8); w.bits(0, 8); w.bits(40, 8); w.ue(0);
    if (profile == 100) { w.ue(1); w.ue(0); w.ue(0); w.bit(0); w.bit(0); }   // chroma 4:2:0, 8-bit, no scaling matrix
    w.ue(0); w.ue(0); w.ue(0);                                                // log2_max_frame_num, poc type 0, log2_max_poc_lsb
    w.ue(1); w.bit(0);                                                        // refs, gaps
    w.ue(widthMbs - 1); w.ue(heightMbs - 1); w.bit(1); w.bit(1);              // frame_mbs_only, direct_8x8
    if (cropBottom) { w.bit(1); w.ue(0); w.ue(0); w.ue(0); w.ue(cropBottom); } else w.bit(0);
    w.bit(0);                                                                 // no VUI
    w.bit(1);                                                                 // rbsp stop bit
    std::vector<uint8_t> au = {0, 0, 0, 1, 0x67};
    for (const uint8_t b : w.bytes) au.push_back(b);
    return au;
}
}

TEST_CASE("h264 sps: coded size and profile of Main, High and a real x264 SPS") {
    H264SpsInfo info;
    auto sps = makeSps(77, 120, 68, 4);   // 1920x1088 cropped to 1920x1080
    REQUIRE(parseH264Sps(sps.data(), sps.size(), &info));
    CHECK(info.width == 1920);
    CHECK(info.height == 1080);
    CHECK(info.profileIdc == 77);
    sps = makeSps(100, 160, 90, 0);       // 2560x1440, High
    REQUIRE(parseH264Sps(sps.data(), sps.size(), &info));
    CHECK(info.width == 2560);
    CHECK(info.height == 1440);
    CHECK(info.profileIdc == 100);
    // an access unit with SPS in the middle of other NAL units
    std::vector<uint8_t> au = {0, 0, 0, 1, 0x09, 0xF0};
    sps = makeSps(100, 80, 45, 0);        // 1280x720
    au.insert(au.end(), sps.begin(), sps.end());
    au.insert(au.end(), {0, 0, 0, 1, 0x68, 0xEB, 0xE3, 0xCB, 0x22, 0xC0});
    REQUIRE(parseH264Sps(au.data(), au.size(), &info));
    CHECK(info.width == 1280);
    CHECK(info.height == 720);
    // x264 1080p High L4.0 SPS as emitted by real encoders (with emulation prevention and VUI)
    const uint8_t real[] = {0, 0, 0, 1, 0x67, 0x64, 0x00, 0x28, 0xAC, 0xD9, 0x40, 0x78, 0x02, 0x27, 0xE5, 0xC0, 0x44, 0x00, 0x00, 0x03,
                            0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0xF0, 0x3C, 0x60, 0xC6, 0x58};
    REQUIRE(parseH264Sps(real, sizeof(real), &info));
    CHECK(info.width == 1920);
    CHECK(info.height == 1080);
    CHECK(info.levelIdc == 40);
    const uint8_t none[] = {0, 0, 0, 1, 0x65, 1, 2, 3, 4};
    CHECK_FALSE(parseH264Sps(none, sizeof(none), &info));
}

// ================================================================ session API

TEST_CASE("api: Teredo, console list and ICE parsing") {
    std::string ip;
    int port = 0;
    CHECK(StreamApi::teredoIpv4("2001:0:4136:e378:8000:63bf:3fff:fdd2", &ip, &port));   // the RFC 4380 example
    CHECK(ip == "192.0.2.45");
    CHECK(port == 40000);
    CHECK_FALSE(StreamApi::teredoIpv4("2001:db8::1", &ip, &port));

    const auto consoles = StreamApi::parseConsoles(R"({"totalItems":2,"results":[{"deviceName":"Sala Series X","serverId":"F4001234ABCD5678","powerState":"ConnectedStandby","consoleType":"XboxSeriesX","playPath":"x","outOfHomeWarning":false},{"deviceName":"broken"}],"continuationToken":null})");
    REQUIRE(consoles.size() == 1);   // entries without a server id are skipped
    CHECK(consoles[0].serverId == "F4001234ABCD5678");
    CHECK(consoles[0].name == "Sala Series X");
    CHECK(consoles[0].powerState == "ConnectedStandby");

    const auto ice = StreamApi::parseIce(R"([{"candidate":"a=candidate:1 1 UDP 100 2001:0:4136:e378:8000:63bf:3fff:fdd2 9002 typ host","sdpMid":"0","sdpMLineIndex":"0"},{"candidate":"a=candidate:2 1 UDP 90 192.168.1.30 9002 typ host","sdpMid":"0","sdpMLineIndex":0},{"candidate":"a=end-of-candidates"}])");
    REQUIRE(ice.size() == 4);   // a Teredo candidate adds two IPv4 candidates; end-of-candidates is dropped
    CHECK(ice[0].candidate.find("192.0.2.45 9002") != std::string::npos);
    CHECK(ice[1].candidate.find("192.0.2.45 40000") != std::string::npos);

    const auto nullable = StreamApi::parseIce(R"([{"candidate":null,"sdpMid":null,"sdpMLineIndex":null},{"candidate":"a=candidate:1 1 UDP 90 192.168.1.30 9002 typ host","sdpMid":null,"sdpMLineIndex":null}])");
    REQUIRE(nullable.size() == 1);
    CHECK(nullable[0].sdpMid == "0");
    CHECK(nullable[0].sdpMLineIndex == 0);
}

TEST_CASE("api: scripted session calls") {
    auto http = std::make_shared<FakeTransport>();
    const std::string host = "uks.core.gssv-play-prodxhome.xboxlive.com";
    StreamApi api(http, host, "GS");
    api.setRetryDelayMs(1);
    http->add(host, "/v6/servers/home", 200, R"({"results":[{"deviceName":"X","serverId":"F1","powerState":"On"}]})");
    CHECK(api.consoles().size() == 1);
    CHECK(http->seen.back().headers["Authorization"] == "Bearer GS");

    // play: the size and locale of the config reach the device info and the settings
    http->add(host, "/v5/sessions/home/play", 200, R"({"sessionPath":"v5/sessions/home/ABC-123","state":"Provisioning"})");
    StreamConfig config;   // 1440p, pt-BR
    CHECK(api.play("F1", config) == "ABC-123");
    {
        const json body = json::parse(http->seen.back().body);
        CHECK(body["serverId"] == "F1");
        CHECK(body["settings"]["nanoVersion"] == "V3;WebrtcTransport.dll");
        CHECK(body["settings"]["locale"] == "pt-BR");
        const json info = json::parse(http->seen.back().headers["X-MS-Device-Info"]);
        CHECK(info["dev"]["displayInfo"]["dimensions"]["widthInPixels"] == 2560);
        CHECK(info["dev"]["displayInfo"]["dimensions"]["heightInPixels"] == 1440);
    }
    http->add(host, "/v5/sessions/home/play", 200, R"({"sessionPath":"v5/sessions/home/ABC-124"})");
    config.width = 1920; config.height = 1080;
    api.play("F1", config);
    CHECK(json::parse(http->seen.back().headers["X-MS-Device-Info"])["dev"]["displayInfo"]["dimensions"]["heightInPixels"] == 1080);

    http->add(host, "/v5/sessions/home/ABC-123/state", 200, R"({"state":"Failed","errorDetails":{"code":"WNSError","message":"WaitingForServerToRegister"}})");
    const auto st = api.state("ABC-123");
    CHECK(st.state == "Failed");
    CHECK(st.errorCode == "WNSError");
    CHECK(st.errorMessage == "WaitingForServerToRegister");

    http->add(host, "/v5/sessions/home/ABC-123/sdp", 202, "");
    http->add(host, "/v5/sessions/home/ABC-123/sdp", 204, "");
    http->add(host, "/v5/sessions/home/ABC-123/sdp", 204, "");
    http->add(host, "/v5/sessions/home/ABC-123/sdp", 200, json{{"exchangeResponse", json{{"sdp", "v=0\r\n"}, {"sdpType", "answer"}, {"status", "success"}}.dump()}}.dump());
    CHECK(api.exchangeSdp("ABC-123", "v=0 offer") == "v=0\r\n");   // waits through 204 replies
    {
        const json body = json::parse(http->seen[http->seen.size() - 4].body);
        CHECK(body["messageType"] == "offer");
        CHECK(body["configuration"]["input"]["maxVersion"] == 8);
    }
    http->add(host, "/v5/sessions/home/ABC-123/ice", 200, "");
    http->add(host, "/v5/sessions/home/ABC-123/ice", 200, json{{"exchangeResponse", R"([{"candidate":"a=candidate:2 1 UDP 90 192.168.1.30 9002 typ host","sdpMid":"0","sdpMLineIndex":0}])"}}.dump());
    CHECK(api.exchangeIce("ABC-123", {{"candidate:1 1 udp 2122260223 192.168.1.10 50000 typ host", "0", 0}}).size() == 1);

    http->add(host, "/v5/sessions/home/ABC-123/connect", 401, R"({"code":"Unauthorized"})");
    bool threw = false;
    try { api.connect("ABC-123", "LPT"); } catch (const ServiceError& e) { threw = e.status == 401; }
    CHECK(threw);   // a refused connect is a ServiceError with the status
}

TEST_CASE("api: SDP exchange replies (nullable wrapper, pending, console refusal)") {
    const std::string host = "uks.core.gssv-play-prodxhome.xboxlive.com";
    // The xHome wrapper is nullable even on success (Greenlight's SDPResponse). A pending wrapper differs from a
    // console error; neither may be turned into a json type exception or mistaken for a completed negotiation.
    const std::string answer = json{{"sdp", "v=0\r\n"}, {"sdpType", "answer"}, {"status", "success"}}.dump();
    const auto sdpReply = [&](const std::vector<std::pair<int, json>>& replies, std::string* error = nullptr) {
        auto transport = std::make_shared<FakeTransport>();
        StreamApi session(transport, host, "GS");
        session.setRetryDelayMs(0);
        const std::string path = "/v5/sessions/home/test/sdp";
        transport->add(host, path, 202, "");
        for (const auto& [status, body] : replies) transport->add(host, path, status, body.dump());
        try { return session.exchangeSdp("test", "v=0 offer"); }
        catch (const std::exception& e) { if (error) *error = e.what(); return std::string(); }
    };
    CHECK(sdpReply({{200, {{"exchangeResponse", answer}, {"errorDetails", {{"code", nullptr}, {"message", nullptr}}}}}}) == "v=0\r\n");
    CHECK(sdpReply({{200, {{"exchangeResponse", answer}, {"errorDetails", nullptr}}}}) == "v=0\r\n");
    CHECK(sdpReply({{200, {{"exchangeResponse", answer}, {"errorDetails", {{"code", 0}, {"message", nullptr}}}}}}) == "v=0\r\n");
    CHECK(sdpReply({{200, {{"exchangeResponse", nullptr}, {"errorDetails", {{"code", nullptr}, {"message", nullptr}}}}},
                    {202, {{"exchangeResponse", ""}, {"errorDetails", nullptr}}},
                    {200, {{"exchangeResponse", answer}, {"errorDetails", nullptr}}}}) == "v=0\r\n");
    std::string error;
    CHECK(sdpReply({{200, {{"exchangeResponse", answer}, {"errorDetails", {{"code", 17}, {"message", nullptr}}}}}}, &error).empty());
    CHECK(error.find("17") != std::string::npos);   // a nonzero numeric error is not silently accepted
    error.clear();
    CHECK(sdpReply({{200, {{"exchangeResponse", nullptr}, {"errorDetails", {{"code", "OfferRejected"}, {"message", "unsupported codec"}}}}}}, &error).empty());
    CHECK(error.find("OfferRejected") != std::string::npos);
    CHECK(error.find("unsupported codec") != std::string::npos);
    error.clear();
    std::vector<std::pair<int, json>> pending(40, {200, {{"exchangeResponse", nullptr}, {"errorDetails", nullptr}}});
    CHECK(sdpReply(pending, &error).empty());
    CHECK(error.find("did not answer") != std::string::npos);
    CHECK(error.find("refused") == std::string::npos);
}

TEST_CASE("api: wake sends the XCCS Power/WakeUp command") {
    auto http = std::make_shared<FakeTransport>();
    StreamApi api(http, "weu.core.gssv-play-prodxhome.xboxlive.com", "GS");
    CHECK_THROWS_AS(api.wake("F4001234ABCD5678"), std::runtime_error);   // no web authorization configured
    CHECK(http->seen.empty());

    api.setWakeAuthorization([] { return std::string("XBL3.0 x=123;WEBTOKEN"); });
    http->add("xccs.xboxlive.com", "/commands", 200, R"({"result":{"opId":"x"}})");
    api.wake("F4001234ABCD5678");
    REQUIRE(http->seen.size() == 1);
    const HttpsRequest& r = http->seen[0];
    CHECK(r.method == "POST");
    CHECK(r.headers.at("Authorization") == "XBL3.0 x=123;WEBTOKEN");   // the web token, not the gssv bearer
    CHECK(r.headers.at("x-xbl-contract-version") == "4");
    CHECK(r.headers.at("skillplatform") == "RemoteManagement");
    const json body = json::parse(r.body);
    CHECK(body["destination"] == "Xbox");
    CHECK(body["type"] == "Power");
    CHECK(body["command"] == "WakeUp");
    CHECK(body["linkedXboxId"] == "F4001234ABCD5678");
    CHECK(body["sourceId"] == "com.microsoft.smartglass");
    CHECK(body["sessionId"].get<std::string>().size() == 36);
    CHECK(body["parameters"].is_array());

    http->add("xccs.xboxlive.com", "/commands", 403, R"({"code":"Forbidden"})");
    bool refused = false;
    try { api.wake("F4001234ABCD5678"); } catch (const ServiceError& e) { refused = e.status == 403; }
    CHECK(refused);
}

// ================================================================ sign-in and token storage

TEST_CASE("storage: tokens are DPAPI-protected on disk and round-trip in FLUVI_XHOME_DATA") {
    DataDirOverride data("dpapi");
    CHECK(dataDirectory() == data.dir);
    const auto file = dataDirectory() / "account.bin";

    SavedTokens tokens;
    tokens.refreshToken = "REFRESH-SECRET-1234567890";
    tokens.accessToken = "ACCESS-SECRET-abcdefghij";
    tokens.accessExpiresAt = 1'800'000'000;
    tokens.userHash = "uhs-777";
    REQUIRE(saveTokens(file, tokens));
    CHECK_FALSE(std::filesystem::exists(std::filesystem::path(file).concat(L".tmp")));   // the temp file is gone

    std::ifstream in(file, std::ios::binary);
    const std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    CHECK(raw.find("REFRESH-SECRET") == std::string::npos);
    CHECK(raw.find("ACCESS-SECRET") == std::string::npos);
    CHECK(raw.find("uhs-777") == std::string::npos);
    // a DPAPI blob: version 1 + the DPAPI provider GUID {df9d8cd0-1501-11d1-8c7a-00c04fc297eb}
    REQUIRE(raw.size() > 24);
    const uint8_t guid[] = {0x01, 0x00, 0x00, 0x00, 0xD0, 0x8C, 0x9D, 0xDF, 0x01, 0x15, 0xD1, 0x11, 0x8C, 0x7A, 0x00, 0xC0, 0x4F, 0xC2, 0x97, 0xEB};
    CHECK(std::memcmp(raw.data(), guid, sizeof(guid)) == 0);
    // our entropy is required: a plain CryptUnprotectData (what another program of the same user would try) fails
    DATA_BLOB blob{DWORD(raw.size()), reinterpret_cast<BYTE*>(const_cast<char*>(raw.data()))};
    DATA_BLOB out{};
    CHECK_FALSE(CryptUnprotectData(&blob, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out));
    if (out.pbData) LocalFree(out.pbData);

    const auto loaded = loadTokens(file);
    REQUIRE(loaded);
    CHECK(loaded->refreshToken == tokens.refreshToken);
    CHECK(loaded->accessToken == tokens.accessToken);
    CHECK(loaded->accessExpiresAt == tokens.accessExpiresAt);
    CHECK(loaded->userHash == tokens.userHash);

    // saving again replaces the file; a damaged or empty file reads as "no sign-in"
    tokens.refreshToken = "REFRESH-2";
    REQUIRE(saveTokens(file, tokens));
    CHECK(loadTokens(file)->refreshToken == "REFRESH-2");
    {
        std::string damaged = raw;
        damaged[damaged.size() / 2] = char(damaged[damaged.size() / 2] ^ 0x55);
        std::ofstream(file, std::ios::binary | std::ios::trunc).write(damaged.data(), std::streamsize(damaged.size()));
    }
    CHECK_FALSE(loadTokens(file));
    { std::ofstream(file, std::ios::binary | std::ios::trunc); }
    CHECK_FALSE(loadTokens(file));
    CHECK_FALSE(loadTokens(dataDirectory() / "missing.bin"));
}

TEST_CASE("account: device-code sign-in, token chain, refresh and expiry") {
    DataDirOverride data("account");
    const auto file = dataDirectory() / "account.bin";
    int64_t now = 1'800'000'000;

    auto http = std::make_shared<FakeTransport>();
    {
        Account account(http, file);
        account.setClock([&] { return now; });
        CHECK_FALSE(account.signedIn());
        http->add("login.microsoftonline.com", "/consumers/oauth2/v2.0/devicecode", 200,
                  R"({"user_code":"ABCD-EFGH","device_code":"DEV","verification_uri":"https://www.microsoft.com/link","expires_in":900,"interval":5,"message":"..."})");
        const DeviceCode code = account.beginSignIn();
        CHECK(code.userCode == "ABCD-EFGH");
        CHECK(code.verificationUri == "https://www.microsoft.com/link");
        CHECK(code.intervalSeconds == 5);
        CHECK(http->seen.back().body.find("client_id=1f907974-e22b-4810-a9de-d9647380c97e") != std::string::npos);
        CHECK(http->seen.back().body.find("scope=xboxlive.signin%20openid%20profile%20offline_access") != std::string::npos);
        http->add("login.microsoftonline.com", "/consumers/oauth2/v2.0/token", 400, R"({"error":"authorization_pending"})");
        CHECK(account.pollSignIn(code) == SignInPoll::Pending);
        http->add("login.microsoftonline.com", "/consumers/oauth2/v2.0/token", 400, R"({"error":"slow_down"})");
        CHECK(account.pollSignIn(code) == SignInPoll::SlowDown);
        http->add("login.microsoftonline.com", "/consumers/oauth2/v2.0/token", 200, R"({"access_token":"AT1","refresh_token":"RT1","expires_in":3600})");
        CHECK(account.pollSignIn(code) == SignInPoll::Done);
        CHECK(account.signedIn());
        REQUIRE(std::filesystem::exists(file));
        std::ifstream in(file, std::ios::binary);
        const std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(raw.find("RT1") == std::string::npos);   // not in clear on disk
    }
    {
        Account account(http, file);
        account.setClock([&] { return now; });
        CHECK(account.signedIn());   // survives a restart
        http->add("user.auth.xboxlive.com", "/user/authenticate", 200, R"({"Token":"UT","NotAfter":"2027-01-01T00:00:00.0000000Z","DisplayClaims":{"xui":[{"uhs":"123"}]}})");
        http->add("xsts.auth.xboxlive.com", "/xsts/authorize", 200, R"({"Token":"GSSV","NotAfter":"2027-01-01T00:00:00Z","DisplayClaims":{"xui":[{"uhs":"123"}]}})");
        http->add("xhome.gssv-play-prod.xboxlive.com", "/v2/login/user", 200,
                  R"({"offeringSettings":{"regions":[{"name":"WestEurope","baseUri":"https://weu.core.gssv-play-prodxhome.xboxlive.com","isDefault":false},{"name":"EastAsia","baseUri":"https://eas.core.gssv-play-prodxhome.xboxlive.com/","isDefault":true}]},"market":"CN","gsToken":"GS","tokenType":"bearer","durationInSeconds":14400})");
        const StreamingAccess access = account.streamingAccess();
        CHECK(access.host == "eas.core.gssv-play-prodxhome.xboxlive.com");
        CHECK(access.gsToken == "GS");
        CHECK(access.regionName == "EastAsia");
        const json userAuth = json::parse(http->seen[http->seen.size() - 3].body);
        CHECK(userAuth["Properties"]["RpsTicket"] == "d=AT1");
        CHECK(userAuth["RelyingParty"] == "http://auth.xboxlive.com");
        const json xsts = json::parse(http->seen[http->seen.size() - 2].body);
        CHECK(xsts["RelyingParty"] == "http://gssv.xboxlive.com/");
        CHECK(xsts["Properties"]["UserTokens"][0] == "UT");
        CHECK(http->seen.back().headers["x-gssv-client"] == "XboxComBrowser");
        CHECK(json::parse(http->seen.back().body)["offeringId"] == "xhome");
        const size_t before = http->seen.size();
        account.streamingAccess();
        CHECK(http->seen.size() == before);   // a valid streaming token is reused

        // the web authorization for wake: another XSTS for http://xboxlive.com, built on the cached user token
        http->add("xsts.auth.xboxlive.com", "/xsts/authorize", 200, R"({"Token":"WEBXSTS","NotAfter":"2027-01-01T00:00:00Z","DisplayClaims":{"xui":[{"uhs":"123"}]}})");
        CHECK(account.webAuthorization() == "XBL3.0 x=123;WEBXSTS");
        const json web = json::parse(http->seen.back().body);
        CHECK(web["RelyingParty"] == "http://xboxlive.com");
        CHECK(web["Properties"]["UserTokens"][0] == "UT");
        CHECK(web["Properties"]["SandboxId"] == "RETAIL");
        const size_t afterWeb = http->seen.size();
        account.webAuthorization();
        CHECK(http->seen.size() == afterWeb);   // cached

        http->add("login.live.com", "/oauth20_token.srf", 200, R"({"access_token":"LPT","refresh_token":"RT-X"})");
        CHECK(account.transferToken() == "LPT");
        CHECK(http->seen.back().body.find("PURPOSE_XBOX_CLOUD_CONSOLE_TRANSFER_TOKEN") != std::string::npos);
        CHECK(http->seen.back().body.find("refresh_token=RT1") != std::string::npos);   // from the refresh token

        // An hour later the access token has expired: it is refreshed and the rotated refresh token kept.
        now += 3700;
        http->add("login.microsoftonline.com", "/consumers/oauth2/v2.0/token", 200, R"({"access_token":"AT2","refresh_token":"RT2","expires_in":3600})");
        http->add("login.live.com", "/oauth20_token.srf", 200, R"({"access_token":"LPT2"})");
        CHECK(account.transferToken() == "LPT2");
        CHECK(http->seen.back().body.find("refresh_token=RT2") != std::string::npos);
    }
    {
        Account account(http, file);
        account.setClock([&] { return now + 7200; });
        http->add("login.microsoftonline.com", "/consumers/oauth2/v2.0/token", 400, R"({"error":"invalid_grant"})");
        bool expired = false;
        try { account.transferToken(); } catch (const ServiceError& e) { expired = e.status == 400; }
        CHECK(expired);
        CHECK_FALSE(account.signedIn());
        CHECK_FALSE(std::filesystem::exists(file));   // a refused refresh signs out and deletes the saved sign-in
    }
    {
        // signOut removes the file
        REQUIRE(saveTokens(file, SavedTokens{"R", "A", 1, "u"}));
        Account account(http, file);
        CHECK(account.signedIn());
        account.signOut();
        CHECK_FALSE(account.signedIn());
        CHECK_FALSE(std::filesystem::exists(file));
    }
    CHECK(describeXboxError(R"({"Identity":"0","XErr":2148916233,"Message":""})").find(L"perfil Xbox") != std::wstring::npos);
    CHECK(describeXboxError("not json").empty());
}

// ================================================================ disconnect policy / decode recovery

TEST_CASE("decode recovery and disconnect policy") {
    VideoDecodeRecovery recovery;
    CHECK(recovery.failed(true) == VideoDecodeRecoveryAction::ResetHistory);
    recovery.picture();
    CHECK(recovery.failed(true) == VideoDecodeRecoveryAction::ResetHistory);
    CHECK(recovery.failed(true) == VideoDecodeRecoveryAction::ResetHistory);
    CHECK(recovery.failed(true) == VideoDecodeRecoveryAction::ReopenHardware);   // three hard errors permit one reopen
    recovery.picture();
    CHECK(recovery.failed(true) == VideoDecodeRecoveryAction::ResetHistory);
    CHECK(recovery.failed(true) == VideoDecodeRecoveryAction::ResetHistory);
    CHECK(recovery.failed(true) == VideoDecodeRecoveryAction::UseSoftware);      // a good picture does not refill the reopen budget
    CHECK(recovery.failed(false) == VideoDecodeRecoveryAction::ResetHistory);
    CHECK(recovery.failed(false) == VideoDecodeRecoveryAction::ResetHistory);
    CHECK(recovery.failed(false) == VideoDecodeRecoveryAction::Stop);

    struct Unit { unsigned id; bool inputLoss = false; };
    std::deque<Unit> queue;
    for (unsigned i = 0; i < 8; ++i) queue.push_back({i});
    CHECK(trimVideoInputs(queue, 8) == 1);
    CHECK(queue.size() == 7);
    CHECK(queue.front().id == 1);
    CHECK(queue.front().inputLoss);   // the loss belongs to the first survivor
    queue.push_back({8});
    CHECK(trimVideoInputs(queue, 8) == 1);
    CHECK(queue.front().id == 2);
    CHECK(queue.front().inputLoss);
    queue.pop_front();
    CHECK(trimVideoInputs(queue, 8) == 0);
    CHECK_FALSE(queue.front().inputLoss);

    CHECK(reconnectableDisconnect("KickForServerShutdown"));
    CHECK(reconnectableDisconnect("connection lost"));
    CHECK_FALSE(reconnectableDisconnect("KickForStreamingClientDisconnect"));
    CHECK_FALSE(reconnectableDisconnect("unknown server disconnect"));
}

TEST_CASE("reconnect budget refills only after 30 s of continuous decoded progress") {
    ReconnectBudget budget;
    CHECK(budget.take());
    CHECK(budget.take());
    CHECK(budget.take());
    CHECK_FALSE(budget.take());   // stops at three
    budget.decoded(ms(1000)); budget.decoded(ms(2500)); budget.disconnected(); budget.decoded(ms(40000));
    CHECK_FALSE(budget.take());   // a brief return or a gap cannot refill
    for (double t = 40000; t <= 71000; t += 500) budget.decoded(ms(t));
    CHECK(budget.take());
    CHECK(budget.attempts() == 1);
}

// ================================================================ WebRTC loopback

namespace {

// A second libdatachannel peer playing the console: answers the offer, sends H.264 and Opus, speaks the
// message-channel handshake and records what the client sends.
class FakeConsole final : public Signaling {
public:
    FakeConsole() {
        rtc::Configuration config;
        config.disableAutoNegotiation = true;
        config.bindAddress = "127.0.0.1";   // loopback only: VPN/TUN adapters must not take the test's packets
        pc = std::make_shared<rtc::PeerConnection>(config);
        pc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState) { cv.notify_all(); });
        pc->onTrack([this](std::shared_ptr<rtc::Track> track) {
            auto desc = track->description();
            if (desc.type() == "video") {
                desc.addSSRC(4242, "video");
                track->setDescription(desc);
                auto config = std::make_shared<rtc::RtpPacketizationConfig>(4242, "video", 102, rtc::H264RtpPacketizer::ClockRate);
                track->setMediaHandler(std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::LongStartSequence, config, 1200));
                video = track;
                videoConfig = config;
            } else if (desc.type() == "audio") {
                desc.addSSRC(4343, "audio");
                track->setDescription(desc);
                auto config = std::make_shared<rtc::RtpPacketizationConfig>(4343, "audio", 111, 48000);
                track->setMediaHandler(std::make_shared<rtc::OpusRtpPacketizer>(config));
                audio = track;
                audioConfig = config;
            }
        });
        pc->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
            std::lock_guard lock(mutex);
            channels.push_back(dc);   // libdatachannel drops the callbacks of a channel nobody holds
            labels.push_back(dc->label() + "/" + dc->protocol());
            const auto text = [](rtc::message_variant& data) {
                std::string t;
                if (auto* b = std::get_if<rtc::binary>(&data)) t.assign(reinterpret_cast<const char*>(b->data()), b->size());
                else t = std::get<std::string>(data);
                return t;
            };
            if (dc->label() == "message") {
                message = dc;
                dc->onMessage([this, text](rtc::message_variant data) {
                    const std::string raw = text(data);
                    const json j = json::parse(raw);
                    std::lock_guard l(mutex);
                    messages.push_back(j.value("type", "") + " " + j.value("target", ""));
                    messageRaw.push_back(raw);
                    if (j.value("type", "") == "Handshake") {
                        const std::string ack = json{{"type", "HandshakeAck"}, {"id", j.value("id", "")}, {"cv", ""}}.dump();
                        message->send(ack);
                    }
                    cv.notify_all();
                });
            } else if (dc->label() == "control") {
                dc->onMessage([this, text](rtc::message_variant data) {
                    const std::string raw = text(data);
                    std::lock_guard l(mutex);
                    controlRaw.push_back(raw);
                    control.push_back(json::parse(raw).value("message", ""));
                    cv.notify_all();
                });
            } else if (dc->label() == "input") {
                input = dc;
                dc->onMessage([this](rtc::message_variant data) {
                    if (auto* b = std::get_if<rtc::binary>(&data)) {
                        std::lock_guard l(mutex);
                        reports.emplace_back(reinterpret_cast<const uint8_t*>(b->data()), reinterpret_cast<const uint8_t*>(b->data()) + b->size());
                        cv.notify_all();
                    }
                });
            }
        });
    }
    ~FakeConsole() override { if (pc) pc->close(); }

    std::string exchangeSdp(const std::string& offer) override {
        offerSdp = offer;
        pc->setRemoteDescription(rtc::Description(offer, rtc::Description::Type::Offer));
        pc->setLocalDescription(rtc::Description::Type::Answer);
        std::unique_lock lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(5), [&] { return pc->gatheringState() == rtc::PeerConnection::GatheringState::Complete; });
        std::string sdp = std::string(*pc->localDescription());
        // Answer without candidates: they go through the ICE exchange, as with the service.
        std::string out, line;
        std::istringstream in(sdp);
        while (std::getline(in, line)) {
            if (line.rfind("a=candidate", 0) == 0 || line.rfind("a=end-of-candidates", 0) == 0) continue;
            out += line + "\n";
        }
        return out;
    }

    std::vector<IceCandidate> exchangeIce(const std::vector<IceCandidate>& local) override {
        for (const auto& c : local) pc->addRemoteCandidate(rtc::Candidate(c.candidate, c.sdpMid));
        std::vector<IceCandidate> out;
        for (const auto& c : pc->localDescription()->candidates()) out.push_back({"a=" + std::string(c.candidate()), "0", 0});
        return out;
    }

    template <typename F> bool waitFor(F f, int timeoutMs = 5000) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), f);
    }

    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> video, audio;
    std::shared_ptr<rtc::RtpPacketizationConfig> videoConfig, audioConfig;
    std::shared_ptr<rtc::DataChannel> message, input;
    std::vector<std::shared_ptr<rtc::DataChannel>> channels;
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> labels, messages, messageRaw, control, controlRaw;
    std::vector<std::vector<uint8_t>> reports;
    std::string offerSdp;
};

std::vector<uint8_t> nal(uint8_t header, size_t size, uint8_t seed) {
    std::vector<uint8_t> out = {0, 0, 0, 1, header};
    for (size_t i = 0; i < size; ++i) out.push_back(uint8_t((i * 31 + seed) | 1));   // never a start-code pattern
    return out;
}

// The video section of an SDP (from m=video to the next m= or the end).
std::string videoSection(const std::string& sdp) {
    const auto at = sdp.find("m=video");
    if (at == std::string::npos) return {};
    const auto end = sdp.find("\nm=", at + 1);
    return sdp.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

} // namespace

TEST_CASE("webrtc loopback: a 1440p session end to end") {
    FakeConsole console;
    WebRtcSession session;
    session.setBindAddress("127.0.0.1");
    std::mutex m;
    std::vector<std::vector<uint8_t>> units;
    std::vector<std::vector<uint8_t>> opus;
    std::optional<fluvi::Rumble> rumble;
    std::string endedReason;
    uint32_t serverW = 0, serverH = 0;
    int64_t firstArrival = 0;
    const int64_t before = fluvi::time::NowQpc();
    WebRtcCallbacks callbacks;
    callbacks.video = [&](std::vector<uint8_t>&& unit, uint32_t, int64_t arrival) { std::lock_guard l(m); if (!firstArrival) firstArrival = arrival; units.push_back(std::move(unit)); };
    callbacks.audio = [&](const uint8_t* d, size_t n, uint32_t) { std::lock_guard l(m); opus.emplace_back(d, d + n); };
    callbacks.rumble = [&](const fluvi::Rumble& v) { std::lock_guard l(m); rumble = v; };
    callbacks.serverVideoSize = [&](uint32_t w, uint32_t h) { std::lock_guard l(m); serverW = w; serverH = h; };
    callbacks.ended = [&](const std::string& reason) { std::lock_guard l(m); endedReason = reason; };
    session.setCallbacks(callbacks);

    StreamConfig config;   // 2560x1440, bitrate 0, pt-BR, High first
    std::string error;
    const bool started = session.start(console, config, &error, std::chrono::seconds(15));
    INFO(error);
    REQUIRE(started);

    // --- the offer: H.264 only, High first, b=AS present with the right value
    const std::string& offer = console.offerSdp;
    CHECK(offer == session.offerSdp());
    const std::string video = videoSection(offer);
    REQUIRE_FALSE(video.empty());
    CHECK(video.find("profile-level-id=640032") != std::string::npos);
    CHECK(video.find("profile-level-id=4d001f") != std::string::npos);
    CHECK(video.find("profile-level-id=42e01f") != std::string::npos);
    CHECK(video.find("SAVPF 100 102 104 106") != std::string::npos);   // High is the first preference
    for (const char* other : {"VP8", "VP9", "AV1", "H265", "HEVC"}) CHECK(offer.find(other) == std::string::npos);
    CHECK(mediaBitrate(offer, "video") == 40960);                      // 0 = the Greenlight maximum
    CHECK(video.find("\r\nb=AS:40960\r\n") != std::string::npos);
    CHECK(mediaBitrate(offer, "audio") == 0);
    CHECK(offer.find("a=candidate") == std::string::npos);             // candidates go through the ICE exchange
    CHECK(offer.find("opus/48000/2") != std::string::npos);

    CHECK(session.waitReady(std::chrono::seconds(5)));
    CHECK(console.waitFor([&] { return console.labels.size() == 4; }));
    {
        std::lock_guard l(console.mutex);
        std::vector<std::string> sorted = console.labels;
        std::sort(sorted.begin(), sorted.end());
        CHECK(sorted == std::vector<std::string>{"chat/chatV1", "control/controlV1", "input/1.0", "message/messageV1"});
    }

    // --- control: VideoPreference and userRequestedResolutionUpdate first (PR #1752), then the authorization
    CHECK(console.waitFor([&] { return console.control.size() >= 6; }));
    {
        std::lock_guard l(console.mutex);
        REQUIRE(console.controlRaw.size() >= 6);
        CHECK(json::parse(console.controlRaw[0]) == json::parse(videoPreferenceMessage(2560, 1440)));
        CHECK(json::parse(console.controlRaw[1]) == json{{"message", "userRequestedResolutionUpdate"}, {"resolutionAlias", "1440"}});
        CHECK(console.control[2] == "authorizationRequest");
        CHECK(console.control[3] == "gamepadChanged");
    }
    // --- message channel: dimensionschanged carries 2560x1440
    CHECK(console.waitFor([&] {
        for (const auto& s : console.messages) if (s.find("/streaming/characteristics/dimensionschanged") != std::string::npos) return true;
        return false;
    }));
    {
        std::lock_guard l(console.mutex);
        bool found = false;
        for (const auto& raw : console.messageRaw) {
            const json j = json::parse(raw);
            if (j.value("target", "") != "/streaming/characteristics/dimensionschanged") continue;
            const json content = json::parse(j["content"].get<std::string>());
            CHECK(content["horizontal"] == 2560);
            CHECK(content["vertical"] == 1440);
            CHECK(content["preferredWidth"] == 2560);
            CHECK(content["preferredHeight"] == 1440);
            found = true;
        }
        CHECK(found);
    }

    CHECK(console.waitFor([&] { return !console.reports.empty(); }));
    {
        std::lock_guard l(console.mutex);
        REQUIRE_FALSE(console.reports.empty());
        CHECK(console.reports[0].size() == 15);
        CHECK(console.reports[0][0] == 8);   // first input report is client metadata
    }

    // --- gamepad
    fluvi::GamepadState pad;
    pad.buttons = fluvi::BtnA;
    pad.leftX = 1234;
    session.sendGamepad(pad);
    CHECK(console.waitFor([&] { return console.reports.size() >= 2; }));
    {
        std::lock_guard l(console.mutex);
        const auto r = console.reports.back();
        const auto expected = gamepadReport(1, 0, pad);
        REQUIRE(r.size() == expected.size());
        CHECK(std::equal(r.begin() + 14, r.end(), expected.begin() + 14));   // same bytes after the header
        CHECK(r[0] == 2);
    }

    // --- console -> client: rumble and the server's video size
    const uint8_t vib[] = {128, 0, 0, 0, 60, 40, 0, 0, 100, 0, 0, 0, 0};
    console.input->send(reinterpret_cast<const std::byte*>(vib), sizeof(vib));
    const uint8_t size1440[] = {16, 0, 0xA0, 0x05, 0, 0, 0x00, 0x0A, 0, 0};   // height 1440 (0x5A0), width 2560 (0xA00)
    console.input->send(reinterpret_cast<const std::byte*>(size1440), sizeof(size1440));

    // --- media: IDR access unit larger than one packet (fragmented), a P frame, two Opus packets
    CHECK(console.waitFor([&] { return console.video && console.video->isOpen() && console.audio && console.audio->isOpen(); }));
    std::vector<std::vector<uint8_t>> sent;
    std::vector<uint8_t> idr;
    for (const auto& part : {nal(0x67, 12, 1), nal(0x68, 4, 2), nal(0x65, 5000, 3)}) idr.insert(idr.end(), part.begin(), part.end());
    const auto p1 = nal(0x41, 800, 4);
    sent.push_back(idr);
    sent.push_back(p1);
    uint32_t ts = 0;
    for (const auto& unit : sent) {
        console.videoConfig->timestamp = ts;
        console.video->sendFrame(reinterpret_cast<const std::byte*>(unit.data()), unit.size(), rtc::FrameInfo(ts));
        ts += 1500;
    }
    const auto p2 = nal(0x41, 100, 5);   // the depacketizer emits a frame once the next timestamp starts
    console.video->sendFrame(reinterpret_cast<const std::byte*>(p2.data()), p2.size(), rtc::FrameInfo(ts));
    const std::vector<uint8_t> opus1 = {0xFC, 1, 2, 3}, opus2 = {0xFC, 4, 5, 6, 7};
    console.audio->sendFrame(reinterpret_cast<const std::byte*>(opus1.data()), opus1.size(), rtc::FrameInfo(uint32_t(0)));
    console.audio->sendFrame(reinterpret_cast<const std::byte*>(opus2.data()), opus2.size(), rtc::FrameInfo(uint32_t(960)));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard l(m);
            if (units.size() >= 2 && opus.size() >= 2 && rumble && serverW) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    {
        std::lock_guard l(m);
        REQUIRE(units.size() >= 2);
        CHECK(units[0] == sent[0]);   // the fragmented IDR (SPS, PPS, IDR) arrives byte for byte, Annex-B
        CHECK(units[1] == sent[1]);
        REQUIRE(opus.size() >= 2);
        CHECK(opus[0] == opus1);
        CHECK(opus[1] == opus2);
        REQUIRE(rumble);
        CHECK(rumble->leftMotor == 60);
        CHECK(rumble->rightMotor == 40);
        CHECK(rumble->durationMs == 100);
        CHECK(serverW == 2560);
        CHECK(serverH == 1440);
        CHECK(firstArrival >= before);                         // arrival times are NowQpc() values
        CHECK(firstArrival <= fluvi::time::NowQpc());
    }
    session.requestKeyframe();
    CHECK(console.waitFor([&] { for (const auto& c : console.control) if (c == "videoKeyframeRequested") return true; return false; }));
    const auto stats = session.stats();
    CHECK(stats.videoUnits >= 2);
    CHECK(stats.audioPackets >= 2);
    CHECK(stats.handshakeDone);
    CHECK(stats.videoBytes > 5000);

    // Malformed fields used to throw through the asynchronous callback.
    console.message->send(std::string("{\"type\":null}"));
    console.message->send(std::string("{\"type\":\"Message\",\"target\":null}"));
    const int64_t now = fluvi::time::NowQpc();
    session.videoPresented(0xFABCDE12, now - ms(30), now - ms(20), now - ms(10), now);
    CHECK(console.waitFor([&] { for (const auto& r : console.reports) if (r.size() == 43 && r[0] == ReportMetadata) return true; return false; }));
    {
        std::lock_guard lock(console.mutex);
        bool valid = false;
        for (const auto& r : console.reports) if (r.size() == 43 && r[0] == ReportMetadata)
            valid = r[14] == 1 && detail::get32(r.data() + 15) == 0xFABCDE12;
        CHECK(valid);   // feedback keeps the original RTP key
    }
    // a server shutdown keeps its specific reason
    console.message->send(json{{"type", "TransactionStart"}, {"target", "/streaming/sessionLifetimeManagement/serverInitiatedDisconnect"}, {"id", "shutdown-test"}, {"content", json{{"reason", "KickForServerShutdown"}}.dump()}}.dump());
    for (unsigned i = 0; i < 100; ++i) { { std::lock_guard l(m); if (!endedReason.empty()) break; } std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    { std::lock_guard l(m); CHECK(endedReason == "KickForServerShutdown"); }

    // Real transport shutdown while the caller sends input/keyframe requests: covers the isOpen/send race and
    // local close racing callbacks; an exception escaping any thread would terminate this process.
    std::atomic<bool> stopped = false;
    std::thread sender([&] { while (!stopped) { session.sendGamepad(pad); session.requestKeyframe(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); } });
    console.pc->close();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    session.close(); stopped = true; sender.join();
    session.sendGamepad(pad); session.requestKeyframe(); session.close();
    CHECK_FALSE(session.ready());
}

TEST_CASE("webrtc loopback: a 1080p session with an explicit bitrate and no High offer") {
    FakeConsole console;
    WebRtcSession session;
    session.setBindAddress("127.0.0.1");
    StreamConfig config;
    config.width = 1920; config.height = 1080;
    config.maxBitrateKbps = 12000;
    config.offerHighProfile = false;
    std::string error;
    const bool started = session.start(console, config, &error, std::chrono::seconds(15));
    INFO(error);
    REQUIRE(started);
    CHECK(mediaBitrate(console.offerSdp, "video") == 12000);
    const std::string video = videoSection(console.offerSdp);
    CHECK(video.find("profile-level-id=640032") == std::string::npos);
    CHECK(video.find("SAVPF 102 104 106") != std::string::npos);
    CHECK(video.find("H264/90000") != std::string::npos);
    CHECK(session.waitReady(std::chrono::seconds(5)));
    CHECK(console.waitFor([&] { return console.controlRaw.size() >= 2; }));
    {
        std::lock_guard l(console.mutex);
        CHECK(json::parse(console.controlRaw[0]) == json::parse(videoPreferenceMessage(1920, 1080)));
        CHECK(json::parse(console.controlRaw[1]) == json{{"message", "userRequestedResolutionUpdate"}, {"resolutionAlias", "1080"}});
    }
    session.close();
}
