#include "agent/executor.hpp"

#include <cstdio>

#include "json.hpp"

namespace realagent {

Executor::Executor(CoreContext& ctx, ApprovalCoordinator& approval)
    : ctx_(ctx), approval_(approval) {}

const ToolEntry* Executor::find_tool(const std::string& name) const {
    const auto it = ctx_.tools.find(name);
    return it == ctx_.tools.end() ? nullptr : &it->second;
}

bool Executor::check_permission(const ToolEntry& tool, const std::string& params_json,
                                std::string* denied_reason) {
    if (!tool.def.dangerous) return true; // 只读工具不触发
    // 遍历权限插件：首版 perm-allow-all 返回 ALLOW。
    bool any_perm_plugin = false;
    for (const auto* p : ctx_.all_plugins) {
        if (!p || !p->api || p->api->type != PLUGIN_TYPE_PERMISSION || !p->api->decide) continue;
        any_perm_plugin = true;
        const auto verdict = p->api->decide(p->instance, tool.def.name, params_json.c_str());
        if (verdict == PLUGIN_PERM_DENY) {
            if (denied_reason) *denied_reason = "denied by permission plugin";
            return false;
        }
        if (verdict == PLUGIN_PERM_ASK) {
            // 审批链路（ADR-0005）：core 发 permission_request → 用户界面裁决 → 回传
            // agent 线程真等裁决（30s 超时 deny），事件循环线程收 /approval-response 唤醒
            const auto user_verdict = approval_.await(tool.def.name, params_json);
            if (user_verdict != PLUGIN_PERM_ALLOW) {
                if (denied_reason) *denied_reason = "denied by user";
                return false;
            }
        }
    }
    if (!any_perm_plugin) {
        fprintf(stderr, "[perm] 无权限插件，dangerous 工具 %s 默认拒绝\n", tool.def.name);
        if (denied_reason) *denied_reason = "no permission plugin";
        return false;
    }
    return true;
}

ExecResult Executor::execute(const std::string& name, const std::string& params_json) {
    ExecResult bad{.status = 1, .messages = "{\"error\":\"unknown tool\"}"};
    const ToolEntry* tool = find_tool(name);
    if (!tool) return bad;
    if (tool->owner == nullptr || tool->owner->api == nullptr || !tool->owner->api->execute_tool) {
        bad.messages = "{\"error\":\"tool not executable\"}";
        return bad;
    }
    // 权限检查点
    std::string reason;
    if (!check_permission(*tool, params_json, &reason)) {
        json err;
        err["error"] = reason;
        bad.messages = err.dump();
        return bad;
    }
    // 执行
    const auto r = tool->owner->api->execute_tool(tool->owner->instance, /*call_id=*/"",
                                                  name.c_str(), params_json.c_str());
    ExecResult out;
    out.status = r.status;
    out.messages = r.messages ? r.messages : "";
    return out;
}

} // namespace realagent
