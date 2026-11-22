// SPDX-License-Identifier: GPL-3.0-only
// Fluvi: thread-safe logger. Writes to stderr (debug builds) and %LOCALAPPDATA%\Fluvi\fluvi.log.
//   FLUVI_INFO("gpu", "device {} created", name);
#pragma once
#include <format>
#include <string>
#include <string_view>

namespace fluvi::log {

enum class Level { Debug, Info, Warn, Error };

void Write(Level level, std::string_view module, std::string_view message);
void SetMinLevel(Level level);
// Redirects the log file (tests). Empty path: no file.
void SetFile(const std::wstring& path);

} // namespace fluvi::log

#define FLUVI_LOG(level, module, ...) ::fluvi::log::Write(level, module, std::format(__VA_ARGS__))
#define FLUVI_DEBUG(module, ...) FLUVI_LOG(::fluvi::log::Level::Debug, module, __VA_ARGS__)
#define FLUVI_INFO(module, ...) FLUVI_LOG(::fluvi::log::Level::Info, module, __VA_ARGS__)
#define FLUVI_WARN(module, ...) FLUVI_LOG(::fluvi::log::Level::Warn, module, __VA_ARGS__)
#define FLUVI_ERROR(module, ...) FLUVI_LOG(::fluvi::log::Level::Error, module, __VA_ARGS__)
