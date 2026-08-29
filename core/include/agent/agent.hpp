/*
 * agent.hpp — agent loop（M3 核心）
 *
 * 形态（ADR-0002/ADR-0007）：while(1)——
 *   调 LLM（造请求 → curl 发出 → 解析 SSE → 计价）→ 广播事件
 *   → 有 tool_use 则顺序执行工具、结果入会话、继续循环；否则完成
 *
 * core 持有抽象对话（JSON），llm 模块负责转成 /v1/messages 的具体形状。
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "agent/context.hpp"
#include "agent/executor.hpp"
#include "agent/session.hpp"
#include "json.hpp"
#include "llm/llm.hpp"

namespace realagent {

class Agents;

/* 一次 LLM 调用的产出 */
struct LlmOutcome {
    std::string text;               // 累积文本（非 tool_use 时）
    std::string thinking;           // 思考内容（流式累积）
    std::string thinking_signature; // thinking 块签名（回传历史用）
    struct ToolUse {
        std::string id;
        std::string name;
        std::string input; // JSON 对象字符串
    };
    std::vector<ToolUse> tool_uses;
    std::string stop_reason;
    std::string error; // 非空 = 本次调用失败的人话原因（广播给客户端）
    double cost = 0;   // 本次调用花掉的钱（USD，绝对值，后到覆盖先到）
};

class Agent {
  public:
    /* workdir 是 agent 干活的地方，**必传、无默认、不从 cwd 取**（ADR-0019）：
     * 会话落在 <workdir>/.realagent/sessions，工具的相对路径也从这里算起。 */
    /* sub = 这个 agent 是别的 agent 派生出来的。会话落点因此不同（ADR-0021）：
     * 客户端建的落 `<workdir>/.realagent/sessions/`，派生的落 `.../sessions/sub/`。
     * **两边都落盘**——不落盘的那份内存里丢不掉（idle 也释放不了），而且出了事查不了。
     * 清单只扫顶层，于是「不在会话列表里显示」不是一个开关，是落点的后果。 */
    Agent(CoreContext &ctx, ApprovalCoordinator &approval, std::string workdir, std::string id,
          Agents *pool = nullptr, bool sub = false);

    const std::string &id() const { return id_; }

    const std::string &workdir() const { return workdir_; }

    /* 会话目录（`<workdir>/.realagent/sessions`）。GET /sessions 扫的就是它——
     * 会话跟着 agent 的工作目录走，不是进程级的（ADR-0019）。 */
    const std::string &session_dir() const { return session_dir_; }

    ~Agent();

    /* 投一条消息进收件箱。三种来源一个队列（ADR-0019）：人发的、别的 agent 发的、
     * 别的 agent 跑完的通知。主循环取下一条时不分辨它是哪来的。 */
    void post(std::string message);

    /* 正在推进一个 Turn（不是 idle）。GET /agents 报状态用。 */
    bool running() const { return running_.load(); }

    /* 事件循环线程要动这个 agent 的历史（/new、/resume、列会话）时先拿这把锁。
     * 拿不到就回一句"忙着呢"，绝不排队等——等的是一次 run 跑完，期间连
     * POST /interrupt 都收不了（ADR-0017）。 */
    std::unique_lock<std::mutex> try_lock() { return {run_mtx_, std::try_to_lock}; }

    /* 中断当前 run（任意线程安全；curl/工具/turn 间均检查） */
    void interrupt();

    /* 内存里此刻留着几条历史。**idle 时是 0**（ADR-0019 §7）。
     * 要问「这个 agent 记了什么」问盘（`Session::read`），不问这里——
     * 这个数只回答「内存里现在占着多少」。 */
    size_t resident() const { return messages_.size(); }

    /* 新建会话（/new 命令）：清空历史 + 换一个会话文件。
     * 旧文件不动——它是记录，不是缓存。 */
    void reset();

    /* 恢复会话（/resume <id>）：JSONL 读回历史，后续追加写进该文件。
     * 读不到返回 false，此时当前会话**原样不动**（宁可恢复失败，也不能把人扔进空白）。*/
    bool resume(const std::string &id);

    /* 当前会话 id（GET /sessions 标出 current，客户端据此知道自己在哪儿） */
    const std::string &session_id() const { return session_.id(); }

    /* SseParser 产出的事件落在这里：累积进 out，该实时广播的顺手广播。
     * public 只因为 libcurl 的写回调是个自由函数，进不来私有区。 */
    void on_llm_event(std::string_view type, const nlohmann::json &ev, LlmOutcome &out,
                      const std::string &model);

  private:
    /* 主循环：从收件箱取下一条，跑到 LLM 不再调工具为止，再取下一条。
     * 收件箱空就阻塞——那就是 idle（ADR-0019）。 */
    void loop();
    /* idle 时历史还给了盘，醒来先读回来。读不到 = 这个会话还没写过盘，
     * 空历史就是对的，不是错。 */
    void ensure_loaded();
    /* 一条消息 → 完整 agent loop（阻塞至 LLM 完成或工具链结束） */
    void run(const std::string &user_input);

    /* 消息入账的唯一入口：进内存，同时落盘。
     * run() 里有六处产生消息，全都走这里——散着写 push_back 迟早漏掉一处，
     * 而漏掉的那条恢复会话时就凭空消失了。 */
    void record(nlohmann::json msg);

    /* 构建抽象对话（system/messages/tools），tier 决定 dialog["model"] 取哪一档 */
    nlohmann::json build_dialog(ModelTier tier) const;
    /* 一次 LLM 调用：build_request → libcurl → SseParser → LlmOutcome */
    bool llm_call(const nlohmann::json &dialog, LlmOutcome &out);
    /* 最后一条 assistant 消息的正文（完成通知带的就是它） */
    std::string last_text() const;
    /* 广播事件（走 CoreContext::emit_fn） */
    void broadcast(const std::string &type, const nlohmann::json &payload);

    CoreContext &ctx_;
    Agents *pool_ = nullptr; // 跑完时沿入边通知邻居；独立构造（测试）时为空
    std::string id_;
    std::string workdir_;
    /* Executor 的 inflight_ / interrupted_ 是**这一个 agent** 的状态，
     * 共享一个就会串味：中断 A 会把 B 的下一次工具调用一起拒掉。 */
    Executor exe_;
    /* 对话历史。**idle 时它是空的**：那份内存是盘上那份的逐字副本
     * （`Session::append` 每条消息即时追加），丢掉不丢信息，醒来重读一遍
     * （ADR-0019 §7）。线程是 16KB，这个是 MB 级——几十个 idle agent 就是几百 MB，
     * 全在等一条可能永远不来的消息。
     * 要问「这个 agent 记了什么」，问盘（`Session::read`），不问这里。 */
    nlohmann::json messages_;
    bool loaded_ = true;      // 新 agent 盘上本来就没有，空历史就是对的
    std::string session_dir_; // <workdir>/.realagent/sessions
    Session session_;         // 当前会话的落盘去处（JSONL，append-only）
    double run_cost_ = 0;     // 本次 run 累计花费（USD），一次用户输入起算清零
    std::atomic<bool> abort_{false};

    // 收件箱与跑它的那条线程。idle 就是阻塞在 cv_ 上——不占 CPU，也不需要"空闲"这个状态
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::string> inbox_;
    bool closing_ = false;
    std::atomic<bool> running_{false};
    std::mutex run_mtx_; // 跑一条消息期间持有，见 try_lock()
    std::thread loop_;
};

} // namespace realagent
