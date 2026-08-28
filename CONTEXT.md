# CONTEXT.md — realagent 术语表

> 本文件只收录本项目的领域术语。通用编程概念不收录。
> 定义描述事物"是什么"，不描述"做什么"。

## Language

### **Agent Loop（代理循环）**:

一次对话的驱动核心。一个 **Turn**（轮次）= 一次 LLM 调用 + 该调用产生的所有工具执行；LLM 继续调用工具则进入下一 Turn，直到 LLM 停止调用工具为止。

_Avoid_: `agentic loop`、`对话循环`

### **Turn（轮次）**:

一次 LLM 调用及其后续的全部工具执行。Turn 是 Loop 的最小推进单位。

_Avoid_: `iteration`、`round`

### **Tool（工具）**:

LLM 可调用的具名函数。带名称、描述、参数 Schema（JSON Schema）、危险标记，执行后返回结构化结果。core 内置静态表 `read` / `edit` / `bash`（`core/src/tools/tools.cpp`），LLM 见到的就是这三个短名。

_Avoid_: `function`、`command`（Tool ≠ 用户斜杠命令）

### **Event（事件）**:

Loop 向客户端发布的生命周期消息。典型序列：
`agent_start → turn_start → message_start → message_update* → message_end → tool_execution_start → tool_execution_end → turn_end → agent_end`

事件是**异步事件流**的一部分（ADR-0002）：生产方与消费方解耦。异步引擎的动机是**多 agent 并发编排**，而非单 agent 工具并行。

出口只有一个：agent 线程 `emit` 入队，事件循环线程 flush 到推送流。没有扇出、没有订阅者——[[ADR-0016]] 之后事件的去处只有客户端。

_Avoid_: `callback`（事件是广播的，回调是一对一的）、`订阅`（已无插件订阅者）

### **Multi-Agent（多代理编排）**:

异步事件流的核心动机：多个 agent 实例并发调度（如主 agent 派生子任务）。每个 agent 内部仍保持工具严格顺序执行。

_Avoid_: `sub-agent`（特指派生出来的子 agent，Multi-Agent 是泛指）

### **Background（后台运行）**:

长时工具（bash 等）的规划机制：工具可在后台运行，agent 不阻塞等待，用户可查看进度。未来并入，不属于第一阶段核心。

_Avoid_: `async tool`（工具本身是同步顺序执行的，后台只是运行策略）

### **Provider（模型提供商）**:

LLM 后端。**供应商身份不是一个概念**（ADR-0016）：core 认的是 [[Protocol]]，不是公司。换一家就是改配置里的[[端点束]]——`protocol` / `base_url` / `model` 三个键，外加凭证 `api_key`。core 里没有任何一处代码能回答「对面是哪一家」，也不需要能。

从前这里是一层插件抽象（ADR-0004 的 Provider 壳：兜底端点、兜底模型名、计价），代价是把「本次用的是哪个模型」变成壳在两次调用之间偷偷记的一笔账。现在计价直接收模型名参数，那个特殊情况随抽象一起消失。

_Avoid_: `backend`、`LLM client`、`Provider 壳`、`套壳`（已废除）

### **Model Tier（模型档位）**:

一次 LLM 调用用哪一档模型：**主模型**（`model`，对话主链路）或**小模型**（`small_model`，标题/摘要一类杂活）。档位只是模型名的选择，两档共用同一 base_url / api_key ——不是独立的供应商配置。`Config::model(ModelTier)` 解析档位，结果经 `dialog["model"]` 传给 `build_request`，协议层对档位无感。**不做档位间回落**：回落会让「我配了小模型」与「我没配所以用了主模型」长得一模一样，出账单时才发现区别。

_Avoid_: `fast model`、`模型 profile`（档位不带端点凭证，不构成完整 profile）

### **Protocol（协议）**:

对面那个端点说哪套话。core 支持三套，由用户在配置里明选（ADR-0017）：`anthropic-messages` / `openai-chat` / `openai-responses`。协议**不是公司**——`anthropic-messages` 由 Anthropic、DeepSeek、OpenRouter 等多家实现，`openai-chat` 的实现者更多。

