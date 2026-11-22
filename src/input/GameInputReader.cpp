// SPDX-License-Identifier: GPL-3.0-only
// GameInput backend (legacy "v0" API from the Windows SDK GameInput.h, system GameInput.dll). Event-driven: a
// reading callback fires for every report (analogThreshold 0), so changes reach the callback as soon as the
// service has them. GameInput.dll is loaded at run time: no import-library dependency, no redistributable on
// Windows 11 (inbox); where it is missing, Start() fails and the factory falls back to XInput.
#include <windows.h>
#include <GameInput.h>
#include <array>
#include <mutex>
#include "EventSink.h"
#include "GamepadMapping.h"
#include "ReaderInternal.h"
#include "RumbleEngine.h"
#include "fluvi/core/Log.h"

namespace fluvi::input {
namespace {

class GameInputReader final : public GamepadReader {
public:
    ~GameInputReader() override { Stop(); }

    bool Start(OnChange onChange) override {
        if (gi_) return true;
        using CreateFn = HRESULT(STDAPICALLTYPE*)(IGameInput**);
        dll_ = LoadLibraryExW(L"GameInput.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll_) { FLUVI_WARN("input", "GameInput.dll not available (error {})", GetLastError()); return false; }
        auto create = reinterpret_cast<CreateFn>(GetProcAddress(dll_, "GameInputCreate"));
        IGameInput* gi = nullptr;
        if (!create || FAILED(create(&gi)) || !gi) {
            FLUVI_WARN("input", "GameInputCreate failed");
            return false;
        }
        sink_.Reset(std::move(onChange));
        gi_ = gi;
        rumble_ = std::make_unique<RumbleEngine>([this](uint8_t i, const MotorLevels& l) { ApplyRumble(i, l); });

        // Guide button reaches the app only while it is the foreground one; the rest of the input keeps the default.
        gi_->SetFocusPolicy(GameInputExclusiveForegroundGuideButton);

        HRESULT hr = gi_->RegisterDeviceCallback(nullptr, GameInputKindGamepad, GameInputDeviceAnyStatus,
                                                 GameInputAsyncEnumeration, this, &DeviceThunk, &deviceToken_);
        if (SUCCEEDED(hr))
            hr = gi_->RegisterReadingCallback(nullptr, GameInputKindGamepad, 0.0f, this, &ReadingThunk, &readingToken_);
        if (SUCCEEDED(hr))   // optional: without it BtnNexus stays unreported
            gi_->RegisterSystemButtonCallback(nullptr, GameInputSystemButtonGuide, this, &SystemThunk, &systemToken_);
        if (FAILED(hr)) {
            FLUVI_WARN("input", "GameInput callback registration failed (0x{:08x})", static_cast<unsigned>(hr));
            Stop();
            return false;
        }
        FLUVI_INFO("input", "GameInput backend started");
        return true;
    }

    void Stop() override {
        if (gi_) {
            for (GameInputCallbackToken* t : {&systemToken_, &readingToken_, &deviceToken_}) {
                if (*t) { gi_->StopCallback(*t); gi_->UnregisterCallback(*t, 1'000'000); *t = 0; }
            }
        }
        rumble_.reset();   // turns the motors off through ApplyRumble while the devices are still held
        {
            std::lock_guard lock(pad_);
            for (auto& s : slots_) {
                if (s.dev) { s.dev->Release(); s.dev = nullptr; }
                s.guide = false;
            }
            sink_.SetConnectedCount(0);
        }
        if (gi_) { gi_->Release(); gi_ = nullptr; }
    }

    void SetRumble(uint8_t index, const Rumble& rumble, uint8_t strength) override {
        if (rumble_) rumble_->Set(index, rumble, strength);
    }

    GamepadState Current(uint8_t index) const override { return sink_.Current(index); }

    ReaderStats Stats() const override {
        ReaderStats st;
        st.backend = gi_ ? Backend::GameInput : Backend::None;
        const_cast<EventSink&>(sink_).Fill(st);
        return st;
    }

private:
    struct Slot {
        IGameInputDevice* dev = nullptr;
        bool guide = false;
    };

    // pad_ held. Returns the slot index of dev, adding it (lowest free index, neutral connect event) if new.
    int FindOrAdd(IGameInputDevice* dev, int64_t qpc) {
        int free = -1;
        for (int i = 0; i < EventSink::kMaxPads; ++i) {
            if (slots_[i].dev == dev) return i;
            if (!slots_[i].dev && free < 0) free = i;
        }
        if (free < 0) return -1;
        dev->AddRef();
        slots_[free] = {dev, false};
        sink_.SetConnectedCount(CountLocked());
        FLUVI_INFO("input", "pad {} connected (GameInput)", free);
        sink_.Submit(static_cast<uint8_t>(free), {}, qpc, true);
        return free;
    }

    int CountLocked() const {
        int n = 0;
        for (const auto& s : slots_) n += s.dev != nullptr;
        return n;
    }

    void OnDevice(IGameInputDevice* dev, GameInputDeviceStatus current) {
        const int64_t qpc = fluvi::time::NowQpc();
        std::lock_guard lock(pad_);
        if (current & GameInputDeviceConnected) {
            FindOrAdd(dev, qpc);
            return;
        }
        for (int i = 0; i < EventSink::kMaxPads; ++i) {
            if (slots_[i].dev != dev) continue;
            slots_[i].dev->Release();
            slots_[i] = {};
            sink_.SetConnectedCount(CountLocked());
            FLUVI_INFO("input", "pad {} disconnected (GameInput)", i);
            sink_.Submit(static_cast<uint8_t>(i), {}, qpc, true);
            if (rumble_) rumble_->Stop(static_cast<uint8_t>(i));
            return;
        }
    }

    void OnReading(IGameInputReading* reading) {
        const int64_t now = fluvi::time::NowQpc();
        sink_.NoteRead();
        IGameInputDevice* dev = nullptr;
        reading->GetDevice(&dev);
        GameInputGamepadState gs{};
        if (!dev || !reading->GetGamepadState(&gs)) { if (dev) dev->Release(); return; }

        // Report age: GameInput timestamps are microseconds on its own clock.
        int64_t readQpc = now;
        const uint64_t cur = gi_->GetCurrentTimestamp(), ts = reading->GetTimestamp();
        if (cur >= ts && cur - ts < 1'000'000) readQpc = now - fluvi::time::MsToQpc(double(cur - ts) / 1000.0);

        {
            std::lock_guard lock(pad_);
            const int idx = FindOrAdd(dev, readQpc);
            if (idx >= 0) {
                RawGameInputPad raw;
                raw.buttons = static_cast<uint32_t>(gs.buttons);
                raw.leftTrigger = gs.leftTrigger;
                raw.rightTrigger = gs.rightTrigger;
                raw.leftX = gs.leftThumbstickX;
                raw.leftY = gs.leftThumbstickY;
                raw.rightX = gs.rightThumbstickX;
                raw.rightY = gs.rightThumbstickY;
                raw.guide = slots_[idx].guide;
                sink_.Submit(static_cast<uint8_t>(idx), MapGameInput(raw), readQpc);
            }
        }
        dev->Release();
    }

    void OnSystemButton(IGameInputDevice* dev, GameInputSystemButtons current) {
        const int64_t now = fluvi::time::NowQpc();
        std::lock_guard lock(pad_);
        const int idx = FindOrAdd(dev, now);
        if (idx < 0) return;
        const bool down = (current & GameInputSystemButtonGuide) != 0;
        slots_[idx].guide = down;
        GamepadState s = sink_.Current(static_cast<uint8_t>(idx));
        s.buttons = static_cast<uint16_t>(down ? (s.buttons | BtnNexus) : (s.buttons & ~BtnNexus));
        sink_.NoteRead();
        sink_.Submit(static_cast<uint8_t>(idx), s, now);
    }

    void ApplyRumble(uint8_t index, const MotorLevels& l) {
        IGameInputDevice* dev = nullptr;
        {
            std::lock_guard lock(pad_);
            if (index < EventSink::kMaxPads && slots_[index].dev) { dev = slots_[index].dev; dev->AddRef(); }
        }
        if (!dev) return;
        GameInputRumbleParams p{l.lowFrequency, l.highFrequency, l.leftTrigger, l.rightTrigger};
        dev->SetRumbleState(&p);
        dev->Release();
    }

    static void CALLBACK DeviceThunk(GameInputCallbackToken, void* ctx, IGameInputDevice* dev, uint64_t,
                                     GameInputDeviceStatus current, GameInputDeviceStatus) {
        static_cast<GameInputReader*>(ctx)->OnDevice(dev, current);
    }
    static void CALLBACK ReadingThunk(GameInputCallbackToken, void* ctx, IGameInputReading* reading, bool) {
        static_cast<GameInputReader*>(ctx)->OnReading(reading);
    }
    static void CALLBACK SystemThunk(GameInputCallbackToken, void* ctx, IGameInputDevice* dev, uint64_t,
                                     GameInputSystemButtons current, GameInputSystemButtons) {
        static_cast<GameInputReader*>(ctx)->OnSystemButton(dev, current);
    }

    HMODULE dll_ = nullptr;
    IGameInput* gi_ = nullptr;
    GameInputCallbackToken deviceToken_ = 0, readingToken_ = 0, systemToken_ = 0;
    std::mutex pad_;                         // serialises device/reading/system callbacks and slot changes
    std::array<Slot, EventSink::kMaxPads> slots_{};
    EventSink sink_;
    std::unique_ptr<RumbleEngine> rumble_;
};

} // namespace

std::unique_ptr<GamepadReader> MakeGameInputReader() { return std::make_unique<GameInputReader>(); }

} // namespace fluvi::input
