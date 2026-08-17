/*
 * session.hpp — 会话持久化（JSONL，PLAN.md R9）
 *
 * 一个会话 = 一个文件：`.realagent/sessions/<id>.jsonl`，一行一条消息，append-only。
 * 人可读、可 diff、可进 git（CONTEXT.md [[Session]]）。
 *
 * **一行就是 messages_ 里的那一条，原样**——没有信封（无外层 id/ts/type 包装）。
 * 这是刻意的：会话的用处只有一个，把历史原样还给模型；只要落盘与内存不是同一个形状，
 * 就要写一对转换函数，而转换函数是丢信息的地方（thinking 的 signature、一条 assistant
 * 消息里的多个 tool_use、将来任何新的 content block 类型）。同形则恢复即读取，
 * 没有转换、没有版本漂移、没有特殊情况。
 * `call_id` 关联本来就在块里（`tool_use.id` / `tool_result.tool_use_id`），不必再包一层。
 *
 * 清单元数据（时间、条数、标题）**不另存**：文件名即 id 且按时间可排序，条数是行数，
 * 标题是第一条 user 消息的正文。没有第二份真相，也就没有对不上的那天。
 */
#pragma once

#include <string>
#include <vector>

#include "json.hpp"

namespace realagent {

/* 会话清单一条（GET /sessions）。字段名与顺序即 PROTOCOL.md 的响应契约。 */
struct SessionInfo {
    std::string id;         // 文件名去掉 .jsonl，形如 20260816-143022-a1b2
    std::string title;      // 第一条 user 消息的正文（截断），空会话为空串
    long long messages = 0; // 消息条数 = 行数
    long long mtime = 0;    // 最后写入时间（Unix 秒），列表排序用
};
BOOST_DESCRIBE_STRUCT(SessionInfo, (), (id, title, messages, mtime))

/* 一个打开着的会话。整个类只做"往一个文件后面追加"这一件事。 */
class Session {
public:
    /* 新会话：生成 id，**不建文件**——空会话不该在清单里占一行。
     * 文件在第一次 append 时才出现。 */
    Session();

    const std::string& id() const { return id_; }

    /* 追加一条消息（一行 JSON + '\n'）。写不进去只报 stderr，不打断对话：
     * 落盘失败是运维问题，不是让用户这轮白说的理由。 */
    void append(const json& msg);

    /* 切到一个已有会话：id 指向的文件读进 out，后续 append 续写该文件。
     * 读不到返回 false，此时本对象**不变**（还是原来那个会话）。 */
    bool resume(const std::string& id, json& out);

    /* 扫描会话目录 → 清单，按 mtime 倒序（最近的在前）。目录不存在返回空。 */
    static std::vector<SessionInfo> list();

private:
    std::string id_;
    std::string path_; // 由 id_ 算出，缓存一份省得每次拼
};

} // namespace realagent