一套协议包含四样共变的东西：**认证头、URL 路径、请求体形状、响应帧结构**（外加 token 字段名）。它们绑成一束由一个 `protocol` 值选定，不拆成四个开关——拆开就能配出无效组合（Bearer 头配 OpenAI 请求体配 `/v1/messages` 路径）。

协议知识按方向分家：`llm/upstream/<协议>.cpp` 管造请求，`llm/downstream/<协议>.cpp` 管解帧。**产出的[[Event]]词汇表只有一套**——协议是三套，[[Agent Loop]]与 TUI 不跟着分三份。

_Avoid_: `anthropic api`（Anthropic 是一家公司，协议本身供应商中立）、`v1-messages`（那只是三套里的一套，不是协议层本身）、`provider 协议`（协议不属于任何供应商）

### **端点束（Endpoint Bundle）**:

`protocol` / `base_url` / `model` 三个配置键的合称。**它们没有默认值，必须用户填**（ADR-0017）：不填完全用不了，而填错产生的报错（端点 404、请求体形状不认、流解析出空）是最难自己诊断的一类——给默认值等于替用户猜，猜错了他还以为是自己配的。

缺键不让 core 退出（core 一退，TUI 只会说「连不上」，更难诊断），而是启动日志喊一遍 + 任何一次 `POST /message` 原样回那段话：缺哪个报哪个、一次报全、附可直接抄的样例。

与它相对的是**有默认值的键**（`api_key` / `small_model` / `permission`）：不填也能用，或者不填时的正确行为是明确的（`permission` 缺省 `ask` 是安全默认，不是猜）。

_Avoid_: `必需配置`（"必需"听起来像 core 在校验，实际是 llm 模块在回答"打一次调用需要什么"）、`默认端点`（不存在这东西）

### **管线（Pipeline）**:

一次 LLM 调用被 core 拆成前后相接的几步，上一步的产物就是下一步的入参：

```
对话 ──build_request──▶ 请求 ──[core 用 libcurl 发出]──▶ 响应流
      （upstream/<协议>）                                   │
       事件 ◀──Pricing::cost── usage 事件 ◀──feed_block ────┘
                                        （downstream/<协议>）
```

从前这是四段（生成请求 → 改请求 → 解析 → 计价），因为「协议固有的」与「供应商身份」
住在两个动态库里，中间必须留一道缝让后者补前者的空。ADR-0016 之后那道缝没有了：
`build_request` 直接读配置里的端点与凭证，一次造出能发的请求。**改请求这一段整段消失**。

段数不因协议而变：三套 [[Protocol]] 换的是每一段**怎么做**，不是**有哪几段**。

管线的段数**明写在 core 流程里**。新增一段（例如上下文压缩）就是加一次调用。
**不引入通用钩子/中间件机制**：那会让"经过哪几段、什么顺序"变成运行时才知道的事，
而顺序恰恰是本项目一路在消灭的隐式仲裁。

_Avoid_: `嵌套`、`套壳`、`装饰器`、`claim`、`接管`、`改请求`、`粗请求/精请求`（均已废除）

### **Module（模块）**:

core 内部的职责分区（`llm` / `agent` / `tools` / `server`），以目录 + 命名空间划分。`llm` 内部再按**方向**分 `upstream/` 与 `downstream/`，每个方向下一个协议一个文件（ADR-0017）。`extension` 模块随插件系统一起删除（ADR-0016）。

_Avoid_: `package`、`library`（指 core 内部模块时）

### **Session（会话）**:

一段连续的对话记录，含消息历史、状态、元数据。以 **JSONL 文件**持久化（一行一条消息/事件），目录结构支持会话树（分支/fork）。人可读、可 diff、可进 git。

_Avoid_: `conversation`（除非与 Session 区分出明确差异）

