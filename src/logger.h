#pragma once

#include <memory>
#include <string>

namespace spdlog {
class logger;
}

void init_logging(const std::string& log_dir);
std::shared_ptr<spdlog::logger> app_logger();