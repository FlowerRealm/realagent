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

「实现」一列对照 core/src/server/quic_server.cpp 的路由分发逐条核实，2026-08-16。

| 端点 | 语义 | 实现 |
|---|---|---|
| `POST /message` | 提交用户消息 → 启动 agent turn。首字符为 `/` 时按斜杠命令处理，不启动 turn（见下「命令」节） | ✅ |
| `POST /command` | 执行斜杠命令，体 `{"command":"/new"}`（命令名带不带 `/` 都认）。与 `POST /message` 的 `/` 前缀分支**共用 core 侧同一份实现**——两个门，一套行为。**agent 正在跑时立刻回 `{"ok":false,"error":"agent 正在运行…"}`**，不排队等（见下「忙碌与中断」） | ✅ |
| `GET /commands` | 斜杠命令列表 `[{name, description}]`（TUI 菜单数据源，core 是唯一真相；`/new` `/resume` `/model`） | ✅ |
| `POST /interrupt` | 中止当前 agent run。**无请求体**，恒返回 `{"status":"ok"}`（不报告当时有没有 run 在跑）。core 侧置 abort 位 + 取消全部待裁决审批（`Agent::interrupt()` + `ApprovalCoordinator::cancel_all()`）。**中止是异步的**：这个 200 只表示信号已置，agent 在下一个检查点才真正停，客户端要等 `interrupted` 帧才算收工。打断范围：LLM 请求、turn 间隙、**以及正在执行的工具**——core 同时把在跑的 bash 进程组打掉（`Executor::interrupt()` → `interrupt_tool()`）。read/edit 跑得快，不设中断点 | ✅ |
| `POST /approval-response` | 审批裁决回传（TUI → core），体 `{"id", "allow"}` | ✅ |
| `GET /statusline` | 状态栏数据（输入框下方那条）：`{"model", "owned_by", "context"}`，后两项来自模型数据表，查不到就只有 model | ✅ |
| `GET /sessions` | 会话清单 `[{id, title, messages, mtime, current}]`，按 `mtime` 倒序 | ✅ |
| `POST /session` | 新建 / 恢复会话：体空 `{}` = 新建，体 `{"id"}` = 恢复。响应 `{"ok", "data"}`（`data` 为更新后的清单）；id 不存在 → `{"ok":false,"error":"unknown session: ..."}`，且**当前会话原样不动** | ✅ |
| `GET /events` | 推送流订阅，见下节 (2)。响应 `200` + `content-type: text/event-stream`，流不关闭 | ✅ |

未匹配任何路由的请求返回 `404` + `{"error":"not found"}`。

### (2) 推送流（HTTP/3 单向流，SSE 语义）

core 为每客户端建立**一条长生命周期推送流**（`GET /events` 流式响应），事件帧按序写入，QUIC 可靠流保证不丢、有序。**打字效果**由帧到达驱动（边写边读，无延迟损失）。

## 帧格式

流中每帧为独立 JSON 对象，SSE event 块语义（`\n` 分隔）：

```
event: <type>
data: <json>

```

「实现」一列对照 core 的全部 emit 点核实（core/src/agent/agent.cpp、core/src/agent/approval.cpp、core/src/main.cpp），2026-08-16。

| type | 载荷 | 说明 | 实现 |
|---|---|---|---|
| `message_update` | delta 文本 | LLM 流式增量 | ✅ |
| `message_start` | 消息结构 | 消息生命周期 | 🟡 只在收到用户输入时发一帧 `{"role":"user"}`；assistant 消息不发 |
| `message_end` | 消息结构 | 消息生命周期 | ❌ 未实现——收工靠 `turn_end` |
| `thinking_start/update/stop` | signature / delta / 空 | 模型思考过程（DeepSeek v4 reasoning），流式增量与 message_update 同语义 | ✅ |
| `tool_output` | `{call_id, stream, text}` | 工具边跑边推的输出（stdout 与 stderr 合流，见下） | ✅ |
| `tool_execution_start/end` | `{name, id}` / `{name, id, status, interrupted}` | 工具生命周期。`interrupted` 为真表示这次是被 `POST /interrupt` 打断的，不是工具自己失败——两者模型的反应完全不同，故分开报 | ✅ |
| `turn_start/end` | 轮次信息 | Turn 生命周期 | ✅ |
| `status_update` | 运行态数据 | 状态行数字（开放键集，见下） | ✅ |
| `statusline` | 状态栏数据 | 会话身份变了就推一帧（见下），与 `GET /statusline` 同一份载荷 | ✅ |
| `permission_request` | 审批问询 | 审批请求（可靠，卡点） | ✅ |
| `interrupted` | 空对象 | `POST /interrupt` 生效——agent 在某个检查点停了。此后本次 run 不再有帧 | ✅ |
| `agent_start/end` | 运行信息 | Agent 生命周期 | ❌ 未实现 |

