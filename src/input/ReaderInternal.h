// SPDX-License-Identifier: GPL-3.0-only
// Backend factories, internal to fluvi_input.
#pragma once
#include <memory>
#include "fluvi/input/GamepadReader.h"

namespace fluvi::input {

std::unique_ptr<GamepadReader> MakeGameInputReader();
std::unique_ptr<GamepadReader> MakeXInputReader();

} // namespace fluvi::input
