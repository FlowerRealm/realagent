/*
 * executor.hpp — 工具执行器（agent 模块）
 *
 * 链路（ADR-0002 / ADR-0005）：
 *   查工具定义 → dangerous 则按 permission 配置裁决（ASK 走审批协调器）→ 执行
 * 单 agent 内工具严格顺序执行。
 *
 * 中止（ADR-0002 R8 的后一半）：execute 卡在 bash 里时，interrupt() 从事件循环线程
 * 进来把子进程组打掉。顺序执行的语义正好意味着"在跑的工具"至多一个，
 * 连指针都不必记——工具那边一个 pid 就够。
 */
#pragma once

#include <mutex>
#include <string>

#include "agent/approval.hpp"
#include "context.hpp"
#include "tools/tools.hpp"

namespace realagent {

class Executor {
public:
    Executor(CoreContext& ctx, ApprovalCoordinator& approval);

    /* 权限检查：dangerous 工具按 permission 配置裁决。ASK → 真等用户裁决（ADR-0005）。 */
    bool check_permission(const ToolDef& tool, const std::string& params_json,
                          std::string* denied_reason);

    /* 执行工具。返回 run_tool 的 json（{"status","output"}），再加一个 "interrupted"：
     * 本次执行期间 core 提过中止——与"工具自己失败了"不是一回事。
     * call_id 透传给工具：实时输出帧（tool_output）要靠它认领是哪次调用。 */
    json execute(const std::string& call_id, const std::string& name,
                 const std::string& params_json);

    /* 中止在跑的工具（任意线程）。没有在跑的也要记下——紧随其后的那次 execute 直接拒掉，
     * 否则用户按了中止、模型的下一个工具照跑不误。 */
    void interrupt();

    /* 新一轮 run 的起点：抹掉上一轮的中止痕迹。不做这一步，中止会一直粘着（同 abort_）。 */
    void reset();

private:
    CoreContext& ctx_;
    ApprovalCoordinator& approval_;

    std::mutex inflight_mtx_;
    bool inflight_ = false;
    bool interrupted_ = false;
};

} // namespace realagent
