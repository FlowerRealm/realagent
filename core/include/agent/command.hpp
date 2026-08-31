/*
 * command.hpp — 斜杠命令（/new、/resume、/model、/provider）
 *
 * 两个门通向这里：`POST /message` 的 `/` 前缀、`POST /command`。一份实现——
 * 两份迟早只改一边，那时同一条命令在两个端点上行为不同，谁都查不出为什么。
 * 命令不投收件箱，直接返回结果。
 *
 * 命令集只有一张表（command.cpp 里的 kCommands）：`GET /commands` 列的是它，
 * 派发认的也是它。加一条命令就是加一行，不存在"两处同步"这个义务。
 */
#pragma once

#include <string>

#include "agent/agent.hpp"
#include "agent/agents.hpp"
#include "agent/context.hpp"
#include "json.hpp"

namespace realagent {

/* 事件循环线程拿不到 agent 的锁时回的那句话。
 *
 * 这些回调跑在事件循环那唯一一条线程上（quic_server.cpp 的 poll 循环）。
 * 在那儿阻塞等锁，等的是"一次 run 跑完"——期间收不了任何请求，
 * 包括那个唯一能把 run 停下来的 POST /interrupt，也推不出任何事件帧。
 * 用户打一条 /new 想放弃当前任务，换来的是整个客户端假死到 run 自己结束。
 *
 * 所以宁可当场说"忙着呢"：拿不到锁就回一句人话，让他先中断。
 * 一次拒绝是一句话，一次假死是没有话。 */
inline constexpr const char *AGENT_BUSY =
    "agent 正在运行——先中断（Esc / POST /interrupt）再执行这条命令";

/* 失败载荷：{"ok":false,"error":msg} */
std::string command_error(const std::string &msg);

/* 斜杠命令清单（GET /commands，TUI 菜单数据源）。core 是唯一真相源。 */
nlohmann::json command_defs();

/* 会话清单（GET /sessions、/new、/resume 共用）：盘上有哪些会话，以及每一个被谁打开着。
 *
 * **`current: bool` 换成 `opened_by`（ADR-0019 §10）**：多 agent 之后「当前」没有主语了，
 * 同一个目录下可以有 N 个 agent 各自打开着一个会话。一个会话要么被某个 agent 打开着，
 * 要么躺在盘上——`opened_by` 直接说的就是这句话，不需要客户端再去问「谁的当前」。 */
nlohmann::json sessions_payload(const Agents &pool, const Agent &agent);

/* 状态栏载荷：配的模型名 + 数据表里查到的元数据（GET /statusline、statusline 帧）。 */
nlohmann::json statusline_payload(const CoreContext &ctx);

/* 执行一条斜杠命令（input 带前导 `/`），返回响应 JSON 字符串。
 * agent 的锁在这里面拿：拿不到就回 AGENT_BUSY，不排队等。
 *
 * `data` 是结构化参数（ADR-0023 §6），`POST /command` 体里的同名字段，不传就是 null。
 * 表单类命令（/provider、/model）走它：四个字段在客户端已经是分好的字符串，
 * 序列化成一行文本再让 core 解析回来，中间凭空多出一整套引号规则——
 * api_key 里可以有空格，base_url 里可以有 `=`。那不是边界情况，是编码往返自己制造的。 */
std::string handle_command(CoreContext &ctx, Agents &pool, Agent &agent,
                           const std::string &input,
                           const nlohmann::json &data = nlohmann::json());

} // namespace realagent