> 实况注（2026-08-16）：已落地（`core/src/agent/session.cpp`），落点 `.realagent/sessions/<id>.jsonl`（相对 cwd，会话按项目分家）。id 形如 `20260816-153739-31d3`——时间戳 + 4 位随机，字典序即时间序，所以清单不需要索引文件。
> **一行 = 抽象对话里的那一条消息，原样**（`{"role":..., "content":[...]}`），没有外层信封。早先设计的"type 分 user/assistant/tool_call/tool_result + id/ts/model 信封"没有采用：那是抽象对话形状定下来之前的设计，不同形就得写一对转换函数，而转换函数正是丢字段的地方（thinking 的 signature、一条 assistant 消息里的多个 tool_use）。call_id 关联本来就在块里。
> 清单元数据也不另存：条数 = 行数，标题 = 第一条 user 消息现取，时间 = 文件 mtime——没有第二份真相，也就没有对不上的那天。
> [[Session Tree]]（fork/分支）仍未做，第一版范围外。

### **Session Tree（会话树）**:

会话间的父子关系，支持从任一会话 fork 分支。JSONL 文件目录结构天然表达。

_Avoid_: `branch`（Git 语义混淆）

### **Thinking（思考）**:

assistant 消息中的一种 content block 类型（`{"type":"thinking","thinking":...,"signature":...}`），承载模型的推理过程（DeepSeek v4 reasoning）。抽象对话里原样保留，`llm` 模块负责与协议的 `thinking` 块互转；core 流式转发 `thinking_update` 事件，TUI 渲染为 dim 斜体块。signature 用于回传历史时校验，缺失时省略。

_Avoid_: `reasoning_content`（DeepSeek 原生 API 字段名，Anthropic 兼容端点映射为 thinking 块）

### **Model（模型元数据）**:

一个模型"是什么"：名称、归属供应商、上下文窗口。**不含计价**——单价留在 [[模型数据表]] 里，客户端只收 [[Cost]] 的最终数字。

与 [[Model Tier]] 划清：Tier 是"这次调用用哪一档"，Model 是"那一档指向的模型是什么货色"。

数据来自 [[模型数据表]]，不是 settings.json：`Config` 管的是"用哪个模型"，数据表管的是"那个模型是什么货色"。**单价不进公开清单**——客户端不算钱，给了没有消费方。

同一个模型有多条到达路径（DeepSeek 直连 / OpenRouter / 内网中转），上下文窗口相同但端点、凭证、单价各异。这不构成冲突：**一次运行只有一个 `base_url`**，表里配的就是这条路径的价。换路径就换 `base_url`，要换价就换表。

core 不校验用户配的模型名在不在表里——**表是参考资料，不是白名单**：配了表外的模型照发不误，不检查、不警告、不兜底（只是算不出钱）。`/model` 交互式选择是另一回事，那里只能从已知的里挑。

_Avoid_: `模型清单`（指单个模型时）、`provider model`

### **模型数据表（models.json）**:

含单价在内的模型数据。两个来源，**不合并**：编译进二进制的出厂表随 core 升级更新；`~/.realagent/models.json` 是用户接管版，**存在即整表接管**，出厂表一概不看。半份表比没有表更难查，所以改一个模型的单价就得连表一起接管。

解析严格：缺字段、格式错即报错原文，不跳过坏条目、不补默认值。报错时不留半份表，core 照常启动——没有表只是不算钱，不是不能对话。

公开出去的只有 `name` / `owned_by` / `context`，**单价不出 core**。

_Avoid_: `模型配置`（它不在 settings.json 里，是另一份数据）

### **Cost（花费）**:

一次 run 花掉的钱（USD）。按本次调用的模型查[[模型数据表]]算出——**单价与 token 用量都不出 core**，客户端只收最终数字。core 按 agent 实例跨 turn 累加，随 [[status_update]] 帧下发；一次用户输入起算清零，帧内为累计绝对值。

_Avoid_: `usage`、`token 统计`（客户端侧无 token 概念，只有钱）

### **status_update（运行态帧）**:

core 向客户端报运行态数据的推送帧，**开放键集**：core 除 `cost`（需累加）外一律不解释、原样转发。落点是 [[Status（状态行）]]——本次 run 的实时数字，run 结束即消失。

