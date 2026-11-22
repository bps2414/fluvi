// SPDX-License-Identifier: GPL-3.0-only
// app without a window, GPU or network: settings JSON, the saver, pad -> key mapping, xhome -> UI model mapping.
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <windows.h>

#include "Models.h"
#include "PadNav.h"
#include "SettingsStore.h"
#include "fluvi/xhome/Https.h"

using namespace fluvi;
using namespace fluvi::app;
using namespace std::chrono_literals;

namespace {

std::filesystem::path TempDir(const char* name) {
    const auto dir = std::filesystem::path(FLUVI_APP_TEST_TMP) / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

void WriteFile(const std::filesystem::path& p, const std::string& text) {
    std::ofstream(p, std::ios::binary) << text;
}

std::string ReadFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

AppSettings Customised() {
    AppSettings s;
    s.video.resolution = StreamResolution::R1080p;
    s.video.bitrateKbps = 20000;
    s.video.mode = StreamMode::Smooth;
    s.video.deblock = 0.3f;
    s.video.sharpness = 0.9f;
    s.video.fullscreen = false;
    s.video.switchRefreshInFullscreen = true;
    s.video.preset = QualityPreset::Max;
    s.video.presetDetected = true;
    s.video.upscaler = UpscalerChoice::Fsr4;
    s.video.newFramesOnly = true;
    s.controller.rumble = false;
    s.controller.rumbleStrength = 40;
    s.audio.volume = 0.65f;
    s.audio.deviceId = L"{0.0.0.00000000}.{abc-ç}";
    s.hotkeys.toggleOverlay = 0x72;
    s.hotkeys.toggleMode = 0x73;
    s.hotkeys.toggleFullscreen = 0x74;
    s.hotkeys.toggleDebug = 0x75;
    return s;
}

void CheckSame(const AppSettings& a, const AppSettings& b) {
    CHECK(a.video.resolution == b.video.resolution);
    CHECK(a.video.bitrateKbps == b.video.bitrateKbps);
    CHECK(a.video.mode == b.video.mode);
    CHECK(a.video.deblock == b.video.deblock);
    CHECK(a.video.sharpness == b.video.sharpness);
    CHECK(a.video.fullscreen == b.video.fullscreen);
    CHECK(a.video.switchRefreshInFullscreen == b.video.switchRefreshInFullscreen);
    CHECK(a.video.preset == b.video.preset);
    CHECK(a.video.presetDetected == b.video.presetDetected);
    CHECK(a.video.upscaler == b.video.upscaler);
    CHECK(a.video.newFramesOnly == b.video.newFramesOnly);
    CHECK(a.controller.rumble == b.controller.rumble);
    CHECK(a.controller.rumbleStrength == b.controller.rumbleStrength);
    CHECK(a.audio.volume == b.audio.volume);
    CHECK(a.audio.deviceId == b.audio.deviceId);
    CHECK(a.hotkeys.toggleOverlay == b.hotkeys.toggleOverlay);
    CHECK(a.hotkeys.toggleMode == b.hotkeys.toggleMode);
    CHECK(a.hotkeys.toggleFullscreen == b.hotkeys.toggleFullscreen);
    CHECK(a.hotkeys.toggleDebug == b.hotkeys.toggleDebug);
}

} // namespace

// ---- settings --------------------------------------------------------------------------------------------------------

TEST_CASE("settings: every field survives a JSON round trip") {
    const AppSettings in = Customised();
    bool valid = false;
    const AppSettings out = ParseSettings(SerializeSettings(in), &valid);
    CHECK(valid);
    CheckSame(in, out);
}

TEST_CASE("settings: defaults round trip to defaults") {
    CheckSame(AppSettings{}, ParseSettings(SerializeSettings(AppSettings{})));
}

TEST_CASE("settings: a missing file gives the defaults and says so") {
    const auto dir = TempDir("missing");
    const LoadedSettings l = LoadSettings(dir / "settings.json");
    CHECK_FALSE(l.existed);
    CHECK_FALSE(l.valid);
    CheckSame(AppSettings{}, l.settings);
}

TEST_CASE("settings: corrupt, empty and non-object files give the defaults") {
    const auto dir = TempDir("corrupt");
    for (const char* text : {"{ \"video\": { \"resolution\": ", "", "not json at all", "[1, 2, 3]", "42", "\"text\""}) {
        WriteFile(dir / "settings.json", text);
        const LoadedSettings l = LoadSettings(dir / "settings.json");
        CHECK(l.existed);
        CHECK_FALSE(l.valid);
        CheckSame(AppSettings{}, l.settings);
    }
    // And the corrupt file is not carried into the next write.
    const AppSettings s = Customised();
    const std::string text = SerializeSettings(s, "garbage {{{");
    CheckSame(s, ParseSettings(text));
}

TEST_CASE("settings: a UTF-8 BOM from a hand edit is accepted") {
    const auto dir = TempDir("bom");
    WriteFile(dir / "settings.json", "\xEF\xBB\xBF" + SerializeSettings(Customised()));
    const LoadedSettings l = LoadSettings(dir / "settings.json");
    CHECK(l.valid);
    CheckSame(Customised(), l.settings);
}

TEST_CASE("settings: unknown keys are ignored, wrong types and out-of-range values fall back per field") {
    const AppSettings d;
    const AppSettings s = ParseSettings(R"({
        "futureFeature": {"x": 1},
        "video": { "resolution": "8k", "bitrateKbps": -5, "mode": 3, "deblock": 7.5, "sharpness": "high",
                   "fullscreen": "yes", "preset": "light", "upscaler": "magic", "newFramesOnly": true, "extra": [1] },
        "controller": { "rumbleStrength": 250, "rumble": false },
        "audio": { "volume": -1, "deviceId": 12 },
        "hotkeys": { "toggleOverlay": 0, "toggleMode": 114 }
    })");
    CHECK(s.video.resolution == d.video.resolution);        // unknown name
    CHECK(s.video.bitrateKbps == d.video.bitrateKbps);      // negative
    CHECK(s.video.mode == d.video.mode);                    // wrong type
    CHECK(s.video.deblock == 1.0f);                         // clamped
    CHECK(s.video.sharpness == d.video.sharpness);          // wrong type
    CHECK(s.video.fullscreen == d.video.fullscreen);        // wrong type
    CHECK(s.video.preset == QualityPreset::Light);          // valid values still land
    CHECK(s.video.upscaler == d.video.upscaler);
    CHECK(s.video.newFramesOnly);
    CHECK(s.controller.rumbleStrength == d.controller.rumbleStrength);   // 250 > 100
    CHECK_FALSE(s.controller.rumble);
    CHECK(s.audio.volume == 0.0f);                          // clamped
    CHECK(s.audio.deviceId.empty());
    CHECK(s.hotkeys.toggleOverlay == d.hotkeys.toggleOverlay);           // 0 is no key
    CHECK(s.hotkeys.toggleMode == 114);
}

