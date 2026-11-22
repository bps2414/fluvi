// SPDX-License-Identifier: GPL-3.0-only
// fluvi_pad_probe: manual tool. Prints the backend, every pad event with a timestamp, and the measured read /
// event rates once a second. Keys: R = rumble test, Q = quit.
//   fluvi_pad_probe [--xinput] [--seconds N] [--quiet]
#include <windows.h>
#include <conio.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include "fluvi/core/Time.h"
#include "fluvi/input/GamepadReader.h"

using namespace fluvi;
using namespace fluvi::input;

static const char* BackendName(Backend b) {
    switch (b) {
        case Backend::GameInput: return "GameInput";
        case Backend::XInput: return "XInput";
        default: return "none";
    }
}

int main(int argc, char** argv) {
    Backend preferred = Backend::GameInput;
    double seconds = 0;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--xinput")) preferred = Backend::XInput;
        else if (!std::strcmp(argv[i], "--quiet")) quiet = true;
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
    }

    auto reader = GamepadReader::Create(preferred);
    std::mutex mutex;
    std::deque<GamepadEvent> queue;     // the reader's callback only enqueues; printing happens on the main thread
    const int64_t t0 = time::NowQpc();
    if (!reader->Start([&](const GamepadEvent& e) {
            std::lock_guard lock(mutex);
            if (queue.size() < 4096) queue.push_back(e);
        })) {
        std::printf("could not start any backend\n");
        return 1;
    }
    std::printf("backend: %s | R = rumble test, Q = quit\n", BackendName(reader->Stats().backend));

    uint64_t total = 0;
    int64_t lastStats = time::NowQpc();
    for (;;) {
        std::deque<GamepadEvent> batch;
        {
            std::lock_guard lock(mutex);
            batch.swap(queue);
        }
        for (const auto& e : batch) {
            ++total;
            if (quiet) continue;
            const auto& s = e.state;
            std::printf("[%10.3f ms] pad%u btn=0x%04x L(%6d,%6d) R(%6d,%6d) LT=%5u RT=%5u\n", time::ElapsedMs(t0, e.qpc),
                        e.index, s.buttons, s.leftX, s.leftY, s.rightX, s.rightY, s.leftTrigger, s.rightTrigger);
        }
        const int64_t now = time::NowQpc();
        if (time::ElapsedMs(lastStats, now) >= 1000) {
            lastStats = now;
            const ReaderStats st = reader->Stats();
            std::printf("-- %s connected=%d reads/s=%.0f events/s=%.0f maxReadToCallback=%.2f ms (events total %llu)\n",
                        BackendName(st.backend), st.connected, st.readsPerSecond, st.eventsPerSecond,
                        st.maxReadToCallbackMs, static_cast<unsigned long long>(total));
        }
        if (_kbhit()) {
            const int c = _getch();
            if (c == 'q' || c == 'Q') break;
            if (c == 'r' || c == 'R') {
                Rumble r;
                r.leftMotor = 100; r.rightMotor = 100; r.leftTrigger = 60; r.rightTrigger = 60;
                r.durationMs = 200; r.delayMs = 150; r.repeat = 2;
                reader->SetRumble(0, r, 100);
                std::printf("rumble: 3 pulses of 200 ms\n");
            }
        }
        if (seconds > 0 && time::ElapsedMs(t0, now) >= seconds * 1000.0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    reader->Stop();
    return 0;
}
