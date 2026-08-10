/*
 * executor.hpp — 工具执行器（agent 模块）
 *
 * 链路（ADR-0002 / ADR-0005）：
 *   查注册表 → 权限检查（dangerous 工具 → permission 插件裁决）→ 调插件 execute_tool
 * 单 agent 内工具严格顺序执行。
 */
#pragma once

#include <string>
#include <vector>

#include "extension/loader.hpp"
#include "agent/approval.hpp"

namespace realagent {

/* 工具执行结果（包装插件返回） */
struct ExecResult {
    int status;
    std::string messages; // JSON
};

class Executor {
public:
    explicit Executor(CoreContext& ctx, ApprovalCoordinator& approval);

    /* 按名查工具；不存在返回 nullptr */
    const ToolEntry* find_tool(const std::string& name) const;

    /* 权限检查：dangerous 工具经权限插件裁决。ASK → 协调器真等用户裁决（ADR-0005）。 */
    bool check_permission(const ToolEntry& tool, const std::string& params_json,
                          std::string* denied_reason);

    /* 执行工具（顺序执行语义由调用方保证）。 */
    ExecResult execute(const std::string& name, const std::string& params_json);

private:
    CoreContext& ctx_;
    ApprovalCoordinator& approval_;
};

} // namespace realagent
