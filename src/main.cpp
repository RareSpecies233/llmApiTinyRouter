#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <mutex>
using json = nlohmann::json;

namespace Log {
static std::ofstream log_stream;
static std::mutex log_mutex;

void Init(const std::string& path) {
  std::lock_guard<std::mutex> lk(log_mutex);
  if (log_stream.is_open()) return;
  log_stream.open(path, std::ios::out | std::ios::app);
}

void Write(const std::string& level, const std::string& msg) {
  std::lock_guard<std::mutex> lk(log_mutex);
  if (!log_stream.is_open()) return;
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
  log_stream << "[" << buf << "] [" << level << "] " << msg << std::endl;
  log_stream.flush();
}

void Info(const std::string& msg) { Write("INFO", msg); }
void Error(const std::string& msg) { Write("ERROR", msg); }
void Debug(const std::string& msg) { Write("DEBUG", msg); }
} // namespace Log

struct RouteConfig {
  std::string inbound_key;
  std::string inbound_model;
  std::string outbound_key;
  std::string outbound_model;
  std::string outbound_api_base;
};

struct ServerConfig {
  std::string host;
  int port;
};

struct AppConfig {
  ServerConfig server;
  std::vector<RouteConfig> routes;
};

struct ParsedUrl {
  bool https;
  std::string host;
  int port;
  std::string base_path;
};

