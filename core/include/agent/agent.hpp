/*
 * agent.hpp — agent loop（M3 核心）
 *
 * 形态（ADR-0002/ADR-0007）：while(1)——
 *   调 LLM（造请求 → curl 发出 → 解析 SSE → 计价）→ 广播事件
 *   → 有 tool_use 则顺序执行工具、结果入会话、继续循环；否则完成
 *
 * core 持有抽象对话（JSON），llm 模块负责转成 /v1/messages 的具体形状。
 */
#pragma once

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

#include "agent/executor.hpp"
#include "agent/session.hpp"
#include "context.hpp"
#include "json.hpp"
#include "llm/llm.hpp"

namespace realagent {

/* 一次 LLM 调用的产出 */
struct LlmOutcome {
    std::string text;               // 累积文本（非 tool_use 时）
    std::string thinking;           // 思考内容（流式累积）
    std::string thinking_signature; // thinking 块签名（回传历史用）
    struct ToolUse {
        std::string id;
        std::string name;
        std::string input; // JSON 对象字符串
    };
    std::vector<ToolUse> tool_uses;
    std::string stop_reason;
    std::string error;  // 非空 = 本次调用失败的人话原因（广播给客户端）
    double cost = 0;    // 本次调用花掉的钱（USD，绝对值，后到覆盖先到）
};

class Agent {
public:
    Agent(CoreContext& ctx, Executor& exe);

    /* 一次用户输入 → 完整 loop（阻塞至 LLM 完成或工具链结束） */
    void run(const std::string& user_input);

    /* 中断当前 run（任意线程安全；curl/工具/turn 间均检查） */
    void interrupt();

    /* 会话消息（抽象格式，供持久化/构建 dialog） */
    nlohmann::json& messages() { return messages_; }

    /* 新建会话（/new 命令）：清空历史 + 换一个会话文件。
     * 旧文件不动——它是记录，不是缓存。 */
    void reset();

    /* 恢复会话（/resume <id>）：JSONL 读回历史，后续追加写进该文件。
     * 读不到返回 false，此时当前会话**原样不动**（宁可恢复失败，也不能把人扔进空白）。*/
    bool resume(const std::string& id);

    /* 当前会话 id（GET /sessions 标出 current，客户端据此知道自己在哪儿） */
    const std::string& session_id() const { return session_.id(); }

    /* 模型数据表（/model 清单与状态栏元数据的唯一来源） */
    const Pricing& pricing() const { return pricing_; }

    /* SseParser 产出的事件落在这里：累积进 out，该实时广播的顺手广播。
     * public 只因为 libcurl 的写回调是个自由函数，进不来私有区。 */
    void on_llm_event(std::string_view type, const nlohmann::json& ev, LlmOutcome& out,
                      const std::string& model);

private:
    /* 消息入账的唯一入口：进内存，同时落盘。
     * run() 里有六处产生消息，全都走这里——散着写 push_back 迟早漏掉一处，
     * 而漏掉的那条恢复会话时就凭空消失了。 */
    void record(nlohmann::json msg);

    /* 构建抽象对话（system/messages/tools），tier 决定 dialog["model"] 取哪一档 */
    nlohmann::json build_dialog(ModelTier tier) const;
    /* 一次 LLM 调用：build_request → libcurl → SseParser → LlmOutcome */
    bool llm_call(const nlohmann::json& dialog, LlmOutcome& out);
    /* 广播事件（走 CoreContext::emit_fn） */
    void broadcast(const std::string& type, const nlohmann::json& payload);

    CoreContext& ctx_;
    Executor& exe_;
    Pricing pricing_;     // 启动时读一次（配置不热重载，ADR-0010）
    nlohmann::json messages_;
    Session session_;     // 当前会话的落盘去处（JSONL，append-only）
    double run_cost_ = 0; // 本次 run 累计花费（USD），一次用户输入起算清零
    std::atomic<bool> abort_{false};
};

} // namespace realagent
