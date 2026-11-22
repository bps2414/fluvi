// SPDX-License-Identifier: GPL-3.0-only
#include "fluvi/input/GamepadReader.h"
#include "ReaderInternal.h"
#include "fluvi/core/Log.h"

namespace fluvi::input {
namespace {

// Tries GameInput first; if it cannot start (DLL missing, service down) the XInput reader takes over.
class AutoReader final : public GamepadReader {
public:
    ~AutoReader() override { Stop(); }

    bool Start(OnChange onChange) override {
        if (active_) return true;
        auto gi = MakeGameInputReader();
        if (gi->Start(onChange)) { active_ = std::move(gi); return true; }
        FLUVI_WARN("input", "GameInput unavailable, falling back to XInput");
        auto xi = MakeXInputReader();
        if (!xi->Start(std::move(onChange))) return false;
        active_ = std::move(xi);
        return true;
    }
    void Stop() override { if (active_) { active_->Stop(); active_.reset(); } }
    void SetRumble(uint8_t index, const Rumble& r, uint8_t strength) override {
        if (active_) active_->SetRumble(index, r, strength);
    }
    GamepadState Current(uint8_t index) const override { return active_ ? active_->Current(index) : GamepadState{}; }
    ReaderStats Stats() const override { return active_ ? active_->Stats() : ReaderStats{}; }

private:
    std::unique_ptr<GamepadReader> active_;
};

} // namespace

std::unique_ptr<GamepadReader> GamepadReader::Create(Backend preferred) {
    switch (preferred) {
        case Backend::GameInput: return std::make_unique<AutoReader>();
        case Backend::XInput: return MakeXInputReader();
        case Backend::None: break;
    }
    return nullptr;
}

} // namespace fluvi::input