_Avoid_: `cost 帧`（单值帧型是死路，加第二个值就得破坏客户端）

### **Statusline（状态栏）**:

输入框**下方**的常驻栏：`🤖 model | 📁 dir | 🌿 git`。内容是**这次会话的身份信息**——哪个模型、哪个目录、哪个分支。目录与分支会话期内不变，启动拿一次；模型会被 `/model` 切档改，改了由 core 推 [[statusline]] 帧覆盖写——客户端不轮询，也不关心是谁改的。推帧的触发是**主循环比对载荷**：事件循环每轮算一次 statusline 载荷，与上次推送的不同才推。因此改配置的代码路径不需要记得通知谁，与状态栏无关的配置变更也不会白推一帧。展示偏好（显示哪几段、emoji 还是 nerd font）纯客户端状态，core 不认。

_Avoid_: `状态行`（那是活动区里的另一条，见下）

### **Status（状态行）**:

[[活动区（Live Region）]]里的读秒行：`⠋ 思考中… (1m23s · $0.0123 · esc 中断)`。内容是**本次 run 的实时数字**（读秒、[[Cost]]），数据经 [[status_update]] 帧推送。run 结束整行消失——它描述"正在发生的事"，事结束了就没有它。

_Avoid_: `statusline`（那是输入框下方那条常驻栏）

### **活动区（Live Region）**:

TUI 底部由 Bubble Tea 每帧重绘的区域：未定型行 + 审批框 + [[子面板（Panel）]] + 斜杠菜单 + 读秒状态行 + 输入框。其上方是**终端原生 scrollback**——已定型的行由 TUI 打进去后不再归 TUI 管（[[ADR-0008]]）。活动区绝不能高过终端。

_Avoid_: `视口`、`viewport`（本项目不自建滚动缓冲）

### **子面板（Panel）**:

斜杠命令的第二层选择（参考 codex cli）：`/model` `/resume` `/statusline` 无参执行后，用同一份结果载荷开一列可选项——↑/↓ 选择、Enter 确认、Esc 取消，模态（开着时按键只喂它）。**确认 = 把该项的整条命令写进输入框走正常提交**，没有第二套提交路径；面板也不新增端点，数据就是命令回包里的 `data`。造不出面板（无数据/坏载荷）就退回文本清单。

### **定型（Freeze）**:

一行渲染内容不再变化、可以打进 scrollback 的状态。判据只有一条：**追加式文本的贪心折行前缀稳定**，故除最后一个折行外全部定型（[[ADR-0008]]）。

_Avoid_: `finalize`（原指把 streaming 消息收进列表，行模型下已无此概念）

---

## Relationships

- 一个 **Agent Loop** 由一个或多个 **Turn** 推进；一个 **Turn** = 一次 LLM 调用 + 该调用产生的全部 **Tool** 执行
- 一次 LLM 调用走一条 **管线**：造请求 → core 发出 → 解析响应 → 计价，每段一个函数，没有可插拔点
- 一份**模型数据表**据此可报 0 到 N 个 **Model**；表有两个来源（出厂 / 用户接管），**不合并**
- 一个 **Model Tier** 解析为一个模型名，经 `dialog["model"]` 传给造请求那一段；协议层不感知档位
- **Cost** 按本次模型查**模型数据表**算出，经 **status_update** 帧下发，落点是 **Status（状态行）**——不是 **Statusline（状态栏）**
- **Tool** 是 core 里的一张静态表；`dangerous` 的那些经 `permission` 配置裁决后才执行

## Example dialogue

> **A：** 我想换成 OpenRouter，要装个什么吗？
> **B：** 不用装东西，改 `base_url` 和 `api_key` 两行。`/v1/messages` 是公共协议，core 那份实现认不出对面是谁。
> **A：** 那价怎么算？OpenRouter 的价跟 DeepSeek 直连不一样。
> **B：** 写一份 `~/.realagent/models.json`，它存在就整表接管出厂表。不写也能跑，只是算不出钱、不发 `cost`——不会因此发个 $0 骗你。
> **A：** 从前那个 `/provider` 命令呢？
> **B：** 删了。它切的是"哪个供应商壳上线"，而供应商壳这个东西已经不存在了（[[ADR-0016]]）——现在"哪家"就是 `base_url` 那一行字，没有第二处真相需要同步。
> **A：** 那我要给 LLM 加个自己的工具呢？
> **B：** 往 `core/src/tools/tools.cpp` 的静态表里加一条，写个函数。从前这要编一个动态库、写 100 行 ABI 样板、给工具名加 `<容器名>_` 前缀——那条路服务过 0 个第三方，所以拆了。

