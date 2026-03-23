cmake_minimum_required(VERSION 3.20)

project(llm_api_proxy VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  cpp_httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.17.3
)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)

FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.14.1
)

FetchContent_MakeAvailable(cpp_httplib nlohmann_json spdlog)

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(llm_api_proxy
  src/config.cpp
  src/logger.cpp
  src/main.cpp
)

target_include_directories(llm_api_proxy PRIVATE src)

target_compile_definitions(llm_api_proxy PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

target_link_libraries(llm_api_proxy PRIVATE
  CURL::libcurl
  OpenSSL::SSL
  OpenSSL::Crypto
  httplib::httplib
  nlohmann_json::nlohmann_json
  spdlog::spdlog
)

if(APPLE)
  target_link_libraries(llm_api_proxy PRIVATE "-framework CoreFoundation" "-framework Security")
endif()