TEST_CASE("settings: keys this build does not know survive a save") {
    const std::string previous = R"({"version": 9, "futureFeature": {"x": 1}, "video": {"holo": true, "mode": "smooth"}})";
    const std::string out = SerializeSettings(AppSettings{}, previous);
    CHECK(out.find("futureFeature") != std::string::npos);
    CHECK(out.find("holo") != std::string::npos);
    CHECK(ParseSettings(out).video.mode == StreamMode::Play);   // known keys take the new value
}

TEST_CASE("settings: the saver debounces bursts into one write, Flush and the destructor write at once") {
    const auto dir = TempDir("saver");
    const auto file = dir / "sub" / "settings.json";   // the folder does not exist yet
    {
        SettingsSaver saver(file, "", 300ms);
        AppSettings s;
        for (int i = 0; i <= 5; ++i) {
            s.audio.volume = static_cast<float>(i) / 10.0f;
            saver.Request(s);
            std::this_thread::sleep_for(10ms);
        }
        CHECK(saver.Writes() == 0);   // still inside the debounce window
        std::this_thread::sleep_for(1000ms);
        CHECK(saver.Writes() == 1);
        CHECK(ParseSettings(ReadFile(file)).audio.volume == 0.5f);

        s.video.fullscreen = false;
        saver.Request(s);
        saver.Flush();
        CHECK(saver.Writes() == 2);
        CHECK_FALSE(ParseSettings(ReadFile(file)).video.fullscreen);

        s.controller.rumbleStrength = 10;
        saver.Request(s);   // written by the destructor
    }
    CHECK(ParseSettings(ReadFile(file)).controller.rumbleStrength == 10);
    CHECK_FALSE(std::filesystem::exists(std::filesystem::path(file).concat(L".tmp")));
}

