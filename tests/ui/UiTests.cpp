// SPDX-License-Identifier: GPL-3.0-only
// ui without a GPU: QR encoding for the sign-in screen.
#include <doctest/doctest.h>

#include "fluvi/ui/Ui.h"

using namespace fluvi::ui;

TEST_CASE("QR encodes the verification URI as a square matrix with finder patterns") {
    const auto qr = EncodeQr("https://www.microsoft.com/link");
    REQUIRE(qr.size() >= 21);
    CHECK((qr.size() - 17) % 4 == 0);   // version v is 17 + 4v modules wide
    for (const auto& row : qr) CHECK(row.size() == qr.size());
    // Finder pattern corners: dark outer ring, light inner ring, dark 3x3 core (top-left, top-right, bottom-left).
    const size_t n = qr.size();
    for (auto [ox, oy] : {std::pair<size_t, size_t>{0, 0}, {n - 7, 0}, {0, n - 7}}) {
        CHECK(qr[oy][ox]);
        CHECK(qr[oy][ox + 6]);
        CHECK(qr[oy + 6][ox]);
        CHECK_FALSE(qr[oy + 1][ox + 1]);
        CHECK(qr[oy + 3][ox + 3]);
    }
}

TEST_CASE("QR of empty text is empty") {
    CHECK(EncodeQr("").empty());
}

TEST_CASE("longer text needs a bigger symbol") {
    const auto little = EncodeQr("https://www.microsoft.com/link");
    const auto big = EncodeQr(std::string(200, 'x'));
    CHECK(big.size() > little.size());
}
