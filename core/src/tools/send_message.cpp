/*
 * send_message.cpp — send_message 工具的定义。实现在 Executor 里（agent/executor.cpp）
 *
 * 与 spawn 同理：投递要认识别的 agent，那是 agent/ 的知识。
 */
#include "tools/tools.hpp"

namespace realagent {

ToolDef send_message_def()
{
    return {"send_message", "给别的 agent 发消息",
            "把一条消息投进另一个 agent 的收件箱。只能发给你有出边的那些 agent "
            "数字 id——没有边就不知道它存在。",
            R"({"type":"object","properties":{"to":{"type":"integer"},"text":{"type":"string"}},"required":["to","text"]})",
            false};
}

} // namespace realagent
