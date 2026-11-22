// SPDX-License-Identifier: GPL-3.0-only
// Fluvi input: reads the physical controller and reports every change immediately (event-driven, ~1 kHz),
// instead of polling every 16 ms like the browser clients. Owner: agent D (src/input).
// GameInput first (Windows SDK GameInput.h), XInput as fallback.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include "fluvi/common/Gamepad.h"

namespace fluvi::input {

enum class Backend : uint8_t { None, GameInput, XInput };

struct GamepadEvent {
    GamepadState state;
    int64_t qpc = 0;          // fluvi::time::NowQpc() when the change was read
    uint8_t index = 0;        // pad index as the report carries it (0 for the first pad)
};

struct ReaderStats {
    Backend backend = Backend::None;
    bool connected = false;
    double readsPerSecond = 0;     // how often the device is sampled
    double eventsPerSecond = 0;    // changes delivered
    double maxReadToCallbackMs = 0;
};

class GamepadReader {
public:
    // Called on the reader's own thread for every change of state (and once on connect/disconnect with a
    // neutral state). Must return fast; never blocks the reader for long.
    using OnChange = std::function<void(const GamepadEvent&)>;

    static std::unique_ptr<GamepadReader> Create(Backend preferred = Backend::GameInput);
    virtual ~GamepadReader() = default;

    virtual bool Start(OnChange onChange) = 0;
    virtual void Stop() = 0;
    // From the console (xhome). strength 0..100 scales it (ControllerSettings::rumbleStrength).
    virtual void SetRumble(uint8_t index, const Rumble& rumble, uint8_t strength = 100) = 0;
    virtual GamepadState Current(uint8_t index = 0) const = 0;
    virtual ReaderStats Stats() const = 0;
};

} // namespace fluvi::input
