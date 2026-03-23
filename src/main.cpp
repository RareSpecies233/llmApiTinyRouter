#include "config.h"
#include "logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include <curl/curl.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace {

struct UrlParts {
    std::string scheme;
    std::string host;
    int port = 0;
    std::string base_path;
};

struct UpstreamResponse {
    long status_code = 0;
    std::string body;
    std::multimap<std::string, std::string> headers;
    std::string content_type;
};

struct PreparedRequest {
    std::string outbound_body;
    std::optional<std::string> inbound_model;
    std::optional<std::string> upstream_model;
};

struct CurlHeaders {
    curl_slist* list = nullptr;

    ~CurlHeaders() {
        if (list != nullptr) {
            curl_slist_free_all(list);
        }
    }

    void append(const std::string& value) {
        list = curl_slist_append(list, value.c_str());
    }
};

struct CurlHandle {
    CURL* handle = nullptr;

    CurlHandle() : handle(curl_easy_init()) {}

    ~CurlHandle() {
        if (handle != nullptr) {
            curl_easy_cleanup(handle);
        }
    }
};

class CurlGlobal {
public:
    CurlGlobal() {
        const auto code = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (code != CURLE_OK) {
            throw std::runtime_error(std::string("curl_global_init failed: ") + curl_easy_strerror(code));
        }
    }

    ~CurlGlobal() {
        curl_global_cleanup();
    }
};

std::atomic<std::uint64_t> g_request_counter{0};

std::string trim_copy(const std::string& value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(start, end - start);
}