### statusline 帧

```json
{"model": "deepseek-v4", "owned_by": "deepseek", "context": 131072}
```

`GET /statusline` 的载荷原样推送：客户端启动时 GET 一次拿初值，之后只等这个帧。

- **变了才推**：core 每轮事件循环比对当前载荷，不同才发一帧，相同不发。
- **谁改的不重要**：载荷本身就是信号。改配置的代码路径不需要通知任何人，客户端也不需要知道是谁改的。
- **不做配置文件热重载**（ADR-0010）：core 启动时读一次 `settings.json`，之后不再看它。用户手改配置需重启 core 才生效，改模型的唯一在线途径是 `/model <name>`。
- **当前客户端只消费 `model`**：TUI 的 `client.Statusline` 三个键都解析（tui/internal/client/client.go:203-206），但传给渲染的 `statusMsg` 只带 `model`（tui/cmd/realagent-tui/statusline.go:89-91）；状态栏另两段 dir 与 git 是 TUI 本地算的，不来自本帧。`owned_by` / `context` 因此目前无人渲染。协议保留这两个键——载荷形状是 core 侧的事实，客户端渲染多少是客户端的事。

### tool_output 帧

```json
{"call_id": "call_00_xxx", "stream": "output", "text": "line 1\n"}
```

- **不与 tool_result 二选一**：完整输出照旧随工具结果回到模型那边，这里推的只是"现在长什么样"，给的是人看的实时反馈。
- `call_id` 是认领凭据——就是 `tool_execution_start` 那个 `id`，客户端靠它把碎片挂到对应那次调用下面。
- **按行推，但不保证一帧一行**：超长行会被切成几帧，最后一段可能没有换行符。客户端要按"续写开着的行"处理（TUI 走 `stream`，见 tui/cmd/realagent-tui/main.go 的 `tool_output` 分支），不能假设一帧即一行。
- **一条流，不分家**：`stream` 恒为 `output`——bash 的 stdout 与 stderr 接在同一个管道上（ADR-0017）。报错原文是模型判断该不该重试的唯一依据，只推 stdout 等于把它扔了；合流而不是两个管道，是因为交错顺序就是人在终端里看见的顺序，而"哪一行来自哪条流"没有第二个读者。
- **谁推谁负责**：帧由工具自己发出（bash 的读循环：`core/src/tools/tools.cpp`），不具备实时输出的工具就不发这个帧。
- 输出超上限后**只吞不推**（core-tools 为 `MAX_OUT`）：管道仍要读干净，中途撒手等于给命令一个 SIGPIPE。

### status_update 帧

```json
{"cost": 0.0123}
```

状态行（读秒行）的数据源。**开放键集**：core 除 `cost` 外一律不解释、原样转发，客户端渲染认识的键、忽略其余。将来加"已用上下文"一类数字，零协议改动。

- **本次 run 累计**：一次用户输入触发的全部 turn 之和，`POST /message` 起算清零。多 turn 会重发完整历史，花费因此逐轮膨胀——这是真实计费口径，core 不做修正。
- **绝对值，非增量**：客户端覆盖写即可，不累加。丢帧不产生永久偏差。
- **钱在 core 里算**（ADR-0009）：SSE 解析出的 token 用量按本次模型查模型数据表换成金额。**token 不跨客户端边界**——客户端收到的只有钱，单价表它一眼都没见过。
- 算不出钱（表里没这个模型、端点不报用量）就不发 `cost`，客户端按"无数据"处理，不显示 $0。
- 每轮至少两帧（`message_start` 后一帧、`message_delta` 后一帧），后者**先于** `stop` 送出，保证客户端收工时数字已定。

完整 message / tool_result 作为独立帧或结束帧，与增量同流保证最终一致。

## 忙碌与中断

`POST /command`、`GET /sessions`、`POST /session` 这三个端点要动对话历史，与 agent 运行互斥。
它们跑在事件循环那唯一一条线程上，所以**拿不到锁就当场回绝，绝不排队等**（ADR-0017）：

