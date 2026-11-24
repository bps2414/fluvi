// SPDX-License-Identifier: GPL-3.0-only
// app without a window, GPU or network: settings JSON, the saver, pad -> key mapping, xhome -> UI model mapping.
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <windows.h>

#include <nlohmann/json.hpp>

#include "Models.h"
#include "PadNav.h"
#include "SettingsStore.h"
#include "WindowGeometry.h"
#include "fluvi/common/I18n.h"
#include "fluvi/xhome/Https.h"

using namespace fluvi;
using namespace fluvi::app;
using namespace std::chrono_literals;

namespace {

// The sentences the app produces come from assets/i18n; the tests read them in pt-BR.
bool LanguageLoaded() {
    static const bool ok = [] {
        Status st;
        return i18n::Load(FLUVI_I18N_DIR, "pt-BR", st);
    }();
    return ok;
}

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
    s.hotkeys.cycleHud = 0x76;
    s.hud.mode = HudMode::Full;
    s.hud.corner = HudCorner::BottomRight;
    s.window = {true, -1500, 40, 1600, 900, true};
    s.language = "en-US";
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
    CHECK(a.hotkeys.cycleHud == b.hotkeys.cycleHud);
    CHECK(a.hud.mode == b.hud.mode);
    CHECK(a.hud.corner == b.hud.corner);
    CHECK(a.window.hasPlacement == b.window.hasPlacement);
    CHECK(a.window.x == b.window.x);
    CHECK(a.window.y == b.window.y);
    CHECK(a.window.width == b.window.width);
    CHECK(a.window.height == b.window.height);
    CHECK(a.window.maximized == b.window.maximized);
    CHECK(a.language == b.language);
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

TEST_CASE("settings: a file from before the HUD, window and language settings still loads with their defaults") {
    const AppSettings d;
    const AppSettings s = ParseSettings(R"({"version": 1, "video": {"resolution": "1080p"}, "hotkeys": {"toggleMode": 114}})");
    CHECK(s.video.resolution == StreamResolution::R1080p);
    CHECK(s.hotkeys.toggleMode == 114);
    CHECK(s.hotkeys.cycleHud == d.hotkeys.cycleHud);
    CHECK(s.hud.mode == HudMode::Off);
    CHECK(s.hud.corner == HudCorner::TopLeft);
    CHECK_FALSE(s.window.hasPlacement);
    CHECK(s.language == "auto");
}

TEST_CASE("settings: HUD, window and language are validated field by field") {
    const AppSettings d;
    const AppSettings s = ParseSettings(R"({
        "hud": { "mode": "huge", "corner": "bottomLeft" },
        "window": { "x": 10, "y": 20, "width": 50, "height": 900, "maximized": true },
        "language": "../etc",
        "hotkeys": { "cycleHud": 300 }
    })");
    CHECK(s.hud.mode == d.hud.mode);                        // unknown name
    CHECK(s.hud.corner == HudCorner::BottomLeft);
    CHECK_FALSE(s.window.hasPlacement);                     // width below the usable size: no placement at all
    CHECK_FALSE(s.window.maximized);
    CHECK(s.language == "auto");                            // not a folder name
    CHECK(s.hotkeys.cycleHud == d.hotkeys.cycleHud);        // 300 is not a virtual key

    const AppSettings ok = ParseSettings(R"({"window": {"x": -30, "y": 5, "width": 1280, "height": 720}, "language": "pt-BR"})");
    CHECK(ok.window.hasPlacement);
    CHECK(ok.window.x == -30);
    CHECK(ok.window.width == 1280);
    CHECK_FALSE(ok.window.maximized);
    CHECK(ok.language == "pt-BR");

    // Without a placement the window key is not written (so a stale one does not come back).
    const std::string out = SerializeSettings(AppSettings{}, R"({"window": {"x": 1, "y": 2, "width": 800, "height": 600}})");
    CHECK(out.find("\"window\"") == std::string::npos);
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

