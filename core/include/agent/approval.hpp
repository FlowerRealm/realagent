/*
 * approval.hpp — 审批协调器（权限检查点 ASK 状态机）
 *
 * ADR-0005：core 永远是审批发起方。权限插件返回 ASK 时，agent 线程阻塞
 * 等待用户裁决（绝不按 allow 放行），事件循环线程收 POST /approval-response 后唤醒。
 *
 * 流程（ADR-0005 审批等待实现）：
 *   executor 检查点 → 权限插件 ASK → ApprovalCoordinator::await
 *     → 发 permission_request（入事件队列 → 推送流）→ 阻塞（条件变量，30s 超时 deny）
 *     → 事件循环收 /approval-response → respond() 设置裁决 + notify
 *     → agent 线程继续，裁决交给权限插件语义
 */
#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <realugin/plugin_api.h>

namespace realagent {

struct PendingApproval {
    std::string id;        // 请求 ID（permission_request / approval-response 关联）
    std::string tool_name;
    std::string params;
    plugin_permission_t verdict = PLUGIN_PERM_DENY;
    bool responded = false;
    std::mutex mtx;
    std::condition_variable cv;
};

class ApprovalCoordinator {
public:
    ApprovalCoordinator() = default;
    ~ApprovalCoordinator();

    /* 事件出口：core → 客户端（推送流）。agent 线程经事件队列异步投递（ADR-0002）。 */
    void set_emit(std::function<void(const std::string& type, const std::string& payload)> emit) {
        emit_ = std::move(emit);
    }

    /* agent 线程：请求审批，阻塞直到裁决。30s 超时按 deny（危险工具默认拒绝）。 */
    plugin_permission_t await(const std::string& tool_name, const std::string& params);

    /* 事件循环线程：收到 /approval-response 裁决 */
    void respond(const std::string& id, bool allow);

    /* 清理全部挂起审批（按 deny 唤醒，避免线程悬挂） */
    void cancel_all();

private:
    std::function<void(const std::string&, const std::string&)> emit_;
    std::mutex mtx_;
    std::unordered_map<std::string, std::shared_ptr<PendingApproval>> pending_;
    uint64_t next_id_ = 1;
};

} // namespace realagent
