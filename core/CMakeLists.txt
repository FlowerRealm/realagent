cmake_minimum_required(VERSION 3.30)
project(realagent-core CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(Boost REQUIRED COMPONENTS json)
find_package(spdlog REQUIRED)
# realugin：插件体系（C ABI + 加载器）。装在别处就给 CMAKE_PREFIX_PATH
find_package(realugin REQUIRED)
# macOS 的 libcurl 是系统库（SDK 自带头），FindCURL 会把 SDK usr/include 当 include dir
# 注入为 isystem——与 brew libc++ 头冲突。因此只取库路径，不用 CURL::libcurl target。
find_package(CURL REQUIRED)
# quiche（QUIC/HTTP3）：brew install cloudflare-quiche
find_path(QUICHE_INCLUDE_DIR quiche.h)
find_library(QUICHE_LIBRARY NAMES quiche)

# —— realagent::sdk —— realagent 的插件词汇表（agent_caps.h）。
# realugin 只给机制，能力键叫什么、什么签名是这边的事；插件工程 find_package(realagent)
# 拿到它。纯 C 头，零依赖（只 include realugin/plugin_api.h）。
add_library(realagent_sdk INTERFACE)
add_library(realagent::sdk ALIAS realagent_sdk)
set_target_properties(realagent_sdk PROPERTIES EXPORT_NAME sdk)
target_link_libraries(realagent_sdk INTERFACE realugin::sdk)
target_include_directories(realagent_sdk INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk>
  $<INSTALL_INTERFACE:include>)

add_executable(realagent-core
  src/main.cpp
  src/config.cpp
  src/extension/slots.cpp
  src/agent/executor.cpp
  src/agent/approval.cpp
  src/agent/agent.cpp
  src/agent/session.cpp
  src/server/quic_server.cpp
)
target_include_directories(realagent-core PRIVATE include ${QUICHE_INCLUDE_DIR})
target_link_libraries(realagent-core PRIVATE
  realagent::sdk
  realugin::loader
  Boost::json
  spdlog::spdlog
  ${CURL_LIBRARIES}
  ${QUICHE_LIBRARY}
)

# —— 安装：只装 SDK（core 自己是可执行文件，另说） ——
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS realagent_sdk EXPORT realagent-targets)
install(DIRECTORY sdk/realagent DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT realagent-targets
  FILE realagent-targets.cmake
  NAMESPACE realagent::
  DESTINATION ${CMAKE_INSTALL_DATADIR}/realagent/cmake)
configure_package_config_file(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/realagent-config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/realagent-config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_DATADIR}/realagent/cmake)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/realagent-config.cmake
  DESTINATION ${CMAKE_INSTALL_DATADIR}/realagent/cmake)

enable_testing()
add_subdirectory(tests)
