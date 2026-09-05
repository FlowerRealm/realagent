/*
 * stop.cpp — stop 工具：收工，回 idle 等下一条消息
 *
 * agent loop 一圈一个 turn，**唯一的出口就是这个工具**：模型不调它，它的回答就会被
 * 原样送回去再跑一圈。「模型这次没调工具」不再等于「它干完了」——那两件事本来就不是
 * 一回事，一个是它这一句话说完了，一个是活干完了。
 *
 * 实现只有一件事：在结果里挂一个 stop 标记。loop 读这个字段，不认工具名字——
 * 名字是给模型看的，loop 认名字就等于把工具表抄了一份进循环里。
 */
#include "tools/tools.hpp"

namespace realagent {

nlohmann::json stop_def()
{
    return tool_def(
        "stop", "收工",
        "Finish this run and go idle until a new message arrives.\n"
        "This is the ONLY way to conclude a run. Call this tool whenever you have answered\n"
        "the user's question, completed the requested task, or have no further actions to perform.\n"
        "Always call this tool along with your final response text. Do NOT invent new tasks.",
        R"({"type":"object","properties":{}})", false);
}

nlohmann::json stop_run()
{
    nlohmann::json r = tool_ok("stopped");
    r["stop"] = true;
    return r;
}

} // namespace realagent
