/*
 * agent.hpp — agent loop（M3 核心）
 *
 * 形态（ADR-0002/ADR-0007）：while(1)——
 *   调 LLM（协议插件构造请求 + libcurl 流式 + parse_feed 解析）→ 广播事件
 *   → 有 tool_use 则顺序执行工具、结果入会话、继续循环；否则完成
 *
 * 协议无关：core 持有抽象对话（JSON），协议插件在 build_request
 * 时转具体协议格式。
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "agent/executor.hpp"
#include "extension/loader.hpp"
#include "json.hpp"

namespace realagent {

/* token 计数（协议插件的 usage 事件；一次 LLM 调用内为绝对值，跨 turn 由 Agent
 * 累加） */
struct Usage {
  int64_t input = 0;
  int64_t output = 0;
  int64_t cache_read = 0;
  int64_t cache_write = 0;

  Usage &operator+=(const Usage &o) {
    input += o.input;
    output += o.output;
    cache_read += o.cache_read;
    cache_write += o.cache_write;
    return *this;
  }
  Usage operator+(const Usage &o) const {
    Usage r = *this;
    r += o;
    return r;
  }
  bool empty() const {
    return input == 0 && output == 0 && cache_read == 0 && cache_write == 0;
  }
};
/* 描述表供出站用（to_json）：字段名与 PROTOCOL.md 的 usage 帧逐字一致 */
BOOST_DESCRIBE_STRUCT(Usage, (), (input, output, cache_read, cache_write))

/* 协议插件的解析结果（一次 LLM 调用的产出） */
struct LlmOutcome {
  std::string text;     // 累积文本（非 tool_use 时）
  std::string thinking; // 思考内容（DeepSeek v4 reasoning，流式累积）
  std::string
      thinking_signature; // thinking 块签名（Anthropic 格式，回传历史用）
  struct ToolUse {
    std::string id;
    std::string name;
    std::string input; // JSON 对象字符串
  };
  std::vector<ToolUse> tool_uses;
  std::string stop_reason;
  std::string error; // 非空 = 本次调用失败的人话原因（广播给客户端）
  Usage usage;       // 本次调用的 token 计数（绝对值，后到覆盖先到）
};

class Agent {
public:
  Agent(CoreContext &ctx, Executor &exe);

  /* 一次用户输入 → 完整 loop（阻塞至 LLM 完成或工具链结束） */
  void run(const std::string &user_input);

  /* 中断当前 run（任意线程安全；curl/工具/turn 间均检查） */
  void interrupt();

  /* 会话消息（抽象格式，供持久化/构建 dialog） */
  json &messages() { return messages_; }

  /* 清空会话（/new 命令：新建会话） */
  void reset();

private:
  /* 构建抽象对话（system/messages/tools），tier 决定 dialog["model"] 取哪一档
   */
  json build_dialog(ModelTier tier) const;
  /* 一次 LLM 调用：协议插件构造 → libcurl → parse_feed → LlmOutcome */
  bool llm_call(const json &dialog, LlmOutcome &out);
  /* 广播事件（走 CoreContext::emit_fn + 插件 on_event） */
  void broadcast(const std::string &type, const json &payload);

  CoreContext &ctx_;
  Executor &exe_;
  json messages_;
  Usage run_usage_;
  std::atomic<bool> abort_{false};
};

} // namespace realagent