// ---- pad -> keys -----------------------------------------------------------------------------------------------------

namespace {
GamepadState Pad(uint16_t buttons, int16_t lx = 0, int16_t ly = 0) {
    GamepadState s;
    s.buttons = buttons;
    s.leftX = lx;
    s.leftY = ly;
    return s;
}
NavKey Down(uint32_t vk, bool repeat = false) { return {vk, true, repeat}; }
NavKey Up(uint32_t vk) { return {vk, false, false}; }
}

TEST_CASE("padnav: buttons map to keys, one down per press and one up per release") {
    PadNav nav;
    auto r = nav.Update(Pad(BtnA), 0, true);
    CHECK(r.keys == std::vector<NavKey>{Down(VK_RETURN)});
    CHECK(nav.Update(Pad(BtnA), 16, true).keys.empty());   // held, no repeat for Enter
    CHECK(nav.Update(Pad(BtnA), 2000, true).keys.empty());
    CHECK(nav.Update(Pad(0), 2016, true).keys == std::vector<NavKey>{Up(VK_RETURN)});
    CHECK(nav.Update(Pad(BtnB), 2032, true).keys == std::vector<NavKey>{Down(VK_ESCAPE)});
    CHECK(nav.Update(Pad(0), 2048, true).keys == std::vector<NavKey>{Up(VK_ESCAPE)});
    CHECK(nav.Update(Pad(BtnUp), 3000, true).keys == std::vector<NavKey>{Down(VK_UP)});
    CHECK(nav.Update(Pad(0), 3010, true).keys == std::vector<NavKey>{Up(VK_UP)});
    CHECK(nav.Update(Pad(BtnDown), 3020, true).keys == std::vector<NavKey>{Down(VK_DOWN)});
    CHECK(nav.Update(Pad(BtnLeft), 3030, true).keys == std::vector<NavKey>{Up(VK_DOWN), Down(VK_LEFT)});
    CHECK(nav.Update(Pad(BtnRight), 3040, true).keys == std::vector<NavKey>{Up(VK_LEFT), Down(VK_RIGHT)});
}

TEST_CASE("padnav: arrows repeat after the delay, then at the interval; releasing stops it") {
    PadNavConfig cfg;
    cfg.repeatDelayMs = 400;
    cfg.repeatIntervalMs = 100;
    PadNav nav(cfg);
    CHECK(nav.Update(Pad(BtnRight), 1000, true).keys == std::vector<NavKey>{Down(VK_RIGHT)});
    CHECK(nav.Update(Pad(BtnRight), 1399, true).keys.empty());
    CHECK(nav.Update(Pad(BtnRight), 1400, true).keys == std::vector<NavKey>{Down(VK_RIGHT, true)});
    CHECK(nav.Update(Pad(BtnRight), 1499, true).keys.empty());
    CHECK(nav.Update(Pad(BtnRight), 1500, true).keys == std::vector<NavKey>{Down(VK_RIGHT, true)});
    CHECK(nav.Update(Pad(BtnRight), 1600, true).keys == std::vector<NavKey>{Down(VK_RIGHT, true)});
    // A long frame gives one repeat, not a burst, and the cadence continues from there.
    CHECK(nav.Update(Pad(BtnRight), 3000, true).keys == std::vector<NavKey>{Down(VK_RIGHT, true)});
    CHECK(nav.Update(Pad(BtnRight), 3050, true).keys.empty());
    CHECK(nav.Update(Pad(BtnRight), 3100, true).keys == std::vector<NavKey>{Down(VK_RIGHT, true)});
    CHECK(nav.Update(Pad(0), 3110, true).keys == std::vector<NavKey>{Up(VK_RIGHT)});
    CHECK(nav.Update(Pad(0), 5000, true).keys.empty());
}

