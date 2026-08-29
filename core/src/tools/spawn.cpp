/*
 * spawn.cpp — spawn 工具的定义。实现在 Executor 里（agent/executor.cpp）
 *
 * 它要认识 Agents 才能派生，而 tools/ 在 agent/ 下面，反过来包含就是层级倒挂。
 * 定义仍旧放在这里：LLM 看见的工具清单只有一份，加一个工具就是加一个文件。
 */
#include "tools/tools.hpp"

namespace realagent {

ToolDef spawn_def()
{
    return {
        "spawn", "派生 agent",
        "派生一个新 agent 去干一件事，立刻返回它的数字 id，不等它跑完。\n"
        "in_edges 是谁能给它发消息、谁收它的完成通知；out_edges 是它能给谁发消息。\n"
        "要收它的产出就把自己的数字 id 写进 in_edges；两个列表都填同一组人就是互相能发。\n"
        "两个列表里只能填你自己或你已经认识的 agent 数字 id——授不出自己没有的能力。",
        R"({"type":"object","properties":{"workdir":{"type":"string","description":"它的工作目录，必填"},"prompt":{"type":"string","description":"派给它的第一条消息"},"in_edges":{"type":"array","items":{"type":"integer"}},"out_edges":{"type":"array","items":{"type":"integer"}}},"required":["workdir","prompt"]})",
        true};
}

} // namespace realagent