std::string to_lower_copy(std::string value) {
    for (auto& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string mask_secret(const std::string& value) {
    if (value.empty()) {
        return "<empty>";
    }
    if (value.size() <= 8) {
        return std::string(value.size(), '*');
    }
    return value.substr(0, 4) + std::string(value.size() - 8, '*') + value.substr(value.size() - 4);
}

std::string body_preview(const std::string& body, std::size_t limit) {
    if (body.size() <= limit) {
        return body;
    }
    return body.substr(0, limit) + "\n...<truncated, total=" + std::to_string(body.size()) + " bytes>";
}

std::string url_encode(const std::string& value) {
    std::ostringstream output;
    output << std::uppercase << std::hex;

    for (const unsigned char ch : value) {
        if (std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            output << static_cast<char>(ch);
        } else {
            output << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }

    return output.str();
}

std::string headers_to_string(const httplib::Headers& headers) {
    std::ostringstream output;
    bool first = true;
    for (const auto& [key, value] : headers) {
        if (!first) {
            output << "; ";
        }
        first = false;

        const auto lower_key = to_lower_copy(key);
        if (lower_key == "authorization" || lower_key == "api-key" || lower_key == "x-api-key") {
            output << key << '=' << mask_secret(value);
        } else {
            output << key << '=' << value;
        }
    }
    return output.str();
}

std::string headers_to_string(const std::multimap<std::string, std::string>& headers) {
    std::ostringstream output;
    bool first = true;
    for (const auto& [key, value] : headers) {
        if (!first) {
            output << "; ";
        }
        first = false;

        const auto lower_key = to_lower_copy(key);
        if (lower_key == "authorization" || lower_key == "api-key" || lower_key == "x-api-key") {
            output << key << '=' << mask_secret(value);
        } else {
            output << key << '=' << value;
        }
    }
    return output.str();
}

void add_common_headers(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Headers", "Authorization, Content-Type, X-Request-Id");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
    res.set_header("X-Proxy-By", "llm_api_proxy");
}

std::optional<std::string> extract_bearer_token(const httplib::Request& req) {
    const auto header = req.get_header_value("Authorization");
    if (header.empty()) {
        return std::nullopt;
    }

    const std::string prefix = "Bearer ";
    if (header.rfind(prefix, 0) != 0) {
        return std::nullopt;
    }

    return trim_copy(header.substr(prefix.size()));
}

bool requires_body(const std::string& method) {
    return method == "POST" || method == "PUT" || method == "PATCH";
}

std::string make_request_id() {
    const auto id = ++g_request_counter;
    std::ostringstream output;
    output << "req-" << std::setw(10) << std::setfill('0') << id;
    return output.str();
}

UrlParts parse_url(const std::string& url) {
    static const std::regex pattern(R"(^([a-zA-Z][a-zA-Z0-9+\-.]*)://([^/:]+)(?::(\d+))?(.*)$)");
    std::smatch match;
    if (!std::regex_match(url, match, pattern)) {
        throw std::runtime_error("invalid outbound_api_url: " + url);
    }

    UrlParts parts;
    parts.scheme = to_lower_copy(match[1].str());
    parts.host = match[2].str();
    parts.port = match[3].matched ? std::stoi(match[3].str()) : (parts.scheme == "https" ? 443 : 80);
    parts.base_path = match[4].matched && !match[4].str().empty() ? match[4].str() : "";
    return parts;
}

std::string join_paths(const std::string& base_path, const std::string& request_path) {
    if (base_path.empty()) {
        return request_path.empty() ? "/" : request_path;
    }

    if (request_path.empty() || request_path == "/") {
        return base_path;
    }

    if (base_path.back() == '/' && request_path.front() == '/') {
        return base_path.substr(0, base_path.size() - 1) + request_path;
    }
    if (base_path.back() != '/' && request_path.front() != '/') {
        return base_path + '/' + request_path;
    }
    return base_path + request_path;
}

bool path_has_v1_prefix(const std::string& path) {
    return path == "/v1" || path.rfind("/v1/", 0) == 0;
}

bool base_path_has_v1_suffix(const std::string& path) {
    return path.size() >= 3 && path.compare(path.size() - 3, 3, "/v1") == 0;
}

std::string normalize_openai_path(const std::string& base_path, const std::string& request_path) {
    if (request_path.empty()) {
        return "/";
    }

    if (path_has_v1_prefix(request_path) && base_path_has_v1_suffix(base_path)) {
        const auto normalized = request_path.substr(3);
        return normalized.empty() ? "/" : normalized;
    }

    if (!path_has_v1_prefix(request_path) && !base_path_has_v1_suffix(base_path) && request_path != "/") {
        return "/v1" + request_path;
    }

    return request_path;
}

size_t write_body_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buffer = static_cast<std::string*>(userdata);
    buffer->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t write_header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    const auto total = size * nitems;
    auto* response = static_cast<UpstreamResponse*>(userdata);

    std::string header_line(buffer, total);
    const auto separator = header_line.find(':');
    if (separator == std::string::npos) {
        return total;
    }

    const auto key = trim_copy(header_line.substr(0, separator));
    const auto value = trim_copy(header_line.substr(separator + 1));
    if (!key.empty()) {
        response->headers.emplace(key, value);
        if (to_lower_copy(key) == "content-type") {
            response->content_type = value;
        }
    }

    return total;
}

std::string append_query_string(const httplib::Request& req, const std::string& path) {
    if (req.params.empty()) {
        return path;
    }

    std::ostringstream output;
    output << path << '?';

    bool first = true;
    for (const auto& [key, value] : req.params) {
        if (!first) {
            output << '&';
        }
        first = false;
        output << url_encode(key) << '=' << url_encode(value);
    }

    return output.str();
}

std::string query_string_only(const httplib::Request& req) {
    const auto query = append_query_string(req, "");
    if (query.empty()) {
        return "";
    }
    return query.front() == '?' ? query.substr(1) : query;
}

std::string build_upstream_url(const UrlParts& url_parts, const httplib::Request& req) {
    const auto normalized_path = normalize_openai_path(url_parts.base_path, req.path);
    const auto path = join_paths(url_parts.base_path, normalized_path);
    return url_parts.scheme + "://" + url_parts.host + ':' + std::to_string(url_parts.port) + append_query_string(req, path);
}

bool should_forward_header(const std::string& key) {
    static const std::unordered_set<std::string> blocked_headers = {
        "authorization",
        "host",
        "content-length",
        "connection",
        "accept-encoding"
    };
    return blocked_headers.find(to_lower_copy(key)) == blocked_headers.end();
}

bool should_return_header(const std::string& key) {
    static const std::unordered_set<std::string> blocked_headers = {
        "content-length",
        "connection",
        "transfer-encoding",
        "keep-alive"
    };
    return blocked_headers.find(to_lower_copy(key)) == blocked_headers.end();
}

std::string resolve_upstream_model_name(const AppConfig& config, const std::string& inbound_model) {
    const auto it = config.model_mappings.find(inbound_model);
    if (it == config.model_mappings.end()) {
        return inbound_model;
    }
    return it->second;
}

PreparedRequest prepare_request_for_upstream(const AppConfig& config, const httplib::Request& req) {
    PreparedRequest prepared{req.body, std::nullopt, std::nullopt};

    if (!requires_body(req.method) || req.body.empty()) {
        return prepared;
    }

    const auto content_type = to_lower_copy(req.get_header_value("Content-Type"));
    if (content_type.find("application/json") == std::string::npos) {
        return prepared;
    }

    const auto json = nlohmann::json::parse(req.body, nullptr, false);
    if (json.is_discarded() || !json.is_object() || !json.contains("model")) {
        return prepared;
    }

    if (!json.at("model").is_string()) {
        throw std::runtime_error("request field model must be a string");
    }

    const auto model = json.at("model").get<std::string>();
    const auto found = std::find(config.inbound_models.begin(), config.inbound_models.end(), model);
    if (found == config.inbound_models.end()) {
        throw std::runtime_error("model is not allowed: " + model);
    }

    prepared.inbound_model = model;
    prepared.upstream_model = resolve_upstream_model_name(config, model);

    if (*prepared.upstream_model != model) {
        auto rewritten_json = json;
        rewritten_json["model"] = *prepared.upstream_model;
        prepared.outbound_body = rewritten_json.dump();
    }

    return prepared;
}

UpstreamResponse forward_request(
    const AppConfig& config,
    const UrlParts& url_parts,
    const std::string& request_id,
    const httplib::Request& req,
    const std::string& outbound_body) {
    CurlHandle curl;
    if (curl.handle == nullptr) {
        throw std::runtime_error("curl_easy_init failed");
    }

    UpstreamResponse response;
    const auto upstream_url = build_upstream_url(url_parts, req);

    CurlHeaders headers;
    headers.append("Authorization: Bearer " + config.outbound_api_key);
    headers.append("X-Proxy-Request-Id: " + request_id);

    for (const auto& [key, value] : req.headers) {
        if (should_forward_header(key)) {
            headers.append(key + ": " + value);
        }
    }

    curl_easy_setopt(curl.handle, CURLOPT_URL, upstream_url.c_str());
    curl_easy_setopt(curl.handle, CURLOPT_HTTPHEADER, headers.list);
    curl_easy_setopt(curl.handle, CURLOPT_CUSTOMREQUEST, req.method.c_str());
    curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION, &write_body_callback);
    curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl.handle, CURLOPT_HEADERFUNCTION, &write_header_callback);
    curl_easy_setopt(curl.handle, CURLOPT_HEADERDATA, &response);
    curl_easy_setopt(curl.handle, CURLOPT_TIMEOUT, config.upstream_timeout_seconds);
    curl_easy_setopt(curl.handle, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl.handle, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.handle, CURLOPT_SSL_VERIFYPEER, config.verify_upstream_tls ? 1L : 0L);
    curl_easy_setopt(curl.handle, CURLOPT_SSL_VERIFYHOST, config.verify_upstream_tls ? 2L : 0L);

    if (requires_body(req.method) || req.method == "DELETE") {
        curl_easy_setopt(curl.handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(outbound_body.size()));
        curl_easy_setopt(curl.handle, CURLOPT_POSTFIELDS, outbound_body.data());
    }

    const auto code = curl_easy_perform(curl.handle);
    if (code != CURLE_OK) {
        throw std::runtime_error(std::string("upstream request failed: ") + curl_easy_strerror(code));
    }

    curl_easy_getinfo(curl.handle, CURLINFO_RESPONSE_CODE, &response.status_code);
    return response;
}

