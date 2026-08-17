/*
 * executor.hpp — 工具执行器（agent 模块）
 *
 * 链路（ADR-0002 / ADR-0005 / ADR-0012）：
 *   现问现答拉工具视图 → 权限检查（dangerous 工具 → 权限槽裁决）→ 调该容器的 tool.execute
 * core 不存工具表：每次执行前向各容器拉一遍（ADR-0012）。单 agent 内工具严格顺序执行。
 *
 * 中止（ADR-0002 R8 的后一半）：execute 卡在插件里时，interrupt() 从事件循环线程进来，
 * 找到在跑的那个容器、调它的 tool.interrupt。**不开线程**——顺序执行的语义正好意味着
 * "在跑的工具"至多一个，一个指针就记得住。
 */
#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "extension/slots.hpp"
#include "agent/approval.hpp"

namespace realagent {

/* 工具执行结果（包装插件返回） */
struct ExecResult {
    int status;
    std::string messages; // JSON
    bool interrupted = false; // core 在本次执行期间提过中止：与"工具自己失败了"不是一回事
};

class Executor {
public:
    Executor(CoreContext& ctx, PluginManager& plugins, ApprovalCoordinator& approval);

    /* 权限检查：dangerous 工具经权限槽裁决。ASK → 协调器真等用户裁决（ADR-0005）。 */
    bool check_permission(const ToolView& tool, const std::string& params_json,
                          std::string* denied_reason);

    /* 执行工具（按对外名字查视图；顺序执行语义由调用方保证）。
     * call_id 透传给插件：实时输出帧（tool_output）要靠它认领是哪次调用。 */
    ExecResult execute(const std::string& call_id, const std::string& name,
                       const std::string& params_json);

    /* 中止在跑的工具（任意线程）。没有在跑的也要记下——紧随其后的那次 execute 直接拒掉，
     * 否则用户按了中止、模型的下一个工具照跑不误。 */
    void interrupt();

    /* 新一轮 run 的起点：抹掉上一轮的中止痕迹。不做这一步，中止会一直粘着（同 abort_）。 */
    void reset();

private:
    CoreContext& ctx_;
    PluginManager& plugins_;
    ApprovalCoordinator& approval_;

    // 在跑的那次调用。锁只护"登记/摘牌"这几行，不覆盖 tool.execute 本身——
    // 覆盖了 interrupt() 就得排在它后面，那正是它要打断的东西
    std::mutex inflight_mtx_;
    const Plugin* inflight_owner_ = nullptr;
    std::string inflight_call_id_;
    bool interrupted_ = false;
};

} // namespace realagent
