find_package(Boost REQUIRED COMPONENTS json)

# deepseek-messages 协议插件测试（dlopen 插件，验证 build_request/parse_feed）
add_executable(test_protocol_plugin test_protocol_plugin.cpp)
target_include_directories(test_protocol_plugin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../sdk)
target_link_libraries(test_protocol_plugin PRIVATE Boost::json)
add_test(NAME protocol_plugin COMMAND test_protocol_plugin)
set_tests_properties(protocol_plugin PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/../realagent-plugins/build)