void set_json_error(httplib::Response& res, int status, const std::string& type, const std::string& message) {
    add_common_headers(res);
    nlohmann::json error = {
        {"error", {
            {"message", message},
            {"type", type},
            {"code", status}
        }}
    };
    res.status = status;
    res.set_content(error.dump(2), "application/json");
}

void log_request(
    const AppConfig& config,
    const std::string& request_id,
    const httplib::Request& req,
    const std::string& auth_token) {
    auto logger = app_logger();
    logger->info(
        "[{}] inbound request remote_addr={} remote_port={} method={} path={} query={} content_length={} headers=[{}] auth={} body_preview=\n{}",
        request_id,
        req.remote_addr,
        req.remote_port,
        req.method,
        req.path,
        query_string_only(req),
        req.body.size(),
        headers_to_string(req.headers),
        mask_secret(auth_token),
        body_preview(req.body, config.request_body_log_limit));
}

void log_upstream_response(
    const AppConfig& config,
    const std::string& request_id,
    const std::string& upstream_url,
    long status_code,
    const std::multimap<std::string, std::string>& headers,
    const std::string& body,
    long long duration_ms) {
    auto logger = app_logger();
    logger->info(
        "[{}] upstream response url={} status={} duration_ms={} headers=[{}] body_preview=\n{}",
        request_id,
        upstream_url,
        status_code,
        duration_ms,
        headers_to_string(headers),
        body_preview(body, config.response_body_log_limit));
}

