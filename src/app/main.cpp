// SPDX-License-Identifier: GPL-3.0-only
// Fluvi: native Xbox home-streaming client. Wave 0 stub; wave 2 wires window, session, decode and present.
#include <windows.h>

#include "fluvi/core/Log.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    FLUVI_INFO("app", "Fluvi {} (stub)", "0.1.0");
    MessageBoxW(nullptr, L"Fluvi 0.1.0: base pronta, ainda sem stream.", L"Fluvi", MB_OK | MB_ICONINFORMATION);
    return 0;
}
