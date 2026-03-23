#include "config.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace {

std::string read_file_text(const std::string& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        throw std::runtime_error("failed to open config file: " + path);
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::vector<std::string> read_string_array(const nlohmann::json& json, const char* key) {
    if (!json.contains(key) || !json.at(key).is_array()) {
        throw std::runtime_error(std::string("missing or invalid array field: ") + key);
    }

    std::vector<std::string> values;
    for (const auto& item : json.at(key)) {
        if (!item.is_string()) {
            throw std::runtime_error(std::string("array field contains non-string item: ") + key);
        }
        values.push_back(item.get<std::string>());
    }

    return values;
}

std::string read_string(const nlohmann::json& json, const char* key, const std::string& fallback = "") {
    if (!json.contains(key)) {
        return fallback;
    }
    if (!json.at(key).is_string()) {
        throw std::runtime_error(std::string("invalid string field: ") + key);
    }
    return json.at(key).get<std::string>();
}

}  // namespace

AppConfig load_config(const std::string& path) {
    const auto text = read_file_text(path);
    const auto json = nlohmann::json::parse(text);

    AppConfig config;
    config.bind_address = read_string(json, "bind_address", config.bind_address);

    if (!json.contains("listen_port") || !json.at("listen_port").is_number_integer()) {
        throw std::runtime_error("missing or invalid integer field: listen_port");
    }
    config.listen_port = json.at("listen_port").get<int>();

    config.inbound_models = read_string_array(json, "inbound_models");

    for (const auto& key : read_string_array(json, "inbound_api_keys")) {
        config.inbound_api_keys.insert(key);
    }

    config.outbound_api_url = read_string(json, "outbound_api_url");
    config.outbound_api_key = read_string(json, "outbound_api_key");
    config.log_dir = read_string(json, "log_dir", config.log_dir);

    if (config.outbound_api_url.empty()) {
        throw std::runtime_error("missing required field: outbound_api_url");
    }
    if (config.outbound_api_key.empty()) {
        throw std::runtime_error("missing required field: outbound_api_key");
    }
    if (config.inbound_models.empty()) {
        throw std::runtime_error("inbound_models must not be empty");
    }
    if (config.inbound_api_keys.empty()) {
        throw std::runtime_error("inbound_api_keys must not be empty");
    }

    if (json.contains("upstream_timeout_seconds")) {
        if (!json.at("upstream_timeout_seconds").is_number_integer()) {
            throw std::runtime_error("invalid integer field: upstream_timeout_seconds");
        }
        config.upstream_timeout_seconds = json.at("upstream_timeout_seconds").get<long>();
    }

    if (json.contains("verify_upstream_tls")) {
        if (!json.at("verify_upstream_tls").is_boolean()) {
            throw std::runtime_error("invalid boolean field: verify_upstream_tls");
        }
        config.verify_upstream_tls = json.at("verify_upstream_tls").get<bool>();
    }

    if (json.contains("request_body_log_limit")) {
        if (!json.at("request_body_log_limit").is_number_unsigned()) {
            throw std::runtime_error("invalid unsigned integer field: request_body_log_limit");
        }
        config.request_body_log_limit = json.at("request_body_log_limit").get<std::size_t>();
    }

    if (json.contains("response_body_log_limit")) {
        if (!json.at("response_body_log_limit").is_number_unsigned()) {
            throw std::runtime_error("invalid unsigned integer field: response_body_log_limit");
        }
        config.response_body_log_limit = json.at("response_body_log_limit").get<std::size_t>();
    }

    std::filesystem::create_directories(config.log_dir);
    return config;
}