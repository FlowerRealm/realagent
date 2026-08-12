find_package(Boost REQUIRED COMPONENTS json)

# 配置与模型档位测试（直接编译 config.cpp，不起服务、不碰网络）
add_executable(test_config test_config.cpp ../src/config.cpp)
target_include_directories(test_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_config PRIVATE Boost::json)
add_test(NAME config COMMAND test_config)

# deepseek-messages 协议插件测试（dlopen 插件，验证 build_request/parse_feed）
add_executable(test_protocol_plugin test_protocol_plugin.cpp)
target_include_directories(test_protocol_plugin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../sdk)
target_link_libraries(test_protocol_plugin PRIVATE Boost::json)
add_test(NAME protocol_plugin COMMAND test_protocol_plugin)
set_tests_properties(protocol_plugin PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/../realagent-plugins/build)
