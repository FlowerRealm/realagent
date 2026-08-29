/*
 * approval.hpp — 审批协调器（权限检查点 ASK 状态机）
 *
 * ADR-0005：core 永远是审批发起方。裁决为 ASK 时，agent 线程阻塞等待用户裁决
 * （绝不按 allow 放行），事件循环线程收 POST /approval-response 后唤醒。
 *
 * 流程：
 *   executor 检查点 → 裁决 ASK → ApprovalCoordinator::await
 *     → 发 permission_request（入事件队列 → 推送流）→ 阻塞（条件变量，30s 超时 deny）
 *     → 事件循环收 /approval-response → respond() 设置裁决 + notify
 *     → agent 线程继续
 */
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace realagent {

/* 权限裁决。ASK 不是第三种结论，是"这一条得问人"——问完仍然只有放行与拒绝。 */
enum class Verdict { Allow,
                     Deny,
                     Ask };

struct PendingApproval {
    std::string id;    // 请求 ID（permission_request / approval-response 关联）
    std::string agent; // 谁在问。中断 A 不能掐掉 B 挂着的这一条（ADR-0019）
    std::string tool_name;
    std::string params;
    Verdict verdict = Verdict::Deny;
    bool responded = false;
    std::mutex mtx;
    std::condition_variable cv;
};

class ApprovalCoordinator {
  public:
    ApprovalCoordinator() = default;
    ~ApprovalCoordinator();

    /* 事件出口：core → 客户端（推送流）。agent 线程经事件队列异步投递（ADR-0002）。 */
    void set_emit(std::function<void(const std::string &type, const std::string &payload)> emit)
    {
        emit_ = std::move(emit);
    }

    /* 此刻有没有客户端能裁决。**没有就别问**——见 executor 的检查点（ADR-0019 §8）。 */
    void set_online(std::function<bool()> fn) { online_ = std::move(fn); }
    bool online() const { return !online_ || online_(); }

    /* agent 线程：请求审批，阻塞直到裁决。30s 超时按 deny（危险工具默认拒绝）。
     * agent 是提问方的 id，随 permission_request 帧下发——两个 agent 同时问，
     * 用户得知道是谁在问（ADR-0019 §8）。 */
    Verdict await(const std::string &agent, const std::string &tool_name,
                  const std::string &params);

    /* 事件循环线程：收到 /approval-response 裁决 */
    void respond(const std::string &id, bool allow);

    /* 取消某个 agent 挂着的全部审批（按 deny 唤醒）。
     * **按 agent，不是一刀切**：POST /interrupt 现在指名道姓，中断 A 却掐掉 B 正等着的
     * 那条审批，是把「停下这一个」办成了「全场停摆」（ADR-0019 §8）。 */
    void cancel(const std::string &agent);

  private:
    /* 析构：谁也不剩了，全部按 deny 放掉，别让线程悬在条件变量上 */
    void cancel_all();

    std::function<bool()> online_;
    std::function<void(const std::string &, const std::string &)> emit_;
    std::mutex mtx_;
    std::unordered_map<std::string, std::shared_ptr<PendingApproval>> pending_;
    uint64_t next_id_ = 1;
};

} // namespace realagent
