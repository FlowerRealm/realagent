/*
 * spawn.cpp — spawn 工具的定义。实现在 Executor 里（agent/executor.cpp）
 *
 * 它要认识 Agents 才能派生，而 tools/ 在 agent/ 下面，反过来包含就是层级倒挂。
 * 定义仍旧放在这里：LLM 看见的工具清单只有一份，加一个工具就是加一个文件。
 */
#include "tools/tools.hpp"

namespace realagent {

nlohmann::json spawn_def()
{
    return tool_def(
        "spawn", "派生 agent",
        "Spawn a new agent in the background. Returns the new agent's numeric id immediately\n"
        "without waiting for it to finish.\n"
        "Agent ids are positive integers incrementing from 1 (1, 2, 3, ...).\n"
        "in_edges: agent ids permitted to send messages to the new agent and receive its\n"
        "completion notice.\n"
        "out_edges: agent ids the new agent is permitted to send messages to.\n"
        "To receive its completion notice, include your own agent id in in_edges.\n"
        "Ids present in both lists establish bidirectional communication.\n"
        "Both lists accept only your own agent id or known agent ids.",
        R"({"type":"object","properties":{"workdir":{"type":"string","description":"working directory for the new agent, required"},"prompt":{"type":"string","description":"initial message handed to the new agent"},"in_edges":{"type":"array","items":{"type":"integer"},"description":"agent ids permitted to send messages to this agent and receive its completion notice"},"out_edges":{"type":"array","items":{"type":"integer"},"description":"agent ids this agent is permitted to send messages to"}},"required":["workdir","prompt"]})",
        true);
}

} // namespace realagent
