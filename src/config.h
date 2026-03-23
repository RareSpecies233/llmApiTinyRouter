#pragma once

#include <string>
#include <unordered_set>
#include <vector>

struct AppConfig {
    std::string bind_address = "0.0.0.0";
    int listen_port = 8080;
    std::vector<std::string> inbound_models;
    std::unordered_set<std::string> inbound_api_keys;
    std::string outbound_api_url;
    std::string outbound_api_key;
    std::string log_dir = "logs";
    long upstream_timeout_seconds = 300;
    bool verify_upstream_tls = true;
    std::size_t request_body_log_limit = 32768;
    std::size_t response_body_log_limit = 32768;
};

AppConfig load_config(const std::string& path);