void handle_proxy_request(const AppConfig& config, const UrlParts& url_parts, const httplib::Request& req, httplib::Response& res) {
    const auto request_id = make_request_id();
    add_common_headers(res);
    res.set_header("X-Request-Id", request_id);

    const auto token = extract_bearer_token(req);
    if (!token.has_value() || config.inbound_api_keys.find(*token) == config.inbound_api_keys.end()) {
        app_logger()->warn(
            "[{}] rejected unauthorized request remote_addr={} path={} auth_header={}",
            request_id,
            req.remote_addr,
            req.path,
            mask_secret(req.get_header_value("Authorization")));
        set_json_error(res, 401, "invalid_request_error", "invalid inbound api key");
        return;
    }

    PreparedRequest prepared_request{req.body, std::nullopt, std::nullopt};
    try {
        prepared_request = prepare_request_for_upstream(config, req);
        log_request(config, request_id, req, *token);
        if (prepared_request.inbound_model.has_value()) {
            app_logger()->info(
                "[{}] model mapping inbound_model={} upstream_model={}",
                request_id,
                *prepared_request.inbound_model,
                prepared_request.upstream_model.value_or(*prepared_request.inbound_model));
        }
    } catch (const std::exception& ex) {
        app_logger()->warn("[{}] request validation failed: {}", request_id, ex.what());
        set_json_error(res, 400, "invalid_request_error", ex.what());
        return;
    }

    const auto upstream_url = build_upstream_url(url_parts, req);
    const auto start = std::chrono::steady_clock::now();

    try {
        auto upstream_response = forward_request(config, url_parts, request_id, req, prepared_request.outbound_body);
        const auto end = std::chrono::steady_clock::now();
        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        for (const auto& [key, value] : upstream_response.headers) {
            if (should_return_header(key)) {
                res.set_header(key.c_str(), value.c_str());
            }
        }

        res.status = static_cast<int>(upstream_response.status_code);
        res.set_content(upstream_response.body, upstream_response.content_type.empty() ? "application/json" : upstream_response.content_type.c_str());

        log_upstream_response(
            config,
            request_id,
            upstream_url,
            upstream_response.status_code,
            upstream_response.headers,
            upstream_response.body,
            duration_ms);
    } catch (const std::exception& ex) {
        const auto end = std::chrono::steady_clock::now();
        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        app_logger()->error(
            "[{}] upstream forward failed url={} duration_ms={} error={}",
            request_id,
            upstream_url,
            duration_ms,
            ex.what());
        set_json_error(res, 502, "api_connection_error", ex.what());
    }
}

