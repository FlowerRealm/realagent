/*
 * agents.hpp — core 里活着的那些 Agent，以及它们之间的边（ADR-0019）
 *
 * 边是一张表：`A → B` 表示 A 知道 B 存在、能往 B 的收件箱投消息。有向，双向就是两条。
 * **只有一种边，边上不带类型**——「A 创建了 B」与「A 想给 C 发消息」是同一条边。
 *
 * 没有边就不知道对方存在：这里不给 agent 提供任何「列出所有 agent」的能力，
 * 于是边只能在创建对方时定下，不能靠查询产生。这是能力模型，不是访问控制——
 * 不存在「有权限但看不见」这种状态。
 *
 * 每个 agent 属于一个**组**，组的单位就是客户端（ADR-0021）。隔离是硬的：跨组的
 * agent id 一律当「无此 agent」，不区分「不存在」与「不是你的」——区分了就等于告诉
 * 调用方别的组里有什么。边不跨组是自动成立的：`spawn` 只能填创建者认识的，而它只认识同组的。
 *
 * 反向查（跑完时找入边）用全表扫。agent 数量不大，扫一遍是微秒级；额外维护一张
 * 入边表就是两份必须永远一致的真相。边不带 bool 值：「有没有边」已经由在不在表里
 * 表达，再存一个 bool 只多出「键在但值为 false」这第二种「没有边」的写法。
 */
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "agent/agent.hpp"

namespace realagent {

class Agents {
  public:
    Agents(CoreContext &ctx, ApprovalCoordinator &approval) : ctx_(ctx), approval_(approval) {}
    ~Agents();

    /* 建一个 agent。workdir 必传、无默认（ADR-0019）。
     *
     * 组的归属：by 非空（agent 派生的）就随创建者，**不可能属于别的组**——组就是
     * 所有权边界，这不是「继承」这种需要判断的东西；by 为空（客户端建的）就归 client。
     *
     * by 是创建方的 id；人（客户端）建的时候为空——**人不是图上的节点**。
     * in / out 是新 agent 的入边与出边：派生方决定它被谁知道、能找谁。
     * 两个列表里的 id 都必须是创建方自己有出边的（含它自己），否则报错——
     * 那是在授予能力，授不出自己没有的。
     *
     * 失败返回空串并写 err。 */
    std::string create(const std::string &client, const std::string &workdir,
                       const std::string &by, const std::vector<std::string> &in,
                       const std::vector<std::string> &out, std::string &err);

    /* 投一条消息。from 为空 = 人发的。
     *
     * 发信人写不写进正文取决于边：收方有指向发方的边才带 `[来自 x]`，
     * 没有就不带、跟人发的一模一样——这不是新规则，是边的语义自己长出来的。
     * 没有 to 这个 agent 返回 false。 */
    bool post(const std::string &to, const std::string &from, const std::string &text);

    /* from 能不能给 to 发消息 */
    bool has_edge(const std::string &from, const std::string &to) const;

    /* 某个 agent 跑完了：沿**入边**把完成通知投给关心它的邻居。
     * 边指向你关心的那个 agent，所以消息顺着边走、完成通知逆着边回来。
     * 由 agent 自己的线程调用。 */
    void on_done(const std::string &id, const std::string &summary);

    void interrupt(const std::string &id);
    void close(const std::string &id);

    /* 清单（GET /agents）：**只列 client 那一组**。组内不受边的约束——
     * 客户端看得见本组全部，agent 只看得见自己的出边邻居，两层。 */
    nlohmann::json list(const std::string &client) const;

    /* 本组里存在即返回指针，否则 nullptr。跨组一律当不存在。 */
    Agent *find(const std::string &client, const std::string &id);

    /* 本组里「哪个会话被哪个 agent 打开着」：session_id → agent_id（ADR-0019 §10）。
     * 一个会话要么被某个 agent 打开着，要么躺在盘上——`current` 这个字段在多 agent
     * 之后没有主语了，问「当前」得先问「谁的当前」。
     * 一次取整张表而不是逐条问：逐条问就是逐条上锁，而这张表本来就一次扫得完。 */
    std::map<std::string, std::string> openers(const std::string &client) const;

    /* 关掉一整组（客户端退出，或断线满 60 秒）。
     * 顺序写死：先 interrupt 组内每个在跑的，再逐个拆——直接拆一个正在跑的 agent，
     * 它的线程会往一个已经拆掉的收件箱里写。 */
    void close_group(const std::string &client);

  private:
    CoreContext &ctx_;
    ApprovalCoordinator &approval_;

    mutable std::mutex mtx_;
    std::map<std::string, std::unique_ptr<Agent>> agents_;
    std::unordered_map<std::string, std::unordered_set<std::string>> edges_; // 出边
    std::unordered_map<std::string, std::string> owner_;                     // agent id → 组
    unsigned next_id_ = 0;

    /* 内部用：调用方已持锁。跨组返回 nullptr —— 隔离的唯一执行点 */
    Agent *locked_find(const std::string &client, const std::string &id) const;
};

} // namespace realagent
