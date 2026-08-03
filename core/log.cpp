#include "core/log.hpp"

#include <memory>
#include <mutex>
#include <vector>

#pragma warning(push)
#pragma warning(disable: 26800 28251)
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#pragma warning(pop)

namespace sam::log {

namespace {

std::once_flag g_init;
std::shared_ptr<spdlog::logger> g_logger;

}  // namespace

void init(const std::filesystem::path& log_dir) {
    std::call_once(g_init, [&] {
        std::error_code ec;
        std::filesystem::create_directories(log_dir, ec);

        auto dist = std::make_shared<spdlog::sinks::dist_sink_mt>();
        dist->add_sink(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            (log_dir / "sam.log").string(),
            1024 * 1024 * 4,
            6));
        dist->add_sink(std::make_shared<spdlog::sinks::msvc_sink_mt>());

        g_logger = std::make_shared<spdlog::logger>("sam", dist);
        g_logger->set_level(spdlog::level::info);
        g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        g_logger->flush_on(spdlog::level::warn);
        spdlog::set_default_logger(g_logger);
    });
}

void shutdown() {
    if (g_logger) {
        g_logger->flush();
    }
    spdlog::shutdown();
}

}  // namespace sam::log
