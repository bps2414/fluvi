// SPDX-License-Identifier: GPL-3.0-only
// Fluvi ui: pure helpers of the system interface and the layout scale, kept free of RmlUi so CPU tests reach them.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace fluvi::ui {

// Replaces every "[key]" in RML text with fluvi::i18n::Tr(key). A key starts with a lower-case letter, holds only
// [a-z0-9_.] and has at least one dot ("[consoles.title]"); anything else in brackets stays as written.
// `replaced` (optional) counts the keys found.
std::string TranslateBrackets(std::string_view text, int* replaced = nullptr);

// Density-independent pixel ratio: the screens are drawn on a 1600x900 dp canvas that grows with the window up to
// 1.5x, never shrinks below 1 dp per logical pixel, and follows the monitor's DPI scale.
float DpRatio(uint32_t width, uint32_t height, float dpiScale);

} // namespace fluvi::ui
