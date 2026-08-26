#include "agent/approval.hpp"

#include <chrono>
#include <vector>

#include "json.hpp"

namespace realagent {

ApprovalCoordinator::~ApprovalCoordinator() { cancel_all(); }

Verdict ApprovalCoordinator::await(const std::string& tool_name,
                                               const std::string& params) {
    std::shared_ptr<PendingApproval> p;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        p = std::make_shared<PendingApproval>();
        p->id = "appr_" + std::to_string(next_id_++);
        p->tool_name = tool_name;
        p->params = params;
        pending_[p->id] = p;
    }
    // 发 permission_request（入事件队列 → 推送流，事件循环线程投递）
    json ev;
    ev["id"] = p->id;
    ev["tool"] = tool_name;
    if (auto args = json::parse(params)) ev["params"] = *args;
    if (emit_) emit_("permission_request", ev.dump());

    // 阻塞等待裁决（30s 超时按 deny）
    std::unique_lock<std::mutex> lk(p->mtx);
    const bool ok = p->cv.wait_for(lk, std::chrono::seconds(30), [&] { return p->responded; });
    {
        std::lock_guard<std::mutex> pm(mtx_);
        pending_.erase(p->id);
    }
    return ok ? p->verdict : Verdict::Deny;
}

void ApprovalCoordinator::respond(const std::string& id, bool allow) {
    std::shared_ptr<PendingApproval> p;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = pending_.find(id);
        if (it == pending_.end()) return;
        p = it->second;
    }
    std::lock_guard<std::mutex> lk(p->mtx);
    p->verdict = allow ? Verdict::Allow : Verdict::Deny;
    p->responded = true;
    p->cv.notify_one();
}

void ApprovalCoordinator::cancel_all() {
    std::vector<std::shared_ptr<PendingApproval>> all;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& [_, p] : pending_) all.push_back(p);
        pending_.clear();
    }
    for (auto& p : all) {
        std::lock_guard<std::mutex> lk(p->mtx);
        p->verdict = Verdict::Deny;
        p->responded = true;
        p->cv.notify_all();
    }
}

} // namespace realagent
