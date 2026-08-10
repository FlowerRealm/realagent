/*
 * agent.hpp — agent loop（M3 核心）
 *
 * 形态（ADR-0002/ADR-0007）：while(1)——
 *   调 LLM（协议插件构造请求 + libcurl 流式 + parse_feed 解析）→ 广播事件
 *   → 有 tool_use 则顺序执行工具、结果入会话、继续循环；否则完成
 *
 * 协议无关：core 持有抽象对话（JSON），协议插件在 build_request 时转具体协议格式。
 */
#pragma once

#include <string>
#include <vector>

#include "json.hpp"
#include "extension/loader.hpp"
#include "agent/executor.hpp"

namespace realagent {

/* 协议插件的解析结果（一次 LLM 调用的产出） */
struct LlmOutcome {
    std::string text;                    // 累积文本（非 tool_use 时）
    struct ToolUse {
        std::string id;
        std::string name;
        std::string input;               // JSON 对象字符串
    };
    std::vector<ToolUse> tool_uses;
    std::string stop_reason;
};

class Agent {
public:
    Agent(CoreContext& ctx, Executor& exe);

    /* 一次用户输入 → 完整 loop（阻塞至 LLM 完成或工具链结束） */
    void run(const std::string& user_input);

    /* 会话消息（抽象格式，供持久化/构建 dialog） */
    json& messages() { return messages_; }

private:
    /* 构建抽象对话（system/messages/tools） */
    json build_dialog() const;
    /* 一次 LLM 调用：协议插件构造 → libcurl → parse_feed → LlmOutcome */
    bool llm_call(const json& dialog, LlmOutcome& out);
    /* 广播事件（走 CoreContext::emit_fn + 插件 on_event） */
    void broadcast(const std::string& type, const json& payload);

    CoreContext& ctx_;
    Executor& exe_;
    json messages_; // 抽象对话历史
};

} // namespace realagent
