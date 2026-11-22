// SPDX-License-Identifier: GPL-3.0-only
#include "fluvi/core/Log.h"

#include <windows.h>
#include <shlobj.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>

namespace fluvi::log {
namespace {

struct State {
    std::mutex mutex;
    Level minLevel = Level::Info;
    FILE* file = nullptr;
    bool fileResolved = false;
    std::wstring filePath;
};

State& state() {
    static State s;
    return s;
}

std::wstring defaultLogPath() {
    PWSTR base = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        std::filesystem::path dir = std::filesystem::path(base) / L"Fluvi";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        path = (dir / L"fluvi.log").wstring();
    }
    CoTaskMemFree(base);
    return path;
}

void openFileLocked(State& s) {
    if (s.fileResolved) return;
    s.fileResolved = true;
    if (s.filePath.empty()) s.filePath = defaultLogPath();
    if (!s.filePath.empty()) s.file = _wfsopen(s.filePath.c_str(), L"w", _SH_DENYWR);
}

const char* levelName(Level level) {
    switch (level) {
    case Level::Debug: return "DBG";
    case Level::Info: return "INF";
    case Level::Warn: return "WRN";
    case Level::Error: return "ERR";
    }
    return "???";
}

} // namespace

void Write(Level level, std::string_view module, std::string_view message) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (level < s.minLevel) return;
    openFileLocked(s);

    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 86'400'000;
    char stamp[16];
    std::snprintf(stamp, sizeof stamp, "%02lld:%02lld:%02lld.%03lld", ms / 3'600'000, ms / 60'000 % 60, ms / 1000 % 60, ms % 1000);

    const std::string line = std::format("{} {} [{}] {}\n", stamp, levelName(level), module, message);
#ifndef NDEBUG
    std::fputs(line.c_str(), stderr);
#endif
    if (s.file) {
        std::fputs(line.c_str(), s.file);
        std::fflush(s.file);
    }
}

void SetMinLevel(Level level) {
    std::lock_guard lock(state().mutex);
    state().minLevel = level;
}

void SetFile(const std::wstring& path) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (s.file) std::fclose(s.file);
    s.file = nullptr;
    s.filePath = path;
    s.fileResolved = path.empty();   // empty: no file at all
    if (!path.empty()) openFileLocked(s);
}

} // namespace fluvi::log
