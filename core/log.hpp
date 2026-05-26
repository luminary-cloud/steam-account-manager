#pragma once

#include <filesystem>
#include <string>

#include <spdlog/spdlog.h>

namespace sam::log {

// Initialises spdlog with a rotating file sink and a Windows debugger sink.
// `log_dir` is created if missing. Safe to call multiple times.
void init(const std::filesystem::path& log_dir);

// Flushes and removes the global sinks. Call once at shutdown.
void shutdown();

}  // namespace sam::log

#define SAM_LOG_TRACE(...) ::spdlog::trace(__VA_ARGS__)
#define SAM_LOG_DEBUG(...) ::spdlog::debug(__VA_ARGS__)
#define SAM_LOG_INFO(...)  ::spdlog::info(__VA_ARGS__)
#define SAM_LOG_WARN(...)  ::spdlog::warn(__VA_ARGS__)
#define SAM_LOG_ERROR(...) ::spdlog::error(__VA_ARGS__)
