#include "agent/history.hpp"

#include <string>
#include <unordered_map>

namespace realagent {

namespace {

/* 帧的形状是 PROTOCOL.md 说了算的，这里只负责按同样的形状再摆一遍 */
void frame(nlohmann::json &out, const char *type, nlohmann::json data)
{
    out.push_back(nlohmann::json{{"type", type}, {"data", std::move(data)}});
}

std::string str(const nlohmann::json &j, const char *key)
{
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

} // namespace

nlohmann::json history_frames(const nlohmann::json &messages)
{
    nlohmann::json out = nlohmann::json::array();
    if (!messages.is_array()) return out;

    // tool_use_id → 工具名。tool_result 那条消息里没有名字，而 tool_execution_end
    // 帧要它。tool_use 必然排在它的 result 前面（否则那段历史本身就是坏的），
    // 所以一边走一边记就够，不需要先扫一遍
    std::unordered_map<std::string, std::string> tool_names;

    for (const auto &msg : messages)
    {
        const std::string role = str(msg, "role");
        const auto content = msg.find("content");
        if (content == msg.end() || !content->is_array()) continue;

        // assistant 消息就是一个 turn 的产出：思考、正文、要调的工具
        if (role == "assistant") frame(out, "turn_start", nlohmann::json::object());

        for (const auto &b : *content)
        {
            const std::string type = str(b, "type");
            if (type == "text" && role == "user")
            {
                // 用户消息的正文走 message_start 的 text 字段。**收件箱里三种来源
                // 都是 user**（人发的、别的 agent 发的、完成通知），历史里分不出来，
                // 也不需要分——发信人写在正文里（ADR-0019 §5）
                frame(out, "message_start", nlohmann::json{{"role", "user"}, {"text", str(b, "text")}});
            }
            else if (type == "text")
            {
                // 实时是一串 delta，回放是一整块。同一个帧类型，客户端那边
                // 「续写当前这条 assistant 消息」的处理逐字相同
                frame(out, "message_update", nlohmann::json{{"delta", str(b, "text")}});
            }
            else if (type == "thinking")
            {
                frame(out, "thinking_start", nlohmann::json{{"signature", str(b, "signature")}});
                frame(out, "thinking_update", nlohmann::json{{"delta", str(b, "thinking")}});
                frame(out, "thinking_stop", nlohmann::json::object());
            }
            else if (type == "tool_use")
            {
                const std::string id = str(b, "id"), name = str(b, "name");
                tool_names[id] = name;
                frame(out, "tool_execution_start", nlohmann::json{{"name", name}, {"id", id}});
            }
            else if (type == "tool_result")
            {
                const std::string id = str(b, "tool_use_id");
                const bool err = b.value("is_error", false);
                // 工具跑出来的东西实时是一串 tool_output，回放是一整块——同一个帧，
                // 客户端认领碎片的那段代码原样吃得下
                frame(out, "tool_output",
                      nlohmann::json{{"call_id", id}, {"stream", "output"}, {"text", str(b, "content")}});
                frame(out, "tool_execution_end", nlohmann::json{{"name", tool_names[id]}, {"id", id}, {"status", err ? 1 : 0}, {"interrupted", false}});
            }
        }

        if (role == "assistant") frame(out, "turn_end", nlohmann::json::object());
    }
    return out;
}

} // namespace realagent
