# 配置与模型档位测试（不起服务、不碰网络）
add_executable(test_config test_config.cpp ../src/config.cpp)
target_include_directories(test_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
add_test(NAME config COMMAND test_config)

# llm 模块测试（三套协议各自的造请求与解析 / 计价 / 端点配置校验）
add_executable(test_llm test_llm.cpp
  ../src/llm/llm.cpp
  ../src/llm/upstream/anthropic_messages.cpp
  ../src/llm/upstream/openai_chat.cpp
  ../src/llm/upstream/openai_responses.cpp
  ../src/llm/downstream/anthropic_messages.cpp
  ../src/llm/downstream/openai_chat.cpp
  ../src/llm/downstream/openai_responses.cpp
  ../src/config.cpp)
target_include_directories(test_llm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
add_test(NAME llm COMMAND test_llm)

# 会话持久化测试（JSONL 读写；cwd 指到临时目录，会话目录相对 cwd）
add_executable(test_session test_session.cpp ../src/agent/session.cpp ../src/config.cpp)
target_include_directories(test_session PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
add_test(NAME session COMMAND test_session)

# 内置工具 + 权限裁决测试（ADR-0016 真正换掉的那半边：静态工具表 + permission 三值）
add_executable(test_tools test_tools.cpp ../src/tools/tools.cpp ../src/agent/executor.cpp
                          ../src/agent/approval.cpp ../src/config.cpp)
target_include_directories(test_tools PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
add_test(NAME tools COMMAND test_tools)
