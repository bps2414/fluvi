// SPDX-License-Identifier: GPL-3.0-only
#include "fluvi/core/Time.h"

#include <windows.h>

namespace fluvi::time {

int64_t QpcFrequency() {
    static const int64_t frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return frequency;
}

int64_t NowQpc() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

double QpcToMs(int64_t ticks) { return double(ticks) * 1000.0 / double(QpcFrequency()); }

int64_t MsToQpc(double ms) { return int64_t(ms * double(QpcFrequency()) / 1000.0); }

} // namespace fluvi::time
