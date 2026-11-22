// SPDX-License-Identifier: GPL-3.0-only
// Ported from Veyra (GPL-3.0-only), include/veyra/xbox/DisconnectPolicy.h @ c6633121. Changes: namespace, progress measured in QPC time.
#pragma once
#include <cstdint>
#include <string_view>

#include "fluvi/core/Time.h"

namespace fluvi::xhome {

// Only explicitly recoverable transport/service losses. A takeover, sign-out,
// console shutdown or an unknown server kick must stay visible to the user.
inline bool reconnectableDisconnect(std::string_view reason) {
    return reason == "connection lost" || reason == "KickForServerShutdown" || reason == "session expired";
}

class ReconnectBudget {
public:
    bool take() { if (attempts_ >= 3) return false; ++attempts_; return true; }
    unsigned attempts() const { return attempts_; }
    // Refill only after 30 seconds of continuous decoded progress. One frame
    // after a reconnect must not enable another endless cycle of retries.
    // `qpc` is a fluvi::time::NowQpc() value; a gap over 1 s restarts the run.
    void decoded(int64_t qpc) {
        if (!progressStart_ || qpc < lastProgress_ || time::ElapsedMs(lastProgress_, qpc) > 1000.0) progressStart_ = qpc;
        lastProgress_ = qpc;
        if (time::ElapsedMs(progressStart_, qpc) >= 30000.0) attempts_ = 0;
    }
    void disconnected() { progressStart_ = lastProgress_ = 0; }
private:
    unsigned attempts_ = 0;
    int64_t progressStart_ = 0, lastProgress_ = 0;
};

} // namespace fluvi::xhome
