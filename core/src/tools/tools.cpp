/*
 * tools.cpp — 工具壳：一张表 + 按名派发
 *
 * 每个工具的定义与实现各占一个文件（read.cpp / edit.cpp / bash.cpp /
 * spawn.cpp / send_message.cpp / stop.cpp），这里只把它们串起来。
 * 加一个工具 = 加一个文件 + 这里两行；改一个工具，壳一个字都不用动。
 */
#include <algorithm>
#include <array>

#include "tools/tools.hpp"

namespace realagent {

namespace {

/* 顺序即模型看到的顺序。函数内静态：跨翻译单元的初始化顺序在这里不构成问题。 */
const std::array<ToolDef, 6> &table()
{
    static const std::array<ToolDef, 6> k = {read_def(), edit_def(), spawn_def(),
                                             send_message_def(), bash_def(), stop_def()};
    return k;
}

} // namespace

std::span<const ToolDef> tool_defs() { return table(); }

const ToolDef *find_tool(std::string_view name)
{
    const auto &k = table();
    const auto it =
        std::find_if(k.begin(), k.end(), [&](const ToolDef &t) { return t.name == name; });
    return it == k.end() ? nullptr : &*it;
}

nlohmann::json run_tool(const std::string &call_id, const std::string &name,
                        const std::string &params_json, const EmitFn &emit,
                        const std::string &workdir)
{
    nlohmann::json params = nlohmann::json::parse(params_json, nullptr, false);
    if (params.is_discarded()) params = nlohmann::json::object();
    if (name == "read") return read_run(params, workdir);
    if (name == "edit") return edit_run(params, workdir);
    if (name == "bash") return bash_run(call_id, params, emit, workdir);
    if (name == "stop") return stop_run();
    // spawn / send_message 由 Executor 拦下，走不到这里
    return tool_fail("unknown tool: " + name);
}

} // namespace realagent
