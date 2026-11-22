// SPDX-License-Identifier: GPL-3.0-only
// Fluvi: result of an operation that can fail. No exceptions cross module boundaries.
#pragma once
#include <string>
#include <utility>

namespace fluvi {

struct Status {
    bool ok = true;
    std::string message;   // human-readable, Portuguese or English; empty when ok

    static Status Ok() { return {}; }
    static Status Fail(std::string why) { return {false, std::move(why)}; }
    explicit operator bool() const { return ok; }
};

} // namespace fluvi
