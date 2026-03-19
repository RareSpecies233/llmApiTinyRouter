cmake_minimum_required(VERSION 3.20)

project(llm_api_proxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

find_package(OpenSSL REQUIRED)

find_path(CPP_HTTPLIB_INCLUDE_DIR
  NAMES httplib.h
  PATHS /opt/homebrew/include /usr/local/include
)

find_path(NLOHMANN_JSON_INCLUDE_DIR
  NAMES nlohmann/json.hpp
  PATHS /opt/homebrew/include /usr/local/include
)

if(CPP_HTTPLIB_INCLUDE_DIR)
  add_library(httplib::httplib INTERFACE IMPORTED)
  set_target_properties(httplib::httplib PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${CPP_HTTPLIB_INCLUDE_DIR}"
    INTERFACE_COMPILE_DEFINITIONS "CPPHTTPLIB_OPENSSL_SUPPORT"
    INTERFACE_LINK_LIBRARIES "OpenSSL::SSL;OpenSSL::Crypto"
  )
else()
  FetchContent_Declare(
    cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.20.1
  )
  FetchContent_MakeAvailable(cpp_httplib)
endif()

if(NLOHMANN_JSON_INCLUDE_DIR)
  add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)
  set_target_properties(nlohmann_json::nlohmann_json PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${NLOHMANN_JSON_INCLUDE_DIR}"
  )
else()
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

add_executable(llm_api_proxy
  src/main.cpp
)

target_link_libraries(llm_api_proxy
  PRIVATE
    httplib::httplib
    nlohmann_json::nlohmann_json
)