TEST_CASE("padnav: the left stick is an arrow on its dominant axis, with hysteresis and a dead zone") {
    PadNav nav;
    CHECK(nav.Update(Pad(0, 8000, 0), 0, true).keys.empty());                         // inside the dead zone
    CHECK(nav.Update(Pad(0, 20000, 0), 10, true).keys == std::vector<NavKey>{Down(VK_RIGHT)});
    CHECK(nav.Update(Pad(0, 12000, 0), 20, true).keys.empty());                       // between off and on: still held
    CHECK(nav.Update(Pad(0, 9000, 0), 30, true).keys == std::vector<NavKey>{Up(VK_RIGHT)});
    CHECK(nav.Update(Pad(0, 12000, 0), 40, true).keys.empty());                       // below `on` again: not pressed
    // Up is positive Y; a diagonal gives only the stronger axis.
    CHECK(nav.Update(Pad(0, 0, 30000), 50, true).keys == std::vector<NavKey>{Down(VK_UP)});
    CHECK(nav.Update(Pad(0, 18000, 30000), 60, true).keys.empty());                   // still Up
    CHECK(nav.Update(Pad(0, 30000, 18000), 70, true).keys == std::vector<NavKey>{Up(VK_UP), Down(VK_RIGHT)});
    CHECK(nav.Update(Pad(0, 0, -30000), 80, true).keys == std::vector<NavKey>{Down(VK_DOWN), Up(VK_RIGHT)});
    // D-pad and stick together are one key; opposite arrows cancel.
    CHECK(nav.Update(Pad(BtnDown, 0, -30000), 90, true).keys.empty());
    CHECK(nav.Update(Pad(BtnLeft | BtnRight), 100, true).keys == std::vector<NavKey>{Up(VK_DOWN)});
}

TEST_CASE("padnav: while the stream owns the pad nothing reaches the UI, and held buttons wait for release") {
    PadNav nav;
    CHECK(nav.Update(Pad(BtnA), 0, true).keys == std::vector<NavKey>{Down(VK_RETURN)});
    // Navigation turns off with A still held (it connected): the key is let go...
    CHECK(nav.Update(Pad(BtnA), 10, false).keys == std::vector<NavKey>{Up(VK_RETURN)});
    // ...and while streaming nothing is emitted, whatever is pressed.
    CHECK(nav.Update(Pad(BtnB | BtnDown), 20, false).keys.empty());
    CHECK(nav.Update(Pad(BtnA | BtnB), 30, false).keys.empty());
    // Back with A held: ignored until released. A button pressed while off is also ignored until released.
    CHECK(nav.Update(Pad(BtnA | BtnB), 40, true).keys.empty());
    CHECK(nav.Update(Pad(BtnA), 50, true).keys.empty());
    CHECK(nav.Update(Pad(0), 60, true).keys.empty());
    CHECK(nav.Update(Pad(BtnA), 70, true).keys == std::vector<NavKey>{Down(VK_RETURN)});
}

TEST_CASE("padnav: View + Menu toggles the overlay once per chord, also while streaming") {
    PadNav nav;
    CHECK_FALSE(nav.Update(Pad(BtnView), 0, true).toggleOverlay);
    CHECK(nav.Update(Pad(BtnView | BtnMenu), 10, true).toggleOverlay);
    CHECK_FALSE(nav.Update(Pad(BtnView | BtnMenu), 20, true).toggleOverlay);   // held
    CHECK_FALSE(nav.Update(Pad(BtnMenu), 30, true).toggleOverlay);
    CHECK(nav.Update(Pad(BtnMenu | BtnView), 40, false).toggleOverlay);        // navEnabled false: still reported
    CHECK_FALSE(nav.Update(Pad(0), 50, false).toggleOverlay);
    CHECK(nav.Update(Pad(BtnView | BtnMenu), 60, false).toggleOverlay);
}

