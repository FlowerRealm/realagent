/*
 * tools.hpp — 内置工具：一张表、按名派发、中止，外加每个工具自己那两件事
 *
 * 一个工具一个文件（src/tools/read.cpp、edit.cpp、bash.cpp、spawn.cpp、
 * send_message.cpp）：发给 LLM 的那段描述与它的实现住在一起，改了行为忘了改描述
 * 就没有缝可钻。tools.cpp 只剩壳——把这些定义串成表，按名字派发。
 *
 * spawn / send_message 只有定义没有实现：它们要认识 Agents，实现在 Executor 里
 * （tools/ 在 agent/ 下面，反过来包含就是层级倒挂）。定义仍旧在这张表里，
 * 因为 LLM 看见的工具清单只有一份。
 *
 * edit 只有一个操作：把一段行范围换成一段文本（范围由 [[Anchor]] 指定，两端闭）。
 * 改、删、插、创建是它的四种用法，不是四个操作——创建就是「文件不存在，没有 anchor
 * 可给」那一格，所以**没有独立的 write 工具**（ADR-0018）。
 *
 * 中止（ADR-0002 R8）：只有 bash 需要——它是唯一会跑很久的那个，所以 interrupt_tool()
 * 就定义在 bash.cpp 里，它要动的状态全在那儿。
 * 中止请求从事件循环线程进来，读循环在 agent 线程；单 agent 内工具严格顺序执行，
 * 同时至多一个子进程，一个 pid 就记得住，不需要表。
 */
#pragma once

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "agent/context.hpp"
#include "json.hpp"

namespace realagent {

/* 工具定义。dangerous = 执行前触发权限检查点 */
struct ToolDef {
    std::string name;
    std::string label;       // UI 显示名
    std::string description; // 发给 LLM 的描述
    std::string parameters;  // 参数 JSON Schema
    bool dangerous = false;
};

/* 工具清单（静态表，寿命 = 进程） */
std::span<const ToolDef> tool_defs();

/* 按名查定义；没有返回 nullptr */
const ToolDef *find_tool(std::string_view name);

/* 执行结果就是一个 json：{"status": <int, 0=成功>, "output": <string, 给模型看的文本>}。
 * 单独一个两字段结构体是多余的信封——工具本来就在拼 json，把 status 放进去即可。
 *
 * 执行。call_id 透传进实时输出帧（tool_output），客户端靠它认领是哪次调用。
 * workdir 是调用方 agent 的工作目录（ADR-0019）：相对路径从这里算起，bash 也 chdir 到这里。
 * core 进程自己的 cwd 与任何 agent 都无关，工具一眼都不该看它。 */
nlohmann::json run_tool(const std::string &call_id, const std::string &name,
                        const std::string &params_json, const EmitFn &emit,
                        const std::string &workdir);

/* 中止在跑的 bash（任意线程）。手上没有在跑的就什么都不做——
 * "下一次调用该不该拒"是 executor 的账，记在这里只会变成一个迟早过期的标志位。 */
void interrupt_tool();

/* —— 结果与参数：每个工具都要的那点东西 —— */

/* 结果只有一种形状：{"status", "output"}。status 非零即错，output 是给模型看的文本。 */
inline nlohmann::json tool_result(int status, std::string output)
{
    return nlohmann::json{{"status", status}, {"output", std::move(output)}};
}
inline nlohmann::json tool_fail(const std::string &msg) { return tool_result(1, msg); }
inline nlohmann::json tool_ok(const std::string &what) { return tool_result(0, what); }

/* 取一个字符串参数；缺失/非字符串返回 nullopt。
 * 参数是模型给的，形状不由 core 说了算——const operator[] 撞上缺键是未定义行为，
 * 这里只能按迭代器查。 */
inline std::optional<std::string> tool_arg(const nlohmann::json &params, std::string_view key)
{
    const auto it = params.find(key);
    if (it == params.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}

/* 相对路径从 agent 的工作目录算起（ADR-0019）。core 进程的 cwd 与任何 agent 都无关 */
inline std::string tool_resolve(const std::string &workdir, const std::string &path)
{
    const std::filesystem::path p(path);
    return p.is_absolute() ? path : (std::filesystem::path(workdir) / p).string();
}

/* —— 行与 anchor（ADR-0018）：read 打印它、edit 校验它，是同一份契约 —— */

inline std::vector<std::string> read_lines(const std::string &path)
{
    std::vector<std::string> lines;
    std::ifstream f(path);
    for (std::string l; std::getline(f, l);) lines.push_back(l);
    return lines;
}

inline bool write_lines(const std::string &path, const std::vector<std::string> &lines)
{
    std::ofstream f(path, std::ios::trunc);
    for (const std::string &l : lines) f << l << '\n';
    return f.good();
}

/* 一行的 hash：FNV-1a，3 个十六进制字符。空白不算——跑一遍格式化不该让它变。 */
inline std::string hash_line(const std::string &s)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : s)
        if (!std::isspace(c)) h = (h ^ c) * 16777619u;
    char buf[4];
    std::snprintf(buf, sizeof buf, "%03x", h & 0xfff);
    return buf;
}

/* —— 每个工具两件事：它的定义、它的实现 —— */

ToolDef read_def();
nlohmann::json read_run(const nlohmann::json &params, const std::string &workdir);

ToolDef edit_def();
nlohmann::json edit_run(const nlohmann::json &params, const std::string &workdir);

ToolDef bash_def();
nlohmann::json bash_run(const std::string &call_id, const nlohmann::json &params,
                        const EmitFn &emit, const std::string &workdir);

ToolDef spawn_def();
ToolDef send_message_def();

} // namespace realagent