std::string ReadAllText(const std::string& path) {
  std::ifstream input(path);
  if (!input.is_open()) {
    throw std::runtime_error("failed to open config: " + path);
  }

  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

std::string RequireString(const json& object, const char* key) {
  if (!object.contains(key) || !object.at(key).is_string()) {
    throw std::runtime_error(std::string("missing or invalid string field: ") + key);
  }
  return object.at(key).get<std::string>();
}

int RequireInt(const json& object, const char* key) {
  if (!object.contains(key) || !object.at(key).is_number_integer()) {
    throw std::runtime_error(std::string("missing or invalid integer field: ") + key);
  }
  return object.at(key).get<int>();
}

AppConfig LoadConfig(const std::string& path) {
  const json root = json::parse(ReadAllText(path));

  if (!root.contains("server") || !root.at("server").is_object()) {
    throw std::runtime_error("missing or invalid server config");
  }
  if (!root.contains("routes") || !root.at("routes").is_array()) {
    throw std::runtime_error("missing or invalid routes config");
  }

  AppConfig config;
  const auto& server = root.at("server");
  config.server.host = RequireString(server, "host");
  config.server.port = RequireInt(server, "port");

  for (const auto& route_json : root.at("routes")) {
    if (!route_json.is_object()) {
      throw std::runtime_error("route entry must be an object");
    }

    RouteConfig route;
    route.inbound_key = RequireString(route_json, "inbound_key");
    route.inbound_model = RequireString(route_json, "inbound_model");
    route.outbound_key = RequireString(route_json, "outbound_key");
    route.outbound_model = RequireString(route_json, "outbound_model");
    route.outbound_api_base = RequireString(route_json, "outbound_api_base");
    config.routes.push_back(std::move(route));
  }

  if (config.routes.empty()) {
    throw std::runtime_error("at least one route is required");
  }

  return config;
}

ParsedUrl ParseUrl(const std::string& url) {
  ParsedUrl parsed;
  std::string remainder;

  if (url.rfind("https://", 0) == 0) {
    parsed.https = true;
    remainder = url.substr(8);
    parsed.port = 443;
  } else if (url.rfind("http://", 0) == 0) {
    parsed.https = false;
    remainder = url.substr(7);
    parsed.port = 80;
  } else {
    throw std::runtime_error("outbound_api_base must start with http:// or https://");
  }

  const auto slash_pos = remainder.find('/');
  const std::string host_port = slash_pos == std::string::npos ? remainder : remainder.substr(0, slash_pos);
  parsed.base_path = slash_pos == std::string::npos ? "" : remainder.substr(slash_pos);

  const auto colon_pos = host_port.find(':');
  if (colon_pos == std::string::npos) {
    parsed.host = host_port;
  } else {
    parsed.host = host_port.substr(0, colon_pos);
    parsed.port = std::stoi(host_port.substr(colon_pos + 1));
  }

  if (parsed.host.empty()) {
    throw std::runtime_error("invalid outbound_api_base host");
  }

  return parsed;
}

std::string GetBearerToken(const httplib::Request& req) {
  const auto auth_header = req.get_header_value("Authorization");
  constexpr char kPrefix[] = "Bearer ";
  if (auth_header.rfind(kPrefix, 0) != 0) {
    return {};
  }
  return auth_header.substr(sizeof(kPrefix) - 1);
}

const RouteConfig* FindRoute(const AppConfig& config, const std::string& inbound_key, const std::string& inbound_model) {
  for (const auto& route : config.routes) {
    if (route.inbound_key == inbound_key && route.inbound_model == inbound_model) {
      return &route;
    }
  }
  return nullptr;
}

std::string BuildTargetPath(const ParsedUrl& parsed, const std::string& request_path) {
  if (parsed.base_path.empty()) {
    return request_path;
  }
  if (parsed.base_path.back() == '/' && !request_path.empty() && request_path.front() == '/') {
    return parsed.base_path.substr(0, parsed.base_path.size() - 1) + request_path;
  }
  if (parsed.base_path.back() != '/' && !request_path.empty() && request_path.front() != '/') {
    return parsed.base_path + "/" + request_path;
  }
  return parsed.base_path + request_path;
}

json BuildErrorBody(const std::string& message, const std::string& type = "invalid_request_error") {
  return json{
      {"error", {{"message", message}, {"type", type}}},
  };
}

void ReplyJson(httplib::Response& res, int status, const json& body) {
  res.status = status;
  res.set_content(body.dump(), "application/json");
}

httplib::Headers BuildForwardHeaders(const httplib::Request& req, const RouteConfig& route) {
  httplib::Headers headers;
  headers.emplace("Authorization", "Bearer " + route.outbound_key);
  headers.emplace("Content-Type", "application/json");

  const auto accept = req.get_header_value("Accept");
  if (!accept.empty()) {
    headers.emplace("Accept", accept);
  }

  const auto organization = req.get_header_value("OpenAI-Organization");
  if (!organization.empty()) {
    headers.emplace("OpenAI-Organization", organization);
  }

  const auto project = req.get_header_value("OpenAI-Project");
  if (!project.empty()) {
    headers.emplace("OpenAI-Project", project);
  }

  return headers;
}

httplib::Result PostUpstream(const ParsedUrl& upstream,
                            const std::string& target_path,
                            const httplib::Headers& headers,
                            const std::string& body) {
  if (upstream.https) {
    httplib::SSLClient client(upstream.host, upstream.port);
    client.enable_server_certificate_verification(true);
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(300, 0);
    client.set_write_timeout(300, 0);
    client.set_follow_location(true);
    return client.Post(target_path.c_str(), headers, body, "application/json");
  }

  httplib::Client client(upstream.host, upstream.port);
  client.set_connection_timeout(10, 0);
  client.set_read_timeout(300, 0);
  client.set_write_timeout(300, 0);
  client.set_follow_location(true);
  return client.Post(target_path.c_str(), headers, body, "application/json");
}

void ProxyRequest(const AppConfig& config, const httplib::Request& req, httplib::Response& res) {
  Log::Info(std::string("incoming request: ") + req.method + " " + req.path);
  const std::string inbound_key = GetBearerToken(req);
  if (inbound_key.empty()) {
    Log::Error("missing or invalid Authorization header");
    ReplyJson(res, 401, BuildErrorBody("missing or invalid Authorization header", "authentication_error"));
    return;
  }

  json request_json;
  try {
    request_json = json::parse(req.body);
  } catch (const std::exception&) {
    ReplyJson(res, 400, BuildErrorBody("request body is not valid JSON"));
    return;
  }

  if (!request_json.contains("model") || !request_json.at("model").is_string()) {
    ReplyJson(res, 400, BuildErrorBody("request body must contain string field: model"));
    return;
  }

  const std::string inbound_model = request_json.at("model").get<std::string>();
  const RouteConfig* route = FindRoute(config, inbound_key, inbound_model);
  if (route == nullptr) {
    ReplyJson(res, 403, BuildErrorBody("inbound key and model do not match any configured route", "permission_error"));
    return;
  }

  request_json["model"] = route->outbound_model;

  ParsedUrl upstream;
  try {
    upstream = ParseUrl(route->outbound_api_base);
  } catch (const std::exception& ex) {
    ReplyJson(res, 500, BuildErrorBody(std::string("invalid outbound_api_base: ") + ex.what(), "server_error"));
    return;
  }

  const auto target_path = BuildTargetPath(upstream, req.path);
  const auto headers = BuildForwardHeaders(req, *route);
  const auto body = request_json.dump();

  Log::Debug(std::string("forwarding to upstream: ") + (upstream.https ? "https://" : "http://") + upstream.host + ":" + std::to_string(upstream.port) + " " + target_path);
  auto upstream_response = PostUpstream(upstream, target_path, headers, body);
  if (!upstream_response) {
    Log::Error("failed to connect upstream API");
    ReplyJson(res, 502, BuildErrorBody("failed to connect upstream API", "api_connection_error"));
    return;
  }

  res.status = upstream_response->status;

  const auto content_type = upstream_response->get_header_value("Content-Type");
  if (!content_type.empty()) {
    res.set_header("Content-Type", content_type);
  }

  const auto cache_control = upstream_response->get_header_value("Cache-Control");
  if (!cache_control.empty()) {
    res.set_header("Cache-Control", cache_control);
  }

  res.body = upstream_response->body;
  Log::Info(std::string("upstream responded with status ") + std::to_string(upstream_response->status));
}

int main(int argc, char** argv) {
  const std::string config_path = argc > 1 ? argv[1] : "config.json";

  AppConfig config;
  try {
    config = LoadConfig(config_path);
  } catch (const std::exception& ex) {
    std::cerr << "config error: " << ex.what() << std::endl;
    return EXIT_FAILURE;
  }

  // 初始化日志
  Log::Init("llm_api_proxy.log");
  Log::Info(std::string("starting proxy with config: ") + config_path);

  httplib::Server server;

  auto handler = [&config](const httplib::Request& req, httplib::Response& res) {
    try {
      ProxyRequest(config, req, res);
    } catch (const std::exception& ex) {
      Log::Error(std::string("unhandled exception in handler: ") + ex.what());
      ReplyJson(res, 500, BuildErrorBody(std::string("internal server error: ") + ex.what(), "server_error"));
    } catch (...) {
      Log::Error("unhandled unknown exception in handler");
      ReplyJson(res, 500, BuildErrorBody("internal server error", "server_error"));
    }
  };

  server.Post(R"(/v1/chat/completions)", handler);
  server.Post(R"(/v1/completions)", handler);
  server.Post(R"(/v1/responses)", handler);

  std::cout << "LLM API proxy listening on " << config.server.host << ":" << config.server.port << std::endl;
  std::cout << "using config: " << config_path << std::endl;

  if (!server.listen(config.server.host.c_str(), config.server.port)) {
    std::cerr << "failed to start server" << std::endl;
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}