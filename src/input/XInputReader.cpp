// SPDX-License-Identifier: GPL-3.0-only
// XInput fallback: one thread polls the connected pads at 1 kHz on a high-resolution waitable timer, with the
// MMCSS "Games" class. xinput1_4.dll is loaded at run time so the undocumented XInputGetStateEx (ordinal 100,
// reports the Guide button) can be used when present.
#include <windows.h>
#include <avrt.h>
#include <xinput.h>
#include <array>
#include <atomic>
#include <thread>
#include "EventSink.h"
#include "GamepadMapping.h"
#include "ReaderInternal.h"
#include "RumbleEngine.h"
#include "fluvi/core/Log.h"

namespace fluvi::input {
namespace {

constexpr int kSlots = 4;                 // XInput user indices
constexpr double kScanIntervalMs = 500;   // probing an empty slot is slow, so it is done rarely

class XInputReader final : public GamepadReader {
public:
    ~XInputReader() override { Stop(); }

    bool Start(OnChange onChange) override {
        if (running_) return true;
        dll_ = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll_) { FLUVI_ERROR("input", "xinput1_4.dll not available (error {})", GetLastError()); return false; }
        getStateEx_ = reinterpret_cast<GetStateFn>(GetProcAddress(dll_, MAKEINTRESOURCEA(100)));
        if (!getStateEx_) getStateEx_ = reinterpret_cast<GetStateFn>(GetProcAddress(dll_, "XInputGetState"));
        setState_ = reinterpret_cast<SetStateFn>(GetProcAddress(dll_, "XInputSetState"));
        if (!getStateEx_) return false;

        sink_.Reset(std::move(onChange));
        for (auto& i : indexOfSlot_) i.store(-1);
        rumble_ = std::make_unique<RumbleEngine>([this](uint8_t i, const MotorLevels& l) { ApplyRumble(i, l); });
        quit_ = false;
        running_ = true;
        thread_ = std::thread([this] { Run(); });
        FLUVI_INFO("input", "XInput backend started (1 kHz polling)");
        return true;
    }

    void Stop() override {
        if (!running_) return;
        quit_ = true;
        if (thread_.joinable()) thread_.join();
        rumble_.reset();
        running_ = false;
        sink_.SetConnectedCount(0);
    }

    void SetRumble(uint8_t index, const Rumble& rumble, uint8_t strength) override {
        if (rumble_) rumble_->Set(index, rumble, strength);
    }

    GamepadState Current(uint8_t index) const override { return sink_.Current(index); }

    ReaderStats Stats() const override {
        ReaderStats st;
        st.backend = running_ ? Backend::XInput : Backend::None;
        const_cast<EventSink&>(sink_).Fill(st);
        return st;
    }

private:
    using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    using SetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);

    void ApplyRumble(uint8_t index, const MotorLevels& l) {
        if (!setState_) return;
        for (int slot = 0; slot < kSlots; ++slot) {
            if (indexOfSlot_[slot].load() != index) continue;
            XINPUT_VIBRATION v{};   // XInput has the two motors only; trigger motors are ignored
            v.wLeftMotorSpeed = static_cast<WORD>(l.lowFrequency * 65535.0f + 0.5f);
            v.wRightMotorSpeed = static_cast<WORD>(l.highFrequency * 65535.0f + 0.5f);
            setState_(static_cast<DWORD>(slot), &v);
        }
    }

    static RawXInputPad ToRaw(const XINPUT_GAMEPAD& g) {
        RawXInputPad r;
        r.buttons = g.wButtons;
        r.leftTrigger = g.bLeftTrigger;
        r.rightTrigger = g.bRightTrigger;
        r.leftX = g.sThumbLX;
        r.leftY = g.sThumbLY;
        r.rightX = g.sThumbRX;
        r.rightY = g.sThumbRY;
        return r;
    }

    void Run() {
        DWORD mmcssTask = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Games", &mmcssTask);
        if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        bool highRes = timer != nullptr;
        if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        if (!highRes) FLUVI_WARN("input", "no high-resolution timer; XInput polling will be coarser than 1 kHz");
        LARGE_INTEGER due;
        due.QuadPart = -10000;   // 1 ms, then periodic
        if (timer) SetWaitableTimer(timer, &due, 1, nullptr, nullptr, FALSE);

        std::array<bool, kSlots> connected{};
        std::array<DWORD, kSlots> lastPacket{};
        std::array<double, kSlots> nextScanMs{};
        auto nowMs = [] { return fluvi::time::QpcToMs(fluvi::time::NowQpc()); };

        while (!quit_) {
            if (timer) WaitForSingleObject(timer, 100); else Sleep(1);
            if (quit_) break;
            const double now = nowMs();
            for (int slot = 0; slot < kSlots; ++slot) {
                if (!connected[slot] && now < nextScanMs[slot]) continue;
                const int64_t readQpc = fluvi::time::NowQpc();
                XINPUT_STATE xs{};
                const DWORD rc = getStateEx_(static_cast<DWORD>(slot), &xs);
                if (rc != ERROR_SUCCESS) {
                    if (connected[slot]) Disconnect(slot, connected, readQpc);
                    nextScanMs[slot] = now + kScanIntervalMs + slot * 40.0;   // stagger the empty-slot probes
                    continue;
                }
                sink_.NoteRead();
                if (!connected[slot]) {
                    connected[slot] = true;
                    lastPacket[slot] = xs.dwPacketNumber - 1;
                    int idx = 0;                      // lowest free pad index
                    for (;; ++idx) {
                        bool used = false;
                        for (int s = 0; s < kSlots; ++s) used |= indexOfSlot_[s].load() == idx;
                        if (!used) break;
                    }
                    indexOfSlot_[slot].store(idx);
                    sink_.SetConnectedCount(Count(connected));
                    FLUVI_INFO("input", "pad {} connected (XInput slot {})", idx, slot);
                    sink_.Submit(static_cast<uint8_t>(idx), {}, readQpc, true);
                }
                if (xs.dwPacketNumber == lastPacket[slot]) continue;   // nothing new from the pad
                lastPacket[slot] = xs.dwPacketNumber;
                sink_.Submit(static_cast<uint8_t>(indexOfSlot_[slot].load()), MapXInput(ToRaw(xs.Gamepad)), readQpc);
            }
        }
        if (timer) CloseHandle(timer);
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    }

    static int Count(const std::array<bool, kSlots>& c) {
        int n = 0;
        for (bool b : c) n += b;
        return n;
    }

    void Disconnect(int slot, std::array<bool, kSlots>& connected, int64_t qpc) {
        connected[slot] = false;
        const int idx = indexOfSlot_[slot].exchange(-1);
        sink_.SetConnectedCount(Count(connected));
        FLUVI_INFO("input", "pad {} disconnected (XInput slot {})", idx, slot);
        if (idx >= 0) {
            if (rumble_) rumble_->Stop(static_cast<uint8_t>(idx));
            sink_.Submit(static_cast<uint8_t>(idx), {}, qpc, true);
        }
    }

    HMODULE dll_ = nullptr;
    GetStateFn getStateEx_ = nullptr;
    SetStateFn setState_ = nullptr;
    std::array<std::atomic<int>, kSlots> indexOfSlot_{};
    std::atomic<bool> quit_{false};
    bool running_ = false;
    std::thread thread_;
    EventSink sink_;
    std::unique_ptr<RumbleEngine> rumble_;
};

} // namespace

std::unique_ptr<GamepadReader> MakeXInputReader() { return std::make_unique<XInputReader>(); }

} // namespace fluvi::input