## Flagged ambiguities

- **"statusline" / "状态行"**指两个不同的东西（输入框下方常驻栏 vs 活动区读秒行）——已解决：**Statusline** 与 **Status** 分列，各自的 _Avoid_ 互指。
- **"模型重名"**曾被当作冲突（后写覆盖）——已解决：一次运行只有一个端点，表里配的就是这条路径的价，不存在两条路径压进一个键的场合（见 [[Model]]）。
- **整套插件词汇**（插件 / 容器 / 能力 / 能力槽 / 借阅 / 转移 / 命名空间前缀 / Provider 壳 / 当前 provider / 依赖 DAG / plugin.json）——**已随 [[ADR-0016]] 整体作废**，2026-08-25 从本表删除。
  这些词曾经很精确，也确实解决过它们要解决的问题（`type` 之争、套壳与 `claim`、注册表副本、静默仲裁）。作废不是因为它们错了，是因为**它们描述的那个东西一个用户都没有**：5 个容器全是本项目自己写的、与 core 同源发布。词汇跟着代码走，代码没了词也就没了。
  想看它们长什么样：ADR-0001 / 0004 / 0011 / 0012 / 0013 / 0014 / 0015 正文都在，只是顶上多了一行 Superseded。

## 已拍板（暂存，随决策更新）

- 项目定位：AI 编码 agent，第一阶段 core + tui（均 C++），gui 后续。
  - 实况注（2026-08-16）：**TUI 不是 C++，是 Go + Bubble Tea**（ADR-0007，见本节 TUI 条）。"均 C++"是 ADR-0007 之前的设想，未随之更新。core 是 C++。
- 架构基调：极简核心，参考 Pi（earendil-works/pi）。
  - 实况注（2026-08-25）：**"+ 插件/扩展架构"已删**（[[ADR-0016]]）。极简核心这半句留着，而且更成立了——插件机制本身就是那个不极简的部分。
- core 分层参考：ai（Provider 抽象）→ agent（Loop/状态/事件）→ tools（注册与执行）→ extension（扩展宿主）。
  - 实况注（2026-08-25）：四层最终落成的是 `llm/` `agent/` `tools/` `server/`。`extension/`（扩展宿主）随 [[ADR-0016]] 删除；`ai/` 与 `tools/` 在插件时代一度是空的（都外移进了容器），现在它们有实体了——`llm/` 就是当初设想的 ai 层，`tools/` 就是工具层，只是不再有"注册"这回事，工具是一张静态表。
- core 为**单一库**，模块以目录 + 命名空间划分，不拆独立库。
- ~~扩展机制：C ABI 动态库（ADR-0001）~~ —— **已废除**（[[ADR-0016]]，2026-08-25）。5 个容器全部并入 core：协议与计价进 `llm/`，工具进 `tools/`，权限成为一个配置键。
- 语言标准：C++26，异步基于协程（ADR-0003）。
- 会话持久化：JSONL 文件 + 会话树。
- 网络传输：core 内置 libcurl（同步 + SSE 流式）。
- 日志：spdlog（第三方依赖）。
- core 第三方依赖：libcurl + spdlog 两个（FTXUI 属 TUI 层）。
  - 实况注（2026-08-28）：需要 find_package 的是**三个**——libcurl、spdlog、quiche（`core/CMakeLists.txt`）。JSON 是 nlohmann/json 3.12.0，单头文件 vendored 在 `core/include/json.hpp`，不必安装、不必链库。括号里的 FTXUI 是 ADR-0007 之前"TUI 也用 C++"方案的残留，本项目**没有也不会**依赖它（TUI 是 Go + Bubble Tea）。
