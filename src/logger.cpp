#include "logger.h"

#include <filesystem>
#include <stdexcept>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

void init_logging(const std::string& log_dir) {
    std::filesystem::create_directories(log_dir);

    static bool initialized = false;
    if (initialized) {
        return;
    }

    constexpr std::size_t queue_size = 8192;
    constexpr std::size_t worker_threads = 1;
    spdlog::init_thread_pool(queue_size, worker_threads);

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::info);

    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        log_dir + "/llm_api_proxy.log", 20 * 1024 * 1024, 10, true);
    file_sink->set_level(spdlog::level::trace);

    std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};
    auto logger = std::make_shared<spdlog::async_logger>(
        "llm_api_proxy",
        sinks.begin(),
        sinks.end(),
        spdlog::thread_pool(),
        spdlog::async_overflow_policy::block);

    logger->set_level(spdlog::level::trace);
    logger->flush_on(spdlog::level::info);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [pid %P] [tid %t] %v");

    spdlog::set_default_logger(logger);
    initialized = true;
}

std::shared_ptr<spdlog::logger> app_logger() {
    auto logger = spdlog::get("llm_api_proxy");
    if (!logger) {
        throw std::runtime_error("logger not initialized");
    }
    return logger;
}