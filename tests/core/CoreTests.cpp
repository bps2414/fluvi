// SPDX-License-Identifier: GPL-3.0-only
#include <doctest/doctest.h>

#include "fluvi/core/LatestQueue.h"
#include "fluvi/core/Log.h"
#include "fluvi/core/Time.h"

TEST_CASE("LatestQueue drops the oldest item when full") {
    fluvi::LatestQueue<int, 3> q;
    CHECK_FALSE(q.Push(1));
    CHECK_FALSE(q.Push(2));
    CHECK_FALSE(q.Push(3));
    CHECK(q.Push(4));
    CHECK(q.Dropped() == 1);
    CHECK(q.TryPop() == 2);
    CHECK(q.TryPop() == 3);
    CHECK(q.TryPop() == 4);
    CHECK_FALSE(q.TryPop().has_value());
}

TEST_CASE("LatestQueue WaitPop returns after Close") {
    fluvi::LatestQueue<int, 2> q;
    q.Close();
    CHECK_FALSE(q.WaitPop(std::chrono::milliseconds(1000)).has_value());
}

TEST_CASE("QPC conversions round-trip") {
    CHECK(fluvi::time::QpcFrequency() > 0);
    const int64_t a = fluvi::time::NowQpc();
    CHECK(fluvi::time::NowQpc() >= a);
    CHECK(fluvi::time::QpcToMs(fluvi::time::MsToQpc(16.0)) == doctest::Approx(16.0).epsilon(0.001));
}

TEST_CASE("Log writes without a file") {
    fluvi::log::SetFile(L"");
    FLUVI_INFO("test", "value {}", 42);
}