- TUI：Go + Bubble Tea（ADR-0007）。参考 claude code / codex 客户端外观，**有状态栏**（见 [[Statusline（状态栏）]]）。原定"无状态栏"，后反转并已完整实现（core 侧 `GET /statusline` + `statusline` 帧，TUI 侧 `tui/cmd/realagent-tui/statusline.go`）；**反转无 ADR 记录，且与现存 ADR 冲突**——`docs/adr/0007-tui-go-bubbletea.md:34` 至今写着"无状态栏（用户明确）"，无 ADR 取代它。理由未知。
- TUI 渲染：历史归终端管，不进 altscreen（ADR-0008）——定型的行打进终端原生 scrollback，Bubble Tea 只重绘底部活动区。
- 配置约定：项目级 `.realagent/` + AGENTS.md；全局 `~/.realagent/`。
  - 实况注（2026-08-25）：**项目级那一半没落地**。`settings.json` 只读 `~/.realagent/settings.json`（`core/src/config.cpp` 明写"唯一的覆盖来源，不看 cwd"）。`extensions/` 目录随 [[ADR-0016]] 一并作废。唯一真按项目分家的是会话目录 `.realagent/sessions`（相对 cwd）。
- 上下文压缩：第一版不做。靠最大上下文模型硬撑，会话满则开新会话；后期可加 auto-compact。
- Steering：第一版只支持中止（abort），不支持中途插话。插话（steering queue）后期基于异步引擎再加。
- 会话管理：第一版支持新建/恢复/列表，无 fork/树导航。
- 多 Agent：第一版单 agent，事件流引擎按可并发设计预留多 agent 编排接口（第二版里程碑）。
- 内置工具（第一版）：**read / edit / bash**，core 里一张静态表（`core/src/tools/tools.cpp`）。write 不单独存在——write 是 edit 的 `+x-0` 特例（空文件追加 = 创建），LLM 创建文件用 edit。
- Provider（第一版）：只做 `/v1/messages` 协议，目标模型 DeepSeek。换供应商 = 改 `base_url`，不装东西。
- 权限（第一版）：配置键 `permission`——`ask`（默认）/ `allow-all` / `deny`，一个 switch（[[ADR-0016]]）。`allow-all` 只为打通链路，非真实安全。
- 审批链路：core 永远是发起方。裁决为 ask 时 core 向用户交互界面（TUI/gui）发询问，界面回传裁决。gui 与 TUI 是平等的 HTTP 客户端，接口按多客户端设计。
- 模型元数据（[[Model]]）：name / owned_by / context 三个字段，**不含单价**。
  - 实况注（2026-08-25）：数据在 `Pricing`（`core/src/llm/llm.cpp`）里，启动时读一次。绕了一圈回到"core 持有"——但持有的是**数据表本身**，不是别人数据的副本，那才是当初 ADR-0012 反对的东西。三个字段与"不含单价"两条自始至终没变。
- 计价：core 按本次模型查[[模型数据表]]算出 [[Cost]]，按 agent 实例跨 turn 累加，经 [[status_update]] 帧下发（本次 run 累计绝对值，客户端覆盖写）。token 不跨**客户端**边界。
- 模型数据表：不走 settings.json；`~/.realagent/models.json` 存在即整表接管编译进二进制的出厂表，不合并。
- 不兜底原则：模型表解析失败即报错原文（不跳过坏条目、不补默认值），配置的模型名不在表里不检查不警告。少写代码优先于替用户擦屁股。
  - 实况注（2026-08-25）："解析失败即硬错"有一处松动——报错但**不拒绝启动**。从前表是插件的，读不动就是那个插件加载失败；现在表是 core 的，为一份可选的价目表拒绝对话，是把次要功能提成了必需品。表读不动 = 本次运行不计价，仅此而已。
