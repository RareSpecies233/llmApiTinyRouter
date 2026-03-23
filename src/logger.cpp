#include "logger.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace {

std::string make_log_file_path(const std::string& log_dir) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t current_time = std::chrono::system_clock::to_time_t(now);

    std::tm local_time{};
#if defined(_WIN32)
    localtime_s(&local_time, &current_time);
#else
    localtime_r(&current_time, &local_time);
#endif

    std::ostringstream name;
    name << "llmapi_log_" << std::put_time(&local_time, "%Y%m%d_%H%M%S") << ".txt";
    return (std::filesystem::path(log_dir) / name.str()).string();
}

}  // namespace

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

    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(make_log_file_path(log_dir), true);
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