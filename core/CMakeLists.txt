cmake_minimum_required(VERSION 3.25)
project(realagent-core CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(Boost REQUIRED COMPONENTS json)
find_package(spdlog REQUIRED)
# macOS 的 libcurl 是系统库（SDK 自带头），FindCURL 会把 SDK usr/include 当 include dir
# 注入为 isystem——与 brew libc++ 头冲突。因此只取库路径，不用 CURL::libcurl target。
find_package(CURL REQUIRED)
# quiche（QUIC/HTTP3）：brew install cloudflare-quiche
find_path(QUICHE_INCLUDE_DIR quiche.h)
find_library(QUICHE_LIBRARY NAMES quiche)

add_executable(realagent-core
  src/main.cpp
  src/config.cpp
  src/extension/loader.cpp
  src/agent/executor.cpp
  src/agent/approval.cpp
  src/agent/agent.cpp
  src/server/quic_server.cpp
)
target_include_directories(realagent-core PRIVATE include ${QUICHE_INCLUDE_DIR})
target_link_libraries(realagent-core PRIVATE
  Boost::json
  spdlog::spdlog
  ${CURL_LIBRARIES}
  ${QUICHE_LIBRARY}
)

enable_testing()
add_subdirectory(tests)
