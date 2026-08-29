/*
 * tools.hpp — 内置工具：read / edit / bash
 *
 * edit 只有一个操作：把一段行范围换成一段文本（范围由 [[Anchor]] 指定，两端闭）。
 * 改、删、插、创建是它的四种用法，不是四个操作——创建就是「文件不存在，没有 anchor
 * 可给」那一格，所以**没有独立的 write 工具**（ADR-0018）。
 *
 * 中止（ADR-0002 R8）：只有 bash 需要——它是唯一会跑很久的那个。
 * 中止请求从事件循环线程进来，读循环在 agent 线程；单 agent 内工具严格顺序执行，
 * 同时至多一个子进程，一个 pid 就记得住，不需要表。
 */
#pragma once

#include <span>
#include <string>

#include "context.hpp"
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

} // namespace realagent