    REQUIRE(LanguageLoaded());
    const auto raw = [](const std::string& text) {   // nothing technical reaches the screen
        for (const char* bad : {"HTTP", "WNS", "Error", "0x", "boom", "XErr", "503", "429"})
            if (text.find(bad) != std::string::npos) return true;
        return text.empty() || text.find("app.") == 0;
    };
    CHECK(DescribeError(xhome::NetworkError("timeout")).find("conexão") != std::string::npos);
    for (int status : {401, 404, 429, 500, 503, 418}) CHECK_FALSE(raw(DescribeError(xhome::ServiceError(status, "", "x"))));
    CHECK_FALSE(raw(DescribeError(std::runtime_error("boom"))));
    CHECK(DescribeError(xhome::ServiceError(401, R"({"XErr": 2148916233})", "x")).find("perfil Xbox") != std::string::npos);
    CHECK_FALSE(raw(DescribeSessionFailure("0x87E10BC6", "sem espaço")));
    const std::string wns = DescribeSessionFailure("WNSError", "Xccs : ErrorCallingWNS : Send command failed : State WaitingForServerToRegister");
    CHECK(wns.find("Suspensão") != std::string::npos);
    CHECK_FALSE(raw(wns));
    CHECK(DescribeSessionFailure("", "console Busy").find("outro aparelho") != std::string::npos);
    CHECK(DescribeEnded("connection lost") == "A conexão com o console caiu.");
    CHECK_FALSE(raw(DescribeEnded("some internal reason")));
    CHECK_FALSE(DescribeSignInFailure(xhome::SignInPoll::Expired, "").empty());
    CHECK(DescribeSignInFailure(xhome::SignInPoll::Pending, "").empty());
}

// ---- i18n: the app's strings -----------------------------------------------------------------------------------------

TEST_CASE("i18n: app.json has the same keys in pt-BR and en-US, and every one is translated") {
    const auto keys = [](const char* lang) {
        std::ifstream in(std::filesystem::path(FLUVI_I18N_DIR) / lang / "app.json");
        const nlohmann::json j = nlohmann::json::parse(in);
        std::vector<std::string> out;
        for (auto it = j.begin(); it != j.end(); ++it) {
            CHECK(it->is_string());
            CHECK(it.key().rfind("app.", 0) == 0);
            out.push_back(it.key());
        }
        return out;
    };
    const auto pt = keys("pt-BR"), en = keys("en-US");
    CHECK(pt == en);
    CHECK(pt.size() > 20);
}

// ---- window geometry ---------------------------------------------------------------------------------------------------

TEST_CASE("window: the default size is 1600x900 dp times the DPI scale, centered, and limited to the work area") {
    const MonitorArea fhd100{{0, 0, 1920, 1040}, 96};
    StartRect r = ComputeStartRect({}, fhd100);
    CHECK(r.width == 1600);
    CHECK(r.height == 900);
    CHECK(r.x == 160);
    CHECK(r.y == 70);
    CHECK_FALSE(r.maximized);

    // 125% on a 1080p laptop: 2000x1125 px do not fit in 1920x1032, so the work area limits it.
    r = ComputeStartRect({}, {{0, 0, 1920, 1032}, 120});
    CHECK(r.width == 1920);
    CHECK(r.height == 1032);
    CHECK(r.x == 0);

    // 4K at 150%: 2400x1350 px.
    r = ComputeStartRect({}, {{0, 0, 3840, 2080}, 144});
    CHECK(r.width == 2400);
    CHECK(r.height == 1350);
}

TEST_CASE("window: the minimum is 1120x630 dp at the monitor DPI") {
    CHECK(MinWindowSize(96).cx == 1120);
    CHECK(MinWindowSize(96).cy == 630);
    CHECK(MinWindowSize(120).cx == 1400);
    CHECK(MinWindowSize(144).cy == 945);
}

TEST_CASE("window: a saved placement is clamped to the work area and the minimum, the maximized flag is kept") {
    const MonitorArea area{{1920, 0, 3840, 1040}, 96};   // a second monitor to the right
    WindowSettings saved{true, 1700, 900, 800, 400, true};   // too small, hangs off the left and bottom
    StartRect r = ComputeStartRect(saved, area);
    CHECK(r.width == 1120);
    CHECK(r.height == 630);
    CHECK(r.x == 1920);                                  // moved onto the monitor
    CHECK(r.y == 1040 - 630);                            // fully above the bottom edge
    CHECK(r.maximized);

    saved = {true, 2000, 100, 5000, 3000, false};        // larger than the monitor
    r = ComputeStartRect(saved, area);
    CHECK(r.width == 1920);
    CHECK(r.height == 1040);
    CHECK(r.x == 1920);
    CHECK(r.y == 0);

    saved = {true, 2500, 120, 1300, 800, false};         // fits: untouched
    r = ComputeStartRect(saved, area);
    CHECK(r.x == 2500);
    CHECK(r.y == 120);
    CHECK(r.width == 1300);
    CHECK(r.height == 800);
}
