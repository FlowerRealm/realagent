#include "agent/executor.hpp"

#include "agent/agents.hpp"

#include <cstdio>

#include "json.hpp"

namespace realagent {

Executor::Executor(CoreContext &ctx, ApprovalCoordinator &approval, std::string workdir,
                   Agents *pool, int agent_id)
    : ctx_(ctx), approval_(approval), workdir_(std::move(workdir)), pool_(pool),
      agent_id_(agent_id) {}

namespace {
/* 权限策略：一个配置键，一个 switch（ADR-0016）。
 *   ask       —— 危险工具一律问用户（默认）
 *   allow-all —— 一律放行。打通链路用，不是安全策略
 *   deny      —— 一律拒绝
 * 认不出的值按 ask：配置写错时该多问一句，不该多放一次行。 */
Verdict decide(const Config &cfg)
{
    const std::string mode = cfg.get("/permission");
    if (mode == "allow-all") return Verdict::Allow;
    if (mode == "deny") return Verdict::Deny;
    if (mode != "ask" && !mode.empty())
        fprintf(stderr, "[perm] 未知 permission=%s，按 ask 处理\n", mode.c_str());
    return Verdict::Ask;
}
} // namespace

bool Executor::check_permission(const ToolDef &tool, const std::string &params_json,
                                std::string *denied_reason)
{
    if (!tool.dangerous) return true; // 只读工具不触发
    switch (decide(*ctx_.config))
    {
        case Verdict::Allow:
            return true;
        case Verdict::Deny:
            if (denied_reason) *denied_reason = "denied by permission policy";
            return false;
        case Verdict::Ask:
            // 没有客户端连着就当场拒绝，不等那 30 秒（ADR-0019 §8）：agent 没有客户端
            // 也照跑，两条合起来就是后台 agent 的每个危险工具都卡 30 秒然后必然被拒——
            // 那不是安全策略，是一个装成策略的超时。当场拒是同一个结论，早 30 秒给出，
            // 而且能说一句诚实的话
            if (!approval_.online())
            {
                if (denied_reason) *denied_reason = "无客户端可裁决";
                return false;
            }
            // 审批链路（ADR-0005）：core 发 permission_request → 用户裁决 → 回传。
            // agent 线程真等裁决（30s 超时 deny），事件循环线程收 /approval-response 唤醒
            if (approval_.await(agent_id_, tool.name, params_json) != Verdict::Allow)
            {
                if (denied_reason) *denied_reason = "denied by user";
                return false;
            }
            return true;
    }
    return false;
}

void Executor::interrupt()
{
    std::lock_guard<std::mutex> lk(inflight_mtx_);
    interrupted_ = true;
    if (inflight_) interrupt_tool(); // 手上没有在跑的：标记留给下一次 execute 撞上
}

void Executor::reset()
{
    std::lock_guard<std::mutex> lk(inflight_mtx_);
    interrupted_ = false;
}

nlohmann::json Executor::execute(const std::string &call_id, const std::string &name,
                                 const std::string &params_json)
{
    const ToolDef *tool = find_tool(name);
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
    nlohmann::json params = nlohmann::json::parse(params_json, nullptr, false);
    if (params.is_discarded()) params = nlohmann::json::object();
    nlohmann::json r = (name == "spawn" || name == "send_message")
                           ? agent_tool(name, params)
                           : run_tool(call_id, name, params_json, ctx_.emit_fn, workdir_);
    {
        std::lock_guard<std::mutex> lk(inflight_mtx_);
        // "算不算被中断"由 core 判——中止是 core 提的，工具不必编造状态码
        r["interrupted"] = interrupted_;
        inflight_ = false;
    }
    return r;
}

/* 取一个整数数组参数；缺失/形状不对当空。参数是模型给的，形状不由 core 说了算。 */
static std::vector<int> id_list(const nlohmann::json &p, std::string_view key)
{
    std::vector<int> out;
    const auto it = p.find(key);
    if (it == p.end() || !it->is_array()) return out;
    for (const auto &v : *it)
    {
        if (v.is_number_integer() || v.is_number_unsigned())
            out.push_back(v.get<int>());
    }
    return out;
}

nlohmann::json Executor::agent_tool(const std::string &name, const nlohmann::json &params)
{
    const auto fail = [](const std::string &m) {
        return nlohmann::json{{"status", 1}, {"output", m}};
    };
    if (!pool_) return fail("no agent graph here");

    const auto str = [&params](std::string_view k) {
        const auto it = params.find(k);
        return it != params.end() && it->is_string() ? it->get<std::string>() : std::string();
    };

    if (name == "send_message")
    {
        const auto it_to = params.find("to");
        if (it_to == params.end() || (!it_to->is_number_integer() && !it_to->is_number_unsigned()))
            return fail("send_message is missing or has an invalid target agent id: to");
        const int to = it_to->get<int>();
        if (!pool_->post(to, str("text")))
            return fail("no such agent: " + std::to_string(to));
        return nlohmann::json{{"status", 0}, {"output", "sent to " + std::to_string(to)}};
    }

    // spawn：派生方决定新 agent 的全部出入边（ADR-0019）
    std::string err;
    const int id = pool_->create(str("workdir"), agent_id_, id_list(params, "in_edges"),
                                 id_list(params, "out_edges"), err);
    if (id <= 0) return fail(err);
    pool_->post(id, str("prompt"));
    return nlohmann::json{{"status", 0}, {"output", std::to_string(id)}};
}

} // namespace realagent
