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
#include "agent/context.hpp"
#include "tools/tools.hpp"

namespace realagent {

class Agents;

class Executor {
  public:
    /* pool 与 agent_id 是"去哪儿找别的 agent"和"谁在用我"，spawn / send_message 要这两样。
     * 独立构造（测试、test-tools）时 pool 为空，那两个工具就报"这里没有 agent 图"。
     * 进构造函数而不是事后 bind()：两段式初始化多出一个"造好了但还没接上"的中间态，
     * 而这里根本不需要它——Agent 的 pool_ / id_ 在 exe_ 之前就已经就位了。 */
    Executor(CoreContext &ctx, ApprovalCoordinator &approval, std::string workdir,
             Agents *pool = nullptr, int agent_id = 0);

    /* 权限检查：dangerous 工具按 permission 配置裁决。ASK → 真等用户裁决（ADR-0005）。 */
    bool check_permission(const ToolDef &tool, const std::string &params_json,
                          std::string *denied_reason);

    /* 执行工具。返回 run_tool 的 json（{"status","output"}），再加一个 "interrupted"：
     * 本次执行期间 core 提过中止——与"工具自己失败了"不是一回事。
     * call_id 透传给工具：实时输出帧（tool_output）要靠它认领是哪次调用。 */
    nlohmann::json execute(const std::string &call_id, const std::string &name,
                           const std::string &params_json);

    /* 中止在跑的工具（任意线程）。没有在跑的也要记下——紧随其后的那次 execute 直接拒掉，
     * 否则用户按了中止、模型的下一个工具照跑不误。 */
    void interrupt();

    /* 新一轮 run 的起点：抹掉上一轮的中止痕迹。不做这一步，中止会一直粘着（同 abort_）。 */
    void reset();

  private:
    CoreContext &ctx_;
    ApprovalCoordinator &approval_;
    std::string workdir_; // 工具的相对路径从这里算起，bash 也 chdir 到这里
    Agents *pool_ = nullptr;
    int agent_id_ = 0; // 这个 executor 属于哪个 agent

    /* spawn / send_message：它们要认识 Agents，所以实现在这儿而不在 tools.cpp——
     * tools/ 在 agent/ 下面，反过来包含就是层级倒挂。 */
    nlohmann::json agent_tool(const std::string &name, const nlohmann::json &params);

    std::mutex inflight_mtx_;
    bool inflight_ = false;
    bool interrupted_ = false;
};

} // namespace realagent
