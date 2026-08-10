# 通信协议（core ↔ 客户端）

> core（C++）与客户端（TUI/未来 gui）的 QUIC/HTTP3 通信契约。
> 依据 ADR-0006（服务化）+ ADR-0007（TUI Go）。2026-08-09 定稿（v2，全可靠流）。
> **设计演进史见文末**——理解"为什么是全可靠流"必读。

## 设计原则

- **所有推送一律可靠**——增量与结构化事件无差别对待，丢弃任何一种都致命：
  - LLM 增量丢一整个段落，用户直接看不懂；
  - turn_end 丢失，整个事件流状态机断裂（灾难性的）。
  因此**不存在"可丢预览"**。
- **用对传输层**：可靠性交给 QUIC 可靠流原生保证，**不自建确认机制**（无水位/无 ACK/无捎带重发/无发送队列）。
- **快握手 + 少校验**：QUIC 0-RTT（重复连接握手与数据同发），无自建校验开销。

## 通道模型

```
TUI ──(1) HTTP/3 请求-响应──▶ core      双向交互（提交/查询/裁决）
TUI ◀──(2) 长生命周期单向流────────────  core      推送流（SSE 语义，可靠有序）
```

### (1) 请求-响应（HTTP/3 标准）

| 端点 | 语义 |
|---|---|
| `POST /message` | 提交用户消息 → 启动 agent turn |
| `POST /command` | 执行命令（/new /resume 等） |
| `POST /approval-response` | 审批裁决回传（TUI → core） |
| `GET /sessions` / `POST /session` | 会话列表 / 新建-恢复 |

### (2) 推送流（HTTP/3 单向流，SSE 语义）

core 为每客户端建立**一条长生命周期推送流**（`GET /events` 流式响应），事件帧按序写入，QUIC 可靠流保证不丢、有序。**打字效果**由帧到达驱动（边写边读，无延迟损失）。

## 帧格式

流中每帧为独立 JSON 对象，SSE event 块语义（`\n` 分隔）：

```
event: <type>
data: <json>

```

| type | 载荷 | 说明 |
|---|---|---|
| `message_update` | delta 文本 | LLM 流式增量 |
| `message_start/end` | 消息结构 | 消息生命周期 |
| `tool_output` | stdout 行 | bash 实时输出 |
| `tool_execution_start/end` | 工具信息 | 工具生命周期 |
| `turn_start/end` | 轮次信息 | Turn 生命周期 |
| `permission_request` | 审批问询 | 审批请求（可靠，卡点） |
| `agent_start/end` | 运行信息 | Agent 生命周期 |

完整 message / tool_result 作为独立帧或结束帧，与增量同流保证最终一致。

## 命令（v1）

`POST /command` 端点（`GET /sessions` / `POST /session` 同理）为完整会话管理的最终形态。首版不实现独立端点：斜杠命令（`/new`、`/resume`）由 `POST /message` 的 `message` 字段以 `/` 前缀触发，core 直接返回命令结果 JSON（`{"ok":true,"command":...}`），不启动 agent turn。未识别命令返回 `{"error":"unknown command"}`。`/new` 清空当前会话；`/resume` 首版仅返回会话消息数（`messages`），JSONL 恢复后置。

## 握手

- 首次连接：QUIC 1-RTT（TLS 1.3）。
- 重复连接：**0-RTT**——握手与首个请求同发（客户端缓存会话票据）。

## 与会话持久化的关系

会话 JSONL（CONTEXT.md）的 `type` 字段与推送流帧类型对齐但**不同**——JSONL 是持久化对话记录（user/assistant/tool_call/tool_result，append-only），推送流是实时事件传输（message_update/tool_output/...）。两者各自服务持久化与实时渲染，恢复会话时从 JSONL 重建上下文，重建后产生的事件再进推送流。

---

## 设计演进史（必读）

### v0 起点：HTTP/1.1 + SSE

最初引入 HTTP 连接时：core 为 HTTP 服务，事件流走 SSE（HTTP/1.1 可靠传输）。动机：语言解放、进程隔离、多客户端。

### v1：QUIC 数据报 + 分层可靠性

引入 QUIC（公网需求）后，设计为**分层可靠性**：
- 可靠类（message / 审批 / 命令）→ HTTP/3 可靠流；
- 尽力类（流式增量）→ QUIC 不可靠数据报（RFC 9221）。

用户提出**时间戳水位 + 捎带发送**机制保证增量最终到达：
- core 每客户端维护发送队列，帧 = `{timestamp, type, payload(json)}`（**帧可为任意结构化 JSON 载荷，不限于文本**）；
- 发送循环：从队首推进，未发帧发出，**已发未确认帧保留**；
- **不设专门重发定时器**：下一次任何发送动作（新增量 / 水位处理 / 周期 tick）**捎带发送全部未确认帧**；
- **上游（LLM 流 / 工具执行）结束后**：最后一次发送清空队列（补发未确认 + 结束标记）；
- 客户端回报**时间戳水位**（"已收到到 T"），core 将 `timestamp ≤ T` 的帧判过期移除——**唯一删除路径**。

用户对机制的澄清："增量丢了也有事, 走我的办法"、"所有丢了都有事"、"一个队列里能装的不是只有char, 也可以是个class/json"。

### v2（最终）：全可靠流

用户质疑："按照我的数据传输方法似乎更像一个TCP? QUIC+HTTP/3的设计真的是对的吗?"

**分析结论**：时间戳水位 = TCP 累积 ACK；捎带发送 = TCP 捎带确认/延迟确认；未确认帧保留重发 = TCP RTO 重传。该机制**本质是在 QUIC 不可靠数据报上重复实现 TCP 的可靠传输**——放着现成的 QUIC 可靠流不用，工作量重复、性能更差。

用户进一步确认："没有两种类型, 丢了哪一种的都很致命"——增量不可丢、事件更不可丢。

**最终方案**：全部推送走 QUIC 可靠流（原生可靠有序），废弃数据报 + 水位 + 捎带 + 发送队列。增量在同一流里（可靠流支持流式实时，打字效果不损失）。QUIC 的选择保留——**"更快的握手, 更少的校验"正是 QUIC 相对 TCP 的核心优势**（0-RTT vs TCP+TLS 的 1-RTT + 1-RTT；可靠流免自建校验）。
