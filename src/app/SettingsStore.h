// SPDX-License-Identifier: GPL-3.0-only
// Fluvi app: AppSettings as JSON in %LOCALAPPDATA%\Fluvi\settings.json. Never throws: a missing or corrupt file gives
// the defaults, a wrong-typed or out-of-range field gives that field's default, unknown keys are ignored on read and
// kept on write (a newer build's keys survive a round trip through an older one).
#pragma once
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "fluvi/common/Settings.h"

namespace fluvi::app {

// %LOCALAPPDATA%\Fluvi\settings.json; the FLUVI_APP_DATA environment variable replaces the Fluvi folder (tests).
std::filesystem::path SettingsPath();

// Pure JSON <-> AppSettings. `previous` is the JSON text last read or written: its unknown keys are carried over.
std::string SerializeSettings(const AppSettings& settings, std::string_view previous = {});
// `valid` (optional) is false when the text is not a JSON object (the result is then all defaults).
AppSettings ParseSettings(std::string_view json, bool* valid = nullptr);

struct LoadedSettings {
    AppSettings settings;
    std::string raw;        // file text ("" when missing), for SerializeSettings(previous)
    bool existed = false;   // the file was there
    bool valid = false;     // and parsed as a JSON object
};
LoadedSettings LoadSettings(const std::filesystem::path& file);
// Temp file + rename, so a crash never leaves half a file. False (logged) on failure.
bool WriteTextFileAtomic(const std::filesystem::path& file, const std::string& text);

// Debounced writer: Request() from any thread, the file is written once `debounce` passes without another
// request; Flush() and the destructor write immediately.
class SettingsSaver {
public:
    SettingsSaver(std::filesystem::path file, std::string raw, std::chrono::milliseconds debounce = std::chrono::milliseconds(600));
    ~SettingsSaver();
    SettingsSaver(const SettingsSaver&) = delete;
    SettingsSaver& operator=(const SettingsSaver&) = delete;

    void Request(const AppSettings& settings);
    void Flush();
    unsigned Writes() const;   // files written so far (tests)

private:
    void Run();
    void WriteLocked(std::unique_lock<std::mutex>& lock);

    std::filesystem::path file_;
    std::string raw_;
    std::chrono::milliseconds debounce_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    AppSettings pending_;
    bool dirty_ = false, stop_ = false;
    std::chrono::steady_clock::time_point due_;
    unsigned writes_ = 0;
    std::thread thread_;
};

} // namespace fluvi::app