```json
{"ok": false, "error": "agent 正在运行——先中断（Esc / POST /interrupt）再执行这条命令"}
```

在那儿等锁，等的是"一次 run 跑完"——期间 core 收不了任何请求，
**包括那个唯一能把 run 停下来的 `POST /interrupt`**，也推不出任何事件帧，
客户端表现为整个假死。一次拒绝是一句话，一次假死是没有话。

`POST /interrupt` 不碰这把锁，所以任何时候都进得来——那是唯一的逃生通道，它必须永远畅通。

## 配置没配齐

`POST /message` 在端点那一束（`protocol` / `base_url` / `model`）缺键时不启动 agent，
直接回 `{"error": "<多行人话>"}`：缺哪个报哪个、列出 `protocol` 的三个可选值、
附一段能直接抄的 `settings.json`（ADR-0017）。

core **不会**因为配置没配齐而拒绝启动——那样客户端只会看见"连不上"，比配错了更难查。

## 命令

斜杠命令有两个入口，**core 侧是同一份实现**：

- `POST /message` 的 `message` 字段以 `/` 开头 —— 交互式客户端的自然路径（用户就在输入框里打）；
- `POST /command`，体 `{"command":"/new"}` —— 给不走消息框的调用方（脚本、未来的 gui 按钮）。

两者都直接返回命令结果 JSON（`{"ok":true,"command":...}`），**不启动 agent turn**。未识别命令返回 `{"error":"unknown command"}`。

存在两个入口是历史形态（v1 曾打算只留前者），不是两套行为——真要改命令语义，改的永远只有一处。

### 命令一览

首个空白分词为命令名，其后是参数。与独立端点返回同一数据形状：

| 斜杠命令 | 行为 | 返回 |
|---|---|---|
| `/new` | 新建会话：清空历史 + 换一个 JSONL 文件（旧会话留在盘上） | `{"ok":true,"command":"new","data":[会话清单]}` |
| `/resume` | 查看会话清单（`current` 标出自己在哪儿） | `{"ok":true,"command":"resume","data":[{id,title,messages,mtime,current}]}` |
| `/resume <id>` | 恢复某个会话（JSONL 读回历史，后续追加写进该文件） | 同上；id 不存在 → `{"ok":false,"error":"unknown session: ..."}`，**当前会话不动** |
| `/model` | 查看模型数据表里的清单 | `{"ok":true,"command":"model","data":[{name,owned_by,context,current}]}` |
| `/model <name>` | 切换主模型（写回 settings.json，下次调用即生效） | 同上（`data` 为更新后清单）；模型不在表里 → `{"ok":false,"error":"unknown model: ..."}` |
| 失败 | — | `{"ok":false,"command":"...","error":"..."}` |

**纯客户端命令不在此表**：`/quit`（退出客户端进程）与 `/statusline`（展示偏好）由 TUI 就地处理，从不发给 core——core 是常驻服务、还连着别的客户端，它没有"退出"这个概念，也不认展示偏好。它们照常出现在斜杠菜单里，用户分不出、也不需要分。

## 握手

- 首次连接：QUIC 1-RTT（TLS 1.3）。
- 重复连接：**0-RTT**——握手与首个请求同发（客户端缓存会话票据）。

## 与会话持久化的关系

会话 JSONL 与推送流是两件事：**JSONL 是持久化的对话记录**（append-only），**推送流是实时事件传输**（`message_update` / `tool_output` / …）。恢复会话时从 JSONL 重建上下文，重建之后产生的事件才进推送流。

一行 JSONL = core 抽象对话里的一条消息，**原样**（`{"role":..., "content":[...]}`，content 块即 `text` / `thinking` / `tool_use` / `tool_result`）。工具调用与结果的关联本来就在块里（`tool_use.id` / `tool_result.tool_use_id`），不另加信封。

> 早先本节记的是"JSONL 的 `type` 字段分 user/assistant/tool_call/tool_result"——那是抽象对话形状定下来之前的设计。落地时改为同形存储：只要盘上与内存不是一个形状，就要写一对转换函数，而转换函数正是丢字段的地方（thinking 的 `signature`、一条 assistant 消息里的多个 `tool_use`、将来任何新块类型）。同形则恢复即读取，没有转换、没有版本漂移。落盘路径 `.realagent/sessions/<id>.jsonl`（相对 cwd，按项目分家；不是配置项）。

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