- 配置产物与打包产物分离：用户可改的一律在运行时目录（`~/.realagent/settings.json`、`~/.realagent/models.json`）；出厂数据编译进二进制，重装即覆盖，不劝用户改。
- 配置机制（ADR-0010）：分层两级——**代码里的默认树打底，`settings.json` 覆盖**。默认树（`config_defaults()`）是键清单的唯一来源，每个键带注释说明作用与取值形状；配置文件不再是唯一来源，只是覆盖层。**无 env 覆盖、无必需键**：配置缺失不是错误状态，只是取到默认值，core 不校验缺了什么。配置文件存在但解析失败仍是硬错（启动即退出）。**默认值写真实可用的值**（[[ADR-0016]]）：从前一律留空串并明写"别拿假 URL 占位"——因为兜底链路靠 `empty()` 判断"用户没配过"，任何非空值都会让 Provider 壳的兜底当场失效。壳没了，那条暗协议随之作废，默认端点与默认模型现在就写在默认树里，装完即可用。
- 配置写回：`persist` **点对点**——读文件、只改目标键、原子写回，不 dump 内存树。默认值因此永不渗进用户的 `settings.json`，文件里只有用户自己配过的东西。文件不存在按空对象处理；坏 JSON 则不写并返回失败（宁可 `/model` 不生效，也不能拿内存树覆盖掉读不懂的用户数据）。
- 配置合并粒度：**对象递归合并，数组与标量整个替换**。数组不合并是刻意的——用户写一个数组的意思是"就是这些"，不是"在默认基础上再加"。
  - 实况注（2026-08-25）：删掉 `plugins.*` 之后默认树是平的，递归分支暂时没有使用者。留着是因为它是"合并"的一般情形，删掉不是消除特殊情况、是删掉通例。
- 不做配置热重载（ADR-0010）：启动时读一次，之后 core 不再看 `settings.json`。手改配置需重启 core。取消的理由是运行时重读引出的问题没有干净解：core 是常驻服务、TUI 是独立进程，stderr 报错没人看得见；报错退出会断掉所有客户端且有误杀风险（部分编辑器与 shell 重定向"先清空再写"，事件循环有概率读到半截文件）。没有运行时重读，就没有运行时坏文件。
- LLM 可配项：api_key / base_url / model / small_model + permission，**全部可缺省**（缺省即默认树里那个真实可用的值）。只配 `api_key` 就能跑。base_url 与 api_key 同级——代理/网关用户（OpenRouter / one-api / 内网中转）必须能自定义端点。
- 模型档位（[[Model Tier]]）：`model` 主模型 + `small_model` 小模型两档，共用 base_url / api_key。档位只换模型名，不换端点凭证——跨供应商小模型不做（真需要时再让 dialog 携带端点 override）。`Config::model(ModelTier)` 是唯一知道键名的地方，协议层无感。**不做档位间回落**：两档各有各的默认值，回落会让"我配了小模型"与"我没配所以用了主模型"长得一模一样，出账单时才发现区别。
- 会话目录不是配置项：`.realagent/sessions` 是 core 自己的落盘路径（`Config::session_dir()`，相对 cwd），写死在 core 里，settings.json 写它不生效。
- 斜杠命令：全部 core 内置——`/new` `/resume` `/model`（`handle_command`，见 `core/src/main.cpp`）。`/model` 无参列[[模型数据表]]的清单，带名切主模型并写回 settings.json；只认表里的模型——交互式选择就该从已知的里挑。
  - 实况注（2026-08-25）：`/plugins` 与 `/provider` 随 [[ADR-0016]] 删除。`/quit` 与 `/statusline` **不归 core**——退出的是客户端进程，展示偏好是客户端的事，两者都是 TUI 本地命令。插件可注册命令这条从设计到废除，实际提供过命令的插件数是 0。
