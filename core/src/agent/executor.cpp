#include "agent/executor.hpp"

#include <cstdio>

#include "json.hpp"

namespace realagent {

Executor::Executor(CoreContext& ctx, ApprovalCoordinator& approval)
    : ctx_(ctx), approval_(approval) {}

namespace {
/* 权限策略：一个配置键，一个 switch（ADR-0016）。
 *   ask       —— 危险工具一律问用户（默认）
 *   allow-all —— 一律放行。打通链路用，不是安全策略
 *   deny      —— 一律拒绝
 * 认不出的值按 ask：配置写错时该多问一句，不该多放一次行。 */
Verdict decide(const Config& cfg) {
    const std::string mode = cfg.get("permission");
    if (mode == "allow-all") return Verdict::Allow;
    if (mode == "deny") return Verdict::Deny;
    if (mode != "ask" && !mode.empty())
        fprintf(stderr, "[perm] 未知 permission=%s，按 ask 处理\n", mode.c_str());
    return Verdict::Ask;
}
} // namespace

bool Executor::check_permission(const ToolDef& tool, const std::string& params_json,
                                std::string* denied_reason) {
    if (!tool.dangerous) return true; // 只读工具不触发
    switch (decide(*ctx_.config)) {
        case Verdict::Allow:
            return true;
        case Verdict::Deny:
            if (denied_reason) *denied_reason = "denied by permission policy";
            return false;
        case Verdict::Ask:
            // 审批链路（ADR-0005）：core 发 permission_request → 用户裁决 → 回传。
            // agent 线程真等裁决（30s 超时 deny），事件循环线程收 /approval-response 唤醒
            if (approval_.await(tool.name, params_json) != Verdict::Allow) {
                if (denied_reason) *denied_reason = "denied by user";
                return false;
            }
            return true;
    }
    return false;
}

void Executor::interrupt() {
    std::lock_guard<std::mutex> lk(inflight_mtx_);
    interrupted_ = true;
    if (inflight_) interrupt_tool(); // 手上没有在跑的：标记留给下一次 execute 撞上
}

void Executor::reset() {
    std::lock_guard<std::mutex> lk(inflight_mtx_);
    interrupted_ = false;
}

nlohmann::json Executor::execute(const std::string& call_id, const std::string& name,
                       const std::string& params_json) {
    const ToolDef* tool = find_tool(name);
    if (!tool) return nlohmann::json{{"status", 1}, {"output", "unknown tool"}, {"interrupted", false}};

    std::string reason;
    if (!check_permission(*tool, params_json, &reason))
        return nlohmann::json{{"status", 1}, {"output", reason}, {"interrupted", false}};

    // 登记在先、执行在后：这个顺序才让 interrupt() 要么打断得到它、要么撞上 interrupted_，
    // 不存在"检查完了才开始跑"的缝
    {
        std::lock_guard<std::mutex> lk(inflight_mtx_);
        if (interrupted_)
            return nlohmann::json{{"status", 1}, {"output", "interrupted by user"}, {"interrupted", true}};
        inflight_ = true;
    }
    nlohmann::json r = run_tool(call_id, name, params_json, ctx_.emit_fn);
    {
        std::lock_guard<std::mutex> lk(inflight_mtx_);
        // "算不算被中断"由 core 判——中止是 core 提的，工具不必编造状态码
        r["interrupted"] = interrupted_;
        inflight_ = false;
    }
    return r;
}

} // namespace realagent
