#include "agent/executor.hpp"

#include <cstdio>

#include "json.hpp"

namespace realagent {

Executor::Executor(CoreContext& ctx) : ctx_(ctx) {}

const ToolEntry* Executor::find_tool(const std::string& name) const {
    const auto it = ctx_.tools.find(name);
    return it == ctx_.tools.end() ? nullptr : &it->second;
}

bool Executor::check_permission(const ToolEntry& tool, const std::string& params_json,
                                std::string* denied_reason) {
    if (!tool.def.dangerous) return true; // 只读工具不触发
    // 遍历权限插件：首版 perm-allow-all 返回 ALLOW。
    // ask：无客户端接入（M5）前按 allow 处理，记录日志。
    bool any_perm_plugin = false;
    for (const auto* p : ctx_.all_plugins) {
        if (!p || !p->api || p->api->type != PLUGIN_TYPE_PERMISSION || !p->api->decide) continue;
        any_perm_plugin = true;
        const auto verdict = p->api->decide(p->instance, tool.def.name, params_json.c_str());
        if (verdict == PLUGIN_PERM_DENY) {
            if (denied_reason) *denied_reason = "denied by permission plugin";
            return false;
        }
        // PLUGIN_PERM_ASK：core 应发询问（M5）；首版按 allow 放行
        if (verdict == PLUGIN_PERM_ASK) {
            fprintf(stderr, "[perm] %s: ASK 无客户端，首版按 allow\n", tool.def.name);
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