void handle_models(const AppConfig& config, const httplib::Request& req, httplib::Response& res) {
    const auto request_id = make_request_id();
    add_common_headers(res);
    res.set_header("X-Request-Id", request_id);

    const auto token = extract_bearer_token(req);
    if (!token.has_value() || config.inbound_api_keys.find(*token) == config.inbound_api_keys.end()) {
        set_json_error(res, 401, "invalid_request_error", "invalid inbound api key");
        return;
    }

    nlohmann::json data = nlohmann::json::array();
    for (const auto& model : config.inbound_models) {
        data.push_back({
            {"id", model},
            {"object", "model"},
            {"created", 0},
            {"owned_by", "llm_api_proxy"}
        });
    }

    nlohmann::json result = {
        {"object", "list"},
        {"data", data}
    };

    app_logger()->info("[{}] served model list to remote_addr={}", request_id, req.remote_addr);
    res.status = 200;
    res.set_content(result.dump(2), "application/json");
}

void handle_options(httplib::Response& res) {
    add_common_headers(res);
    res.status = 204;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const std::string config_path = argc > 1 ? argv[1] : "config.json";
        const auto config = load_config(config_path);
        init_logging(config.log_dir);

        app_logger()->info(
            "starting llm_api_proxy bind_address={} listen_port={} inbound_models={} inbound_api_key_count={} outbound_api_url={} outbound_api_key={} log_dir={} verify_upstream_tls={} upstream_timeout_seconds={}",
            config.bind_address,
            config.listen_port,
            config.inbound_models.size(),
            config.inbound_api_keys.size(),
            config.outbound_api_url,
            mask_secret(config.outbound_api_key),
            config.log_dir,
            config.verify_upstream_tls,
            config.upstream_timeout_seconds);

        const UrlParts url_parts = parse_url(config.outbound_api_url);
        CurlGlobal curl_global;

        httplib::Server server;
        server.set_keep_alive_max_count(100);
        server.set_read_timeout(60, 0);
        server.set_write_timeout(300, 0);
        server.set_idle_interval(0, 500000);
        server.set_payload_max_length(100 * 1024 * 1024);

        server.set_exception_handler([](const auto& req, auto& res, std::exception_ptr ep) {
            try {
                if (ep) {
                    std::rethrow_exception(ep);
                }
            } catch (const std::exception& ex) {
                app_logger()->error("unhandled exception method={} path={} error={}", req.method, req.path, ex.what());
                set_json_error(res, 500, "server_error", "internal server error");
            }
        });

        server.set_error_handler([](const auto& req, auto& res) {
            add_common_headers(res);
            app_logger()->warn("http error status={} method={} path={}", res.status, req.method, req.path);
        });

        server.Get("/healthz", [](const auto&, auto& res) {
            add_common_headers(res);
            res.status = 200;
            res.set_content("{\"status\":\"ok\"}", "application/json");
        });

        server.Get("/v1/models", [&config](const auto& req, auto& res) {
            handle_models(config, req, res);
        });

        server.Get("/models", [&config](const auto& req, auto& res) {
            handle_models(config, req, res);
        });

        server.Options(R"(/.*)", [](const auto&, auto& res) {
            handle_options(res);
        });

        auto proxy_handler = [&config, &url_parts](const auto& req, auto& res) {
            handle_proxy_request(config, url_parts, req, res);
        };

        server.Get(R"(/.*)", proxy_handler);
        server.Post(R"(/.*)", proxy_handler);
        server.Put(R"(/.*)", proxy_handler);
        server.Patch(R"(/.*)", proxy_handler);
        server.Delete(R"(/.*)", proxy_handler);

        app_logger()->info("listening on {}:{}", config.bind_address, config.listen_port);
        if (!server.listen(config.bind_address.c_str(), config.listen_port)) {
            app_logger()->error("failed to bind {}:{}", config.bind_address, config.listen_port);
            return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    } catch (const std::exception& ex) {
        std::cerr << "fatal: " << ex.what() << std::endl;
        return EXIT_FAILURE;
    }
}