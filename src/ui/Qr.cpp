// SPDX-License-Identifier: GPL-3.0-only
#include <exception>
#include <qrcodegen.hpp>
#include "fluvi/core/Log.h"
#include "fluvi/ui/Ui.h"

namespace fluvi::ui {

std::vector<std::vector<bool>> EncodeQr(const std::string& text) {
    std::vector<std::vector<bool>> out;
    if (text.empty()) return out;
    try {
        const auto qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int n = qr.getSize();
        out.assign(static_cast<size_t>(n), std::vector<bool>(static_cast<size_t>(n), false));
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                out[static_cast<size_t>(y)][static_cast<size_t>(x)] = qr.getModule(x, y);
    } catch (const std::exception& e) {
        FLUVI_WARN("ui", "QR encode failed: {}", e.what());
        out.clear();
    }
    return out;
}

} // namespace fluvi::ui
