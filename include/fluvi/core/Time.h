// SPDX-License-Identifier: GPL-3.0-only
// Fluvi: one clock for the whole app, QueryPerformanceCounter ticks. Every timestamp that crosses a module
// boundary (arrival, decode, present, input) is a QPC value from NowQpc().
#pragma once
#include <cstdint>

namespace fluvi::time {

int64_t NowQpc();
int64_t QpcFrequency();                       // ticks per second, cached
double QpcToMs(int64_t ticks);                // a duration in ticks -> ms
int64_t MsToQpc(double ms);
inline double ElapsedMs(int64_t fromQpc, int64_t toQpc) { return QpcToMs(toQpc - fromQpc); }

} // namespace fluvi::time