- 通信协议：见 `docs/PROTOCOL.md`（可靠流请求-响应 + 推送流，全可靠 + 0-RTT）。
- 架构：core 为常驻服务，客户端（TUI/未来 gui）通过 **QUIC/HTTP3** 连接（ADR-0006）。REST 语义（POST /message、POST /approval-response 等）；推送流为 HTTP/3 长生命周期单向流（SSE 语义），**全可靠**（增量与事件无差别，QUIC 可靠流原生保证，无自建确认机制）。0-RTT 快握手。支持公网部署。
- QUIC 库：core 用 **Cloudflare quiche**（QUIC + HTTP/3 一体，`core/CMakeLists.txt:17-19`）；TUI 用 quic-go。msquic 于 2026-08-09 被弃（纯传输层无 H3 语义），ADR-0006 当时记的替代品是 ngtcp2 + nghttp3，但落地的是 quiche——**此次换库无 ADR 记录，理由未知**（ADR-0006 与本条原文均未更新，ADR-0002 已在用"quiche 非线程安全"作论据）。
- 出站 Provider 请求：core 用 libcurl（HTTP/1.1 客户端，请求 DeepSeek）；入站客户端通信走 QUIC。
- 工具结果：一个 json，形状 `{"status": <int, 0=成功>, "output": <string, 给模型看的文本>}`；`Executor::execute` 再加一个 `"interrupted"` 键（core 本次执行期间提没提过中止）。没有 `ToolResult`/`ExecResult` 结构体——工具本来就在拼 json，两个字段的信封是多余的。
- JSON 实现：nlohmann/json 3.12.0，单头文件逐字节 vendored 在 `core/include/json.hpp`，类型就是 `nlohmann::json`——**core 不包壳**。链式 `a["b"]["c"]` 与隐式转换是库自带的；读不受控的输入用 `find()` / `value(key, 默认值)`（const `operator[]` 撞上缺键是未定义行为），解析用 `parse(text, nullptr, false)` + `is_discarded()`。
- DeepSeek 接入：端点 `https://api.deepseek.com/anthropic`，模型 `deepseek-v4-flash`（或 `deepseek-v4-pro`），API key 见 platform.deepseek.com。工具调用与流式完整支持；`cache_control` 被忽略（验证首版无需 vendor 层）。
- 参考资料：`OPENCODE_RESEARCH.md`（OpenCode 架构调研）。

## 目录结构

```
realagent/                  # 主仓库（core + tui + docs）
├── core/                   # C++ QUIC/HTTP3 服务（ADR-0006）
│   ├── include/            #   公共头：config.hpp / context.hpp / json.hpp + agent/ llm/ tools/ server/
│   ├── src/
│   │   ├── llm/            #   一次 LLM 调用：造请求 + SSE 解析 + 计价（llm.cpp）
│   │   ├── tools/          #   内置工具静态表：read / edit / bash（tools.cpp）
│   │   ├── agent/          #   agent loop、事件流、状态、工具执行、审批
│   │   ├── server/         #   QUIC/HTTP3 服务（quiche）、推送流、审批端点
│   │   ├── config.cpp      #   配置：默认树 + settings.json 覆盖 + 点对点写回
│   │   └── main.cpp        #   启动、事件循环、端点回调、斜杠命令
│   ├── tests/              #   test_config / test_llm / test_session
│   └── CMakeLists.txt
├── tui/                    # Go + Bubble Tea 客户端（ADR-0007）
│   ├── cmd/realagent-tui/
│   └── internal/
├── docs/                   # ADR 等文档 + capabilities.md（core 内置能力清单）
├── CMakeLists.txt          # 顶层：core 构建 + go build（TUI）
├── CONTEXT.md
└── OPENCODE_RESEARCH.md    # （未重建）
```

**2026-08-25（[[ADR-0016]]）之后不再存在的**：`core/src/extension/`（宿主词汇与管线槽位）、
`core/sdk/`（`agent_caps.h` 能力键与签名）、`cmake/`（SDK 的 find_package 导出）、
`realugin/`（插件体系，独立仓库）、`realagent-plugins/`（5 个容器，独立仓库）、
`~/.realagent/extensions/`（装好的动态库）。

`core/src/` 下 `permission/` 目录**不存在，也不再规划**：权限是一个配置键 + `executor.cpp`
里一个 switch，审批协调器在 `agent/approval.cpp`（ASK 状态机 + `permission_request` 帧）。
为一个 switch 单开一个目录，是把"这件事很重要"和"这件事很复杂"搞混了。
