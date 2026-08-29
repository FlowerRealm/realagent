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
        "Spawn a new agent to do one thing; returns its numeric id at once, without waiting\n"
        "for it to finish.\n"
        "in_edges is who may send it messages and who receives its completion notice;\n"
        "out_edges is who it may send messages to.\n"
        "To receive its output, put your own numeric id in in_edges; the same ids in both\n"
        "lists means messages flow both ways.\n"
        "Both lists may only contain your own id or agent ids you already know — you cannot\n"
        "grant a capability you do not have yourself.",
        R"({"type":"object","properties":{"workdir":{"type":"string","description":"its working directory, required"},"prompt":{"type":"string","description":"the first message handed to it"},"in_edges":{"type":"array","items":{"type":"integer"}},"out_edges":{"type":"array","items":{"type":"integer"}}},"required":["workdir","prompt"]})",
        true};
}

} // namespace realagent
