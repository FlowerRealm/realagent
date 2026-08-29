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

# 会话历史 → 事件帧（ADR-0020）。纯函数，不起服务、不碰盘
add_executable(test_history test_history.cpp ../src/agent/history.cpp)
target_include_directories(test_history PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
add_test(NAME history COMMAND test_history)

# 内置工具 + 权限裁决测试（ADR-0016 真正换掉的那半边：静态工具表 + permission 三值）
add_executable(test_tools test_tools.cpp
                          ../src/tools/tools.cpp ../src/tools/read.cpp ../src/tools/edit.cpp
                          ../src/tools/bash.cpp ../src/tools/spawn.cpp ../src/tools/send_message.cpp
                          ../src/agent/executor.cpp
                          ../src/agent/agents.cpp ../src/agent/agent.cpp ../src/agent/session.cpp
                          ../src/llm/llm.cpp
                          ../src/llm/upstream/anthropic_messages.cpp ../src/llm/upstream/openai_chat.cpp
                          ../src/llm/upstream/openai_responses.cpp
                          ../src/llm/downstream/anthropic_messages.cpp
                          ../src/llm/downstream/openai_chat.cpp
                          ../src/llm/downstream/openai_responses.cpp
                          ../src/agent/approval.cpp ../src/config.cpp)
target_include_directories(test_tools PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_tools PRIVATE ${CURL_LIBRARIES})
add_test(NAME tools COMMAND test_tools)

# agent 的收件箱与线程生命周期（ADR-0019）。端点没配，LLM 调用当场失败——
# 这里验的是消息进出收件箱，不是模型回了什么，所以不碰网络。
add_executable(test_agent test_agent.cpp
  ../src/agent/agent.cpp ../src/agent/agents.cpp ../src/agent/executor.cpp ../src/agent/approval.cpp
  ../src/agent/session.cpp ../src/llm/llm.cpp
  ../src/tools/tools.cpp ../src/tools/read.cpp ../src/tools/edit.cpp
  ../src/tools/bash.cpp ../src/tools/spawn.cpp ../src/tools/send_message.cpp
  ../src/llm/upstream/anthropic_messages.cpp ../src/llm/upstream/openai_chat.cpp
  ../src/llm/upstream/openai_responses.cpp ../src/llm/downstream/anthropic_messages.cpp
  ../src/llm/downstream/openai_chat.cpp ../src/llm/downstream/openai_responses.cpp
  ../src/config.cpp)
target_include_directories(test_agent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_agent PRIVATE ${CURL_LIBRARIES})
add_test(NAME agent COMMAND test_agent)
