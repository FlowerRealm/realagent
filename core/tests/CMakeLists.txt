find_package(Boost REQUIRED COMPONENTS json)

# 配置与模型档位测试（直接编译 config.cpp，不起服务、不碰网络）
add_executable(test_config test_config.cpp ../src/config.cpp)
target_include_directories(test_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_config PRIVATE Boost::json)
add_test(NAME config COMMAND test_config)

# deepseek-messages 协议插件测试（dlopen 插件，验证 build_request/parse_feed）。
# 容器编在哪儿由 REALAGENT_PLUGIN_DIR 说了算（默认沿用并排布局），
# 运行时还可用同名环境变量覆盖——两个仓库不必并排摆放。
set(REALAGENT_PLUGIN_DIR "${CMAKE_SOURCE_DIR}/../realagent-plugins/build"
    CACHE PATH "已编好的插件容器目录（供 protocol_plugin 测试 dlopen）")
add_executable(test_protocol_plugin test_protocol_plugin.cpp)
target_link_libraries(test_protocol_plugin PRIVATE realagent::sdk Boost::json)
target_compile_definitions(test_protocol_plugin PRIVATE RA_PLUGIN_DIR="${REALAGENT_PLUGIN_DIR}")
add_test(NAME protocol_plugin COMMAND test_protocol_plugin)

# 会话持久化测试（JSONL 读写；cwd 指到临时目录，会话目录相对 cwd）
add_executable(test_session test_session.cpp ../src/agent/session.cpp ../src/config.cpp)
target_include_directories(test_session PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_session PRIVATE Boost::json)
add_test(NAME session COMMAND test_session)