// ---- xhome -> UI model ------------------------------------------------------------------------------------------------

TEST_CASE("models: power state names") {
    CHECK(MapPower("On") == ui::PowerState::On);
    CHECK(MapPower("ConnectedStandby") == ui::PowerState::Standby);
    CHECK(MapPower("Off") == ui::PowerState::Off);
    CHECK(MapPower("") == ui::PowerState::Unknown);
    CHECK(MapPower("Whatever") == ui::PowerState::Unknown);
}

TEST_CASE("models: console type names") {
    CHECK(ConsoleTypeName("XboxSeriesX") == "Xbox Series X");
    CHECK(ConsoleTypeName("XboxSeriesS") == "Xbox Series S");
    CHECK(ConsoleTypeName("XboxOne") == "Xbox One");
    CHECK(ConsoleTypeName("XboxOneX") == "Xbox One X");
    CHECK(ConsoleTypeName("") == "");
}

TEST_CASE("models: console list -> UI model") {
    std::vector<xhome::Console> list(3);
    list[0] = {"F4000A1B", "Sala", "On", "XboxSeriesX", false};
    list[1] = {"F4000E5F", "", "ConnectedStandby", "XboxSeriesS", false};
    list[2] = {"F4000C8D", "Escritório", "Off", "XboxOne", true};
    ConsolesContext ctx;
    ctx.accountName = "Conta";
    ctx.busyId = "F4000E5F";
    ctx.refreshing = true;
    ctx.error = "x";
    const ui::ConsolesModel m = MapConsoles(list, ctx);
    REQUIRE(m.consoles.size() == 3);
    CHECK(m.accountName == "Conta");
    CHECK(m.refreshing);
    CHECK(m.error == "x");
    CHECK(m.consoles[0].id == "F4000A1B");
    CHECK(m.consoles[0].name == "Sala");
    CHECK(m.consoles[0].type == "Xbox Series X");
    CHECK(m.consoles[0].power == ui::PowerState::On);
    CHECK_FALSE(m.consoles[0].busy);
    CHECK(m.consoles[1].name == "Xbox Series S");   // no name: the type stands in
    CHECK(m.consoles[1].power == ui::PowerState::Standby);
    CHECK(m.consoles[1].busy);                      // the one being woken
    CHECK(m.consoles[2].power == ui::PowerState::Off);
    CHECK(MapConsoles({}, {}).consoles.empty());
}

TEST_CASE("models: connect stages and error sentences") {
    CHECK(MapStage(StreamStage::Provisioning) == ui::ConnectStage::Provisioning);
    CHECK(MapStage(StreamStage::ReadyToConnect) == ui::ConnectStage::ReadyToConnect);
    CHECK(MapStage(StreamStage::Connecting) == ui::ConnectStage::Connecting);
    CHECK(MapStage(StreamStage::Connected) == ui::ConnectStage::Connected);

    CHECK(DescribeError(xhome::NetworkError("timeout")).find("conexão") != std::string::npos);
    CHECK(DescribeError(xhome::ServiceError(503, "", "x")).find("503") != std::string::npos);
    CHECK(DescribeError(xhome::ServiceError(429, "", "x")).find("429") != std::string::npos);
    CHECK(DescribeError(std::runtime_error("boom")).find("boom") != std::string::npos);
    CHECK(DescribeSessionFailure("0x87E10BC6", "sem espaço").find("0x87E10BC6") != std::string::npos);
    const std::string wns = DescribeSessionFailure("WNSError", "Xccs : ErrorCallingWNS : Send command failed : State WaitingForServerToRegister");
    CHECK(wns.find("Suspensão") != std::string::npos);
    CHECK(wns.find("WNS") == std::string::npos);
    CHECK(DescribeEnded("connection lost") == "A conexão com o console caiu.");
    CHECK_FALSE(DescribeSignInFailure(xhome::SignInPoll::Expired, "").empty());
    CHECK(DescribeSignInFailure(xhome::SignInPoll::Pending, "").empty());
}
