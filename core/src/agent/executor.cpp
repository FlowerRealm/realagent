#include "agent/executor.hpp"

#include <cstdio>
#include <cstdlib>

#include "json.hpp"

namespace realagent {

Executor::Executor(CoreContext& ctx, PluginManager& plugins, ApprovalCoordinator& approval)
    : ctx_(ctx), plugins_(plugins), approval_(approval) {}

bool Executor::check_permission(const ToolView& tool, const std::string& params_json,
                                std::string* denied_reason) {
    if (!tool.def->dangerous) return true; // 只读工具不触发
    // 权限槽（ADR-0012）：一个裁决者，一次调用。不遍历、不投票——
    // 槽位独占之后 ALLOW 就是字面意义的放行，不存在覆盖他人裁决的单调性问题
    const auto& perm = ctx_.slots.permission;
    if (!perm) {
        fprintf(stderr, "[perm] 权限槽空置，dangerous 工具 %s 默认拒绝\n", tool.name.c_str());
        if (denied_reason) *denied_reason = "no permission plugin";
        return false;
    }
    // 传**对外名**（core-tools_bash），不是本名（bash）。裁决者只有一个、面对全世界的
    // 工具，它需要全局唯一的名字才能分辨"哪个容器的 bash"。这与 execute 传本名不矛盾：
    // 插件只认识自己的名字，裁决者只认得全局的名字，两边要的本来就是不同的那一个。
    const auto verdict = perm.fn(perm.self, tool.name.c_str(), params_json.c_str());
    if (verdict == PLUGIN_PERM_DENY) {
        if (denied_reason) *denied_reason = "denied by permission plugin";
        return false;
    }
    if (verdict == PLUGIN_PERM_ASK) {
        // 审批链路（ADR-0005）：core 发 permission_request → 用户界面裁决 → 回传
        // agent 线程真等裁决（30s 超时 deny），事件循环线程收 /approval-response 唤醒
        // 同理传对外名：审批框是给用户看的，"core-tools_bash" 才说明是谁要跑
        if (approval_.await(tool.name, params_json) != PLUGIN_PERM_ALLOW) {
            if (denied_reason) *denied_reason = "denied by user";
            return false;
        }
    }
    return true;
}

void Executor::interrupt() {
    std::lock_guard<std::mutex> lk(inflight_mtx_);
    interrupted_ = true;
    if (!inflight_owner_) return; // 手上没有在跑的：标记留给下一次 execute 撞上
    // 容器不具备这项能力就是不可中断——照实等它跑完，不假装成功
    auto itr = cap_of<plugin_tool_interrupt_fn>(*inflight_owner_, PLUGIN_CAP_TOOL_INTERRUPT);
    if (itr) itr.fn(itr.self, inflight_call_id_.c_str());
}

void Executor::reset() {
    std::lock_guard<std::mutex> lk(inflight_mtx_);
    interrupted_ = false;
}

ExecResult Executor::execute(const std::string& call_id, const std::string& name,
                             const std::string& params_json) {
    ExecResult bad{.status = 1, .messages = "{\"error\":\"unknown tool\"}"};
    // 现问现答（ADR-0012）：core 不存工具表，用时向各容器拉一遍
    const auto views = plugins_.tools();
    const auto it = std::find_if(views.begin(), views.end(),
                                 [&](const ToolView& v) { return v.name == name; });
    if (it == views.end()) return bad;

    auto exec = cap_of<plugin_tool_execute_fn>(*it->owner, PLUGIN_CAP_TOOL_EXECUTE);
    if (!exec) {
        bad.messages = "{\"error\":\"tool not executable\"}";
        return bad;
    }
    std::string reason;
    if (!check_permission(*it, params_json, &reason)) {
        json err;
        err["error"] = reason;
        bad.messages = err.dump();
        return bad;
    }
    // 登记在先、执行在后：这个顺序才让 interrupt() 要么打断得到它、要么撞上 interrupted_，
    // 不存在"检查完了才开始跑"的缝
    {
        std::lock_guard<std::mutex> lk(inflight_mtx_);
        if (interrupted_) {
            bad.messages = "interrupted by user";
            bad.interrupted = true;
            return bad;
        }
        inflight_owner_ = it->owner;
        inflight_call_id_ = call_id;
    }
    // 传插件侧本名（def->name），不是对外名字——前缀是 core 加的，插件不认识
    const auto r = exec.fn(exec.self, call_id.c_str(), it->def->name, params_json.c_str());
    ExecResult out;
    out.status = r.status;
    out.messages = r.messages ? r.messages : "";
    {
        std::lock_guard<std::mutex> lk(inflight_mtx_);
        // "算不算被中断"由 core 判——中止是 core 提的，插件不必编造状态码
        out.interrupted = interrupted_;
        inflight_owner_ = nullptr;
    }
    // 结果是本次调用现造的 → 转移：core 释放（ADR-0012）
    std::free(const_cast<char*>(r.messages));
    return out;
}

} // namespace realagent
