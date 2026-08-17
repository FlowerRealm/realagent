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

LLM 可调用的具名函数。带名称、描述、参数 Schema（JSON Schema，插件侧用宏 + JSON 字符串字面量定义），执行后返回结构化结果。错误通过抛异常/返回错误信号传达，而非塞进正常结果里。

_Avoid_: `function`、`command`（Tool ≠ 用户斜杠命令）

### **Event（事件）**:

Loop 向订阅者（TUI、扩展、日志）发布的生命周期消息。典型序列：
`agent_start → turn_start → message_start → message_update* → message_end → tool_execution_start → tool_execution_end → turn_end → agent_end`

事件是**异步事件流**的一部分（ADR-0002）：生产方与消费方解耦，扩展通过订阅事件获得能力。异步引擎的动机是**多 agent 并发编排**，而非单 agent 工具并行。

_Avoid_: `callback`（事件是广播的，回调是一对一的）

### **Multi-Agent（多代理编排）**:

异步事件流的核心动机：多个 agent 实例并发调度（如主 agent 派生子任务）。每个 agent 内部仍保持工具严格顺序执行。

_Avoid_: `sub-agent`（特指派生出来的子 agent，Multi-Agent 是泛指）

### **Background（后台运行）**:

长时工具（bash 等）的规划机制：工具可在后台运行，agent 不阻塞等待，用户可查看进度。未来并入，不属于第一阶段核心。

_Avoid_: `async tool`（工具本身是同步顺序执行的，后台只是运行策略）

### **插件（Plugin）**:

一组[[能力]]的**容器**：一个 C ABI 动态库（`.so`/`.dylib`），运行时由 core 目录扫描 + `dlopen` 加载。见 [[ADR-0001]]。

容器**本身不是能力**，它只承担三件事：**命名空间**（其能力与具名条目一律以它命名，跨容器不撞名）、**生命周期单元**（加载/卸载/禁用的粒度，装卸的是整个容器）、**诊断单元**（`GET /plugins` 报的是容器清单及其内含能力）。

core 的**调用路径一律按能力寻址，不按容器寻址**：谁提供某个能力是加载期算出来的事，调用点只面对能力本身。容器名只出现在装卸、命名与诊断里。

_Avoid_: `Extension`（除 core 的 `extension/` 目录名外不再使用——代码、`plugin.json`、`PluginManager` 全线是 plugin，术语跟着代码走）、`插件即能力`、`调用插件`（调用的是能力）

### **依赖（Dependency）**:

容器与容器之间的边：`plugin.json` 的 `deps` 声明本容器需要哪些**容器**先就位。

**依赖的单位是容器，不是[[能力]]**——如同 mod 依赖另一个 mod，而不是依赖那个 mod 里的某件物品。按能力写依赖会让同一个容器以它各个能力的名义在 `deps` 里反复出现，边被打散成线团。

全部容器构成一张 **DAG**（不是森林——一个容器可以有多个 `deps`，多父不成立森林），`deps` 是边。

按 `deps` 建**反图**（被依赖者 → 依赖者），从入度为 0 的点起，**BFS 逐层**推进即加载顺序：入度计数保证一个容器**等齐全部** deps 才轮到它。跑完仍有剩余节点，那些点构成**环**——非法，点名。同一张反图的出边就是"谁依赖我"，卸载安全检查直接读它。

容器就位之后，"它到底提供哪些[[能力]]"是随后**查询**的事——依赖保证存在，查询回答内容。

_Avoid_: `森林`（多父）、`DFS 序`（可能在另一条 dep 未就绪时就初始化）、`按能力声明依赖`

### **Provider（模型提供商）**:

LLM 后端的统一抽象。**适配本身走插件机制**（ADR-0004）：core 收集对话信息 → 生成请求 → 改请求 → core 发出（见[[管线]]）。core 不内置任何具体协议知识，也不认任何供应商身份。

_Avoid_: `backend`、`LLM client`（指 core 内置模块时）

### **Model Tier（模型档位）**:

一次 LLM 调用用哪一档模型：**主模型**（`model`，对话主链路）或**小模型**（`small_model`，标题/摘要一类杂活）。档位只是模型名的选择，两档共用同一 base_url / api_key ——不是独立的供应商配置。core 侧 `Config::model(ModelTier)` 解析档位，结果照旧经 `dialog["model"]` 透传给协议链，[[Provider 壳]]与协议层对档位无感。两档均为必需配置——不配就起不来，没有回落。

_Avoid_: `fast model`、`模型 profile`（档位不带端点凭证，不构成完整 profile）

### **v1-messages（协议层）**:

`/v1/messages` 是一种协议，不是某家公司的私有物——Anthropic、DeepSeek、OpenRouter 等多家公司都实现同一端点。协议层插件只懂协议固有内容（请求结构 + SSE 解析 + thinking 块），不识别任何供应商、不设端点/模型默认值。

_Avoid_: `anthropic api`（Anthropic 是一家公司，协议本身供应商中立）

### **Provider 壳（供应商适配）**:

承载**供应商身份**的容器，在[[管线]]上提供两段：**改请求**（把粗请求补成能真发出去的请求——端点、默认模型名、凭证）与**计价**（把 token 用量换算成钱）。另提供模型清单。

它**不包裹任何插件、不调用任何插件**：收一个请求还一个请求，收一个事件还一个事件。装了多个供应商容器（DeepSeek、OpenRouter…）时，只有[[当前 provider]]指名的那个参与管线，其余照常加载但不上线——两家都往请求里塞自己的端点与凭证，串起来的结果只取决于加载顺序，那是被明令禁止的静默仲裁。

计价属于供应商身份：按本次模型查自己的[[模型数据表]]乘出钱数，把 token 用量事件换成 [[Cost]]，**token 到此为止不再上传**。

_Avoid_: `壳`、`套壳`、`vendor shim`、`Provider Adapter`（不再有包裹关系，见[[管线]]）

### **Protocol Plugin（协议层插件）**:

懂某个协议本身的容器，在[[管线]]上提供两段：**生成请求**（对话 → 该协议的粗请求）与**解析响应**（响应流 → 事件）。现阶段 `/v1/messages`（多家公司共用，详见 [[v1-messages]]）。

两段是**两个各自独立的[[能力]]**，不是一对：各占各的槽、各自被选中，只是通常由同一个容器提供。协议层供应商中立——粗请求里没有端点、没有模型名、没有凭证，那些由[[Provider 壳]]在下一段补。

_Avoid_: `成对`（是两个独立能力）、`provider plugin`（协议层 ≠ 供应商层）

### **管线（Pipeline）**:

一次 LLM 调用被 core 拆成前后相接的几步，每步调一个[[能力]]，上一步的产物就是下一步的入参：

```
对话 ──生成请求──▶ 粗请求 ──改请求──▶ 精请求 ──[core 发出]──▶ 响应流
                                                          │
       事件 ◀──计价──  事件  ◀──解析响应───────────────────┘
```

**插件互不认识，也互不调用**：改请求的插件收到一个请求、还回一个请求，它不知道这个请求是谁生成的；生成请求的插件也不知道有人会改它。流程归 core，每一段归一个插件。

四段的归属**由用户选定的 provider 一次确定，零推导**：配置指名的容器提供「改请求」与「计价」，它 `plugin.json` 里的 `protocol` 字段指名的容器提供「生成请求」与「解析响应」。不会出现"生成来自 A、解析来自 B"的拼盘。

管线的段数是**明写在 core 流程里**的。新增一段（例如上下文压缩）要改 core：加一个能力名、加一次调用——但不动插件接口、不破 ABI、已有插件不重编。**不引入通用钩子/中间件机制**：那会让"经过哪几段、什么顺序"变成运行时才知道的事，而顺序恰恰是本项目一路在消灭的隐式仲裁。

这取代了原先的**插件嵌套**（外层持有内层引用、逐层包裹、`claim` 接管）：装饰关系、接管、被接管者退出候选集、接管方与被接管者的连带卸载，全部不再存在。

_Avoid_: `嵌套`、`套壳`、`装饰器`、`外层/内层`、`claim`、`接管`（均为已废除的旧模型）

### **架构原则：钩子与策略分离（core 哲学）**:

core 只提供**检查点/钩子**（工具执行前、请求构造时），所有**策略外置为插件**：Provider 适配是插件（ADR-0004）、权限审批是插件（ADR-0005）。core 是编排者，不是决策者。新需求的判断标准：这是 core 的编排职责，还是可替换的策略？策略一律进插件。

_Avoid_: `core 内置策略`（除非 ADR 明确拍板）

### **能力（Capability）**:

一个名字 + 一个函数。不是一份数据，不是一次注册，是[[插件]]提供、需要时被调用的一件**功能**。

**零例外**：生成请求、改请求、解析响应、计价、工具执行、权限裁决——每个都是一个函数。曾被当作"一组"的请求构造 + 响应解析，实为[[管线]]上两段独立能力，被 core 的网络往返隔开而已，不构成一个能力。

**数据留在插件里**：模型清单、工具清单、单价表——这些都是插件自己的数据，core 不收藏、不建表、不同步。要用的时候调 `model.list` / `tool.execute` 问它一次，用完就扔。core 存一份副本，就得同步、就得失效、就得在插件卸载时注销，而这些机器全部只为维护那份副本而存在。

判据一句话：**core 里出现"插件数据的副本"，就是设计跑偏了**；唯一允许的副本是可随时丢弃的缓存，且必须有明写的失效点。

**能力的地址是 (容器名, 能力名)**。能力在容器内按名建键，因此一个容器对同一能力最多提供一份——地址天然唯一。[[插件]]是容器，不是能力：同一个容器里的两个能力互不相干，接管、进槽、被调用都各算各的。

_Avoid_: `注册`（push 语义，暗示数据搬进 core）、`插件数据入库`、`同步到 core`

### **借阅（Borrow）**:

core 向插件取数据时不接管所有权：拿到的是指向插件自有内存的 `const` 指针，**有效期等于插件在位的时长**（加载到进程退出或被禁用），core 读完即用，不释放、不长期持有。

与"交出所有权"划清：core 需要留着的东西（如构造好的请求）才由插件经 core 的分配器申请、core 释放。判据是**core 要不要留着它**——不留就借，要留才转移所有权。借阅两侧都不释放，因此不存在跨边界的堆所有权问题；它唯一的风险是插件卸载后仍被引用，而那与[[能力槽]]要遵守的是同一条不变量：**插件一卸，从它那儿借来的一切当场作废**。

_Avoid_: `拷贝一份`（清单类不拷）、`内存归插件但 core 释放`

### **能力槽（Capability Slot）**:

[[管线]]上的一段，core 侧一个指针，恰好指向一个提供者。插件**填了哪个能力就提供哪个能力**——core 不问"你是什么类型"，只看"你能干什么"。

槽里装的是**函数指针 + 实例**，不是插件：调用点只知道"有个东西能干这事"，不知道它来自哪个 `.dylib`、叫什么名字。

槽位**独占**：一段管线只能有一个执行者。同一能力有多个提供者时，由配置指名（如[[当前 provider]]决定谁来改请求与计价）；无从指名又不止一个候选，则该槽空置并点名双方——不做后写覆盖、不做先到先得、不猜。

_Avoid_: `插件类型`、`type`（能力由插件决定，不由声明约束——枚举分类已废除）、`成对的槽`

### **当前 provider**:

由配置 `provider` **显式指定**的那个[[Provider 壳]]：它提供的**改请求**与**计价**两段上线，其余供应商容器照常加载但不参与[[管线]]。`/provider` 切换即换槽位指针，无需重载插件。

选中它**同时选定了协议**：它 `plugin.json` 的 `protocol` 字段指名哪个协议容器，「生成请求」与「解析响应」就归谁。用户选的是 provider 这一个名字，一次定下整条[[管线]]。

未配置时：恰好一个候选就用它，多个候选是硬错误并点名——**不猜**。

_Avoid_: `自动选入口`（依赖图推导出来的归属是静默冲突，已废除）

### **命名空间（Namespace）**:

插件注册的具名条目（工具、命令）一律带插件名前缀，跨插件因此不可能撞名。工具用下划线（`core-tools_read`）——LLM 工具名受 API 约束 `^[a-zA-Z0-9_-]{1,64}$`，斜杠与点号非法；命令用冒号（`<容器名>:<命令名>`）。

**短名是 core 的名单，不是自动去重**：配置 `plugins.unprefixed` 列出的插件其条目不加前缀（首版含 `core-tools`，故 LLM 见到的是 `read` / `edit` / `bash`）。自动去重的结果会随装了哪些插件而漂移，名单不会。

每个条目**恰好一个对外名字**——上名单即短名，不上即前缀名，不注册别名。最终名字撞车（无论出自哪条路径）即加载失败点名。

_Avoid_: `别名`、`fallback 短名`

### **插件元数据（Plugin Metadata）**:

插件携带的声明信息：名称、描述、版本、接口版本、前置容器依赖。独立 JSON 文件（`plugin.json`）。**不含 type**——能力由实现决定（见 [[能力槽]]）。插件名须匹配 `^[a-zA-Z0-9-]+$`：允许连字符，**禁止下划线**，否则[[命名空间]]前缀无法无歧义解析。

[[Provider 壳]]额外声明 `protocol`：它的「改请求」是照着哪个协议的请求形状写的。这件事**推不出来**（往 `body.model` 填模型名、往 headers 塞凭证，位置都是协议特有的），只能由插件作者声明；也不开放给用户配——用户选的是 provider，不是"把某家的精修接到别的协议上"。该字段自带一条[[依赖]]边，`deps` 不必重复列。

_Avoid_: `manifest`

### **Module（模块）**:

core 内部的职责分区（ai / agent / tools / extension），以目录 + 命名空间划分，**不**编译为独立库。C++ 多库拆分成本高，单库分模块是刻意选择。

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

assistant 消息中的一种 content block 类型（`{"type":"thinking","thinking":...,"signature":...}`），承载模型的推理过程（DeepSeek v4 reasoning）。抽象对话里原样保留，协议插件负责与 Anthropic `thinking` 块互转；core 流式转发 `thinking_update` 事件，TUI 渲染为 dim 斜体块。signature 用于回传历史时校验，缺失时省略。

_Avoid_: `reasoning_content`（DeepSeek 原生 API 字段名，Anthropic 兼容端点映射为 thinking 块）

### **Model（模型元数据）**:

一个模型"是什么"：名称、归属供应商、上下文窗口。**不含计价**——单价是 [[Provider 壳]] 的私事，core 只收 [[Cost]] 的最终数字。

与 [[Model Tier]] 划清：Tier 是"这次调用用哪一档"，Model 是"那一档指向的模型是什么货色"。

数据归**插件**所有，不是 core 的配置：[[Provider 壳]]自己读自己的模型数据表（[[模型数据表]]），core 要用时向[[当前 provider]]要一份清单，**用完即弃、不建注册表**（[[能力]]：core 不留插件数据的副本）。**单价不报**——core 不算钱，存了没有消费方。

不同[[Provider 壳]]报同一个模型名是**预期行为**——同一模型有多条到达路径（DeepSeek 直连 / OpenRouter），上下文窗口相同但端点、凭证、单价各异。因为清单是现问现答的，"是哪条路径"由问的时候是谁在位决定，不存在把多条路径压进一张表的问题。

core 不内置任何模型数据，也不校验用户配的模型名在不在表里——**表是参考资料，不是白名单**：配了表外的模型照发不误，不检查、不警告、不兜底。

_Avoid_: `模型清单`（指单个模型时）、`provider model`

### **模型数据表（models.json）**:

[[Provider 壳]]自有的模型数据文件，含单价在内的全部字段。两个位置，**不合并**：包内 `models.json` 是出厂数据（打包产物，只读，随插件升级更新）；运行时目录 `~/.realagent/models/<插件名>.json` 是用户接管版，**存在即整体接管**，出厂数据一概不看。

解析严格：缺字段、格式错即插件加载失败并报错原文，不跳过坏条目、不补默认值。

_Avoid_: `模型配置`（它不走 [[配置]] 那套 core 注入机制，是插件自己的数据）

### **Cost（花费）**:

一次 run 花掉的钱（USD）。由 [[Provider 壳]] 按本次调用的模型查自己的[[模型数据表]]算出——**单价与 token 用量都不出插件**，core 只收最终数字。core 按 agent 实例跨 turn 累加，随 [[status_update]] 帧下发；一次用户输入起算清零，帧内为累计绝对值。

_Avoid_: `usage`、`token 统计`（core 侧已无 token 概念，只有钱）

### **status_update（运行态帧）**:

插件向客户端报运行态数据的推送帧，**开放键集**：插件报什么键就是什么键，core 除 `cost`（需累加）外一律不认识、原样转发。落点是 [[Status（状态行）]]——本次 run 的实时数字，run 结束即消失。

_Avoid_: `cost 帧`（单值帧型是死路，加第二个值就得破坏客户端）

### **Statusline（状态栏）**:

输入框**下方**的常驻栏：`🤖 model | 📁 dir | 🌿 git`。内容是**这次会话的身份信息**——哪个模型、哪个目录、哪个分支。目录与分支会话期内不变，启动拿一次；模型会被 `/model` 切档改，改了由 core 推 [[statusline]] 帧覆盖写——客户端不轮询，也不关心是谁改的。推帧的触发是**主循环比对载荷**：事件循环每轮算一次 statusline 载荷，与上次推送的不同才推。因此改配置的代码路径不需要记得通知谁，与状态栏无关的配置变更（如 `plugins.disabled`）也不会白推一帧。展示偏好（显示哪几段、emoji 还是 nerd font）纯客户端状态，core 不认。

_Avoid_: `状态行`（那是活动区里的另一条，见下）

### **Status（状态行）**:

[[活动区（Live Region）]]里的读秒行：`⠋ 思考中… (1m23s · $0.0123 · esc 中断)`。内容是**本次 run 的实时数字**（读秒、[[Cost]]），数据经 [[status_update]] 帧推送。run 结束整行消失——它描述"正在发生的事"，事结束了就没有它。

_Avoid_: `statusline`（那是输入框下方那条常驻栏）

### **活动区（Live Region）**:

TUI 底部由 Bubble Tea 每帧重绘的区域：未定型行 + 审批框 + [[子面板（Panel）]] + 斜杠菜单 + 读秒状态行 + 输入框。其上方是**终端原生 scrollback**——已定型的行由 TUI 打进去后不再归 TUI 管（[[ADR-0008]]）。活动区绝不能高过终端。

_Avoid_: `视口`、`viewport`（本项目不自建滚动缓冲）

### **子面板（Panel）**:

斜杠命令的第二层选择（参考 codex cli）：`/model` `/plugins` `/statusline` 无参执行后，用同一份结果载荷开一列可选项——↑/↓ 选择、Enter 确认、Esc 取消，模态（开着时按键只喂它）。**确认 = 把该项的整条命令写进输入框走正常提交**，没有第二套提交路径；面板也不新增端点，数据就是命令回包里的 `data`。造不出面板（无数据/坏载荷）就退回文本清单。

### **定型（Freeze）**:

一行渲染内容不再变化、可以打进 scrollback 的状态。判据只有一条：**追加式文本的贪心折行前缀稳定**，故除最后一个折行外全部定型（[[ADR-0008]]）。

_Avoid_: `finalize`（原指把 streaming 消息收进列表，行模型下已无此概念）

---

## Relationships

- 一个 **Agent Loop** 由一个或多个 **Turn** 推进；一个 **Turn** = 一次 LLM 调用 + 该调用产生的全部 **Tool** 执行
- 一个 **能力槽**恰好由一个插件占用；一个插件可占 0 到 N 个槽（能力来自容器 `capabilities()` 交出的 `{名字, 函数}` 表，按名字查得到即提供）
- **Provider 壳**与 **Protocol Plugin** 互不认识：各自提供[[管线]]上不同的一段，由 core 依次调用（生成请求 → 改请求 → 发出 → 解析响应 → 计价）
- **当前 provider** 由配置 `provider` 从已加载的供应商容器中选出，决定"改请求"与"计价"两段归谁
- 一个 **Provider 壳**拥有一份**模型数据表**，据此可报 0 到 N 个 **Model**；core 用时现问，不建注册表
- 一个 **Model Tier** 解析为一个模型名；模型名原样下传，**Provider 壳**不感知档位
- **Cost** 由 **Provider 壳**算出，经 **status_update** 帧下发，落点是 **Status（状态行）**——不是 **Statusline（状态栏）**
- 一个插件注册的每个具名条目（工具/命令）恰好有一个对外名字，由**命名空间**规则决定

## Example dialogue

> **A：** 加一个 OpenRouter 壳，它也报 `deepseek-v4-flash`，跟 deepseek 壳撞了吗？
> **B：** 不撞。模型清单是**现问现答**的——core 只向[[当前 provider]]要，问的时候谁在位，答案就是谁的。core 不存表，也就没有"两条路径压进一个键"这回事。
> **A：** 两个壳都提供"改请求"，**能力槽**不是独占吗？
> **B：** 是独占。"提供某能力"和"占着槽"是两回事：两个壳都加载、都初始化，只有 `provider` 指名的那个上线。这也是 `/provider` 能不重启切换的原因——另一个早就 init 好了，换个指针而已。
> **A：** 那 `v1-messages` 呢？它跟壳是什么关系？
> **B：** 没有关系。它提供"生成请求"和"解析响应"，壳提供"改请求"和"计价"，四段各占各的槽，由 core 依次调用（见[[管线]]）。谁也不包谁，谁也不调谁。
> **A：** 如果我一个壳都不装，只装 `v1-messages`？
> **B：** 那"生成请求"和"解析响应"两段照常有人干，"改请求"和"计价"两段空着——core 直接把粗请求发出去。端点和凭证得你自己在配置里写全，否则请求发不出去。

## Flagged ambiguities

- **"插件类型"**曾同时指「`plugin.json` 声明的 type」与「插件实际能干什么」——已解决：删除 `type`，只保留后者，落在**能力槽**上（ADR-0011）。
- **"模型重名"**曾被当作冲突（后写覆盖）——已解决：它是**预期行为**，冲突与"同一事物的多条路径"是两回事。**注**：该结论最初的理由是"注册表改按 (插件, 模型名) 建键"，而那个复合键方案已随 ADR-0012 连同注册表本身一起作废。结论不变但理由换了——现在是**根本没有表**：清单向[[当前 provider]]现问现答，问的时候谁在位答案就是谁的，不存在"两条路径压进一个键"的场合（见 [[Model]]）。
- **"Provider 壳" / "Provider Adapter"** 曾作为两个条目重复定义——已解决：统一用「Provider 壳」，Provider Adapter 列为待避免的别名。
- **"statusline" / "状态行"**指两个不同的东西（输入框下方常驻栏 vs 活动区读秒行）——已解决：**Statusline** 与 **Status** 分列，各自的 _Avoid_ 互指。
- **"入口"**曾指「core 从依赖图推导出的协议链最外层」——已解决：改为**当前 provider**，由配置显式指定，推导逻辑删除。
- **"能力" vs "注册"**：已解决（ADR-0012）——[[能力]]是"一个名字 + 一组函数"，工具/命令/模型三张注册表全部取消，一律现调现取。曾一度把能力拆到函数级（`protocol.build_request`）与按能力声明[[依赖]]，均已否决：查函数级与查能力级本质相同，而按能力声明依赖会让同一容器在 `deps` 里反复出现。
- **"套壳 / 嵌套"**曾是协议链的组织方式（壳持有内层引用、`claim` 接管）——已废除（ADR-0012）：改为[[管线]]，插件互不认识，core 依次调用每一段。随之作废的还有"构造 + 解析成对"——那是两段独立能力，中间隔着 core 的网络往返而已。
- **"插件"曾同时指容器与能力**——已解决：[[插件]]是容器（命名空间 / 生命周期单元 / 诊断单元），core 的调用路径只按能力寻址。已解决：术语向代码低头——统一用**插件**，`Extension` 只保留为 core 的目录名。

---

## 已拍板（暂存，随决策更新）

- 项目定位：AI 编码 agent，第一阶段 core + tui（均 C++），gui 后续。
  - 实况注（2026-08-16）：**TUI 不是 C++，是 Go + Bubble Tea**（ADR-0007，见本节 TUI 条）。"均 C++"是 ADR-0007 之前的设想，未随之更新。core 是 C++。
- 架构基调：极简核心 + 插件/扩展架构，参考 Pi（earendil-works/pi）。
- core 分层参考：ai（Provider 抽象）→ agent（Loop/状态/事件）→ tools（注册与执行）→ extension（扩展宿主）。
  - 实况注（2026-08-16）：四层里**只有 agent 与 extension 有实体**。`ai/` 与 `tools/` 在 `core/src/` 下**不存在**——Provider 抽象已整体外移到插件（[[管线]]四段），工具注册表随 ADR-0012 删除（工具清单现问现答），两层因此都没有落地的必要。实际 `core/src/` 为：`agent/` `extension/` `server/` + `config.cpp` `main.cpp`。
- core 为**单一库**，模块以目录 + 命名空间划分，不拆独立库。
- 扩展机制：C ABI 动态库（ADR-0001）。
- 语言标准：C++26，异步基于协程（ADR-0003）。
- 会话持久化：JSONL 文件 + 会话树。
- 网络传输：core 内置 libcurl（同步 + SSE 流式）。
- 日志：spdlog（第三方依赖）。
- core 第三方依赖：libcurl + spdlog 两个（FTXUI 属 TUI 层）。
  - 实况注（2026-08-16）：实际是**四个**——libcurl、spdlog、Boost.json、quiche（`core/CMakeLists.txt:12-19`）。括号里的 FTXUI 是 ADR-0007 之前"TUI 也用 C++"方案的残留，本项目**没有也不会**依赖它（TUI 是 Go + Bubble Tea）。
- TUI：Go + Bubble Tea（ADR-0007）。参考 claude code / codex 客户端外观，**有状态栏**（见 [[Statusline（状态栏）]]）。原定"无状态栏"，后反转并已完整实现（core 侧 `GET /statusline` + `statusline` 帧，TUI 侧 `tui/cmd/realagent-tui/statusline.go`）；**反转无 ADR 记录，且与现存 ADR 冲突**——`docs/adr/0007-tui-go-bubbletea.md:34` 至今写着"无状态栏（用户明确）"，无 ADR 取代它。理由未知。
- TUI 渲染：历史归终端管，不进 altscreen（ADR-0008）——定型的行打进终端原生 scrollback，Bubble Tea 只重绘底部活动区。
- 配置约定：项目级 `.realagent/`（settings.json + extensions/）+ AGENTS.md；全局 `~/.realagent/`。
- 上下文压缩：第一版不做。靠最大上下文模型硬撑，会话满则开新会话；后期可加 auto-compact。
- Steering：第一版只支持中止（abort），不支持中途插话。插话（steering queue）后期基于异步引擎再加。
- 会话管理：第一版支持新建/恢复/列表，无 fork/树导航。
- 多 Agent：第一版单 agent，事件流引擎按可并发设计预留多 agent 编排接口（第二版里程碑）。
- 内置工具（第一版）：**read / edit / bash 合并在一个工具插件中**。write 不单独存在——write 是 edit 的 `+x-0` 特例（空文件追加 = 创建），LLM 创建文件用 edit。
- Provider（第一版）：只做 Anthropic `/v1/messages` 协议，目标模型 Deepseek。
- 权限（第一版）：最小策略插件——永远允许，只为打通链路，非真实安全。
- 审批链路：core 永远是发起方。首版插件直接通过；未来插件 ask 时 core 向用户交互界面（TUI/gui）发询问，界面回传裁决。gui 与 TUI 是平等的 HTTP 客户端，接口按多客户端设计。
- 模型元数据（[[Model]]）：core 定义类型 + 持有注册表，数据全部来自插件（`list_models`，ABI 升 2）。core 的 Model 只有 name / owned_by / context 三个字段，**不含单价**。
  - 实况注（2026-08-16）：**"持有注册表"已作废**（ADR-0012 决策 4 删掉工具/命令/模型三张表）。core 不再定义 Model 类型、不建表，改为向[[当前 provider]]要一份 JSON 清单现问现答（能力键 `model.list`，`core/src/extension/slots.cpp` 的 `models_json()`）。三个字段与"不含单价"两条不变。
- 计价：全在 [[Provider 壳]]，core 只收 [[Cost]] 并按 agent 实例跨 turn 累加，经 [[status_update]] 帧下发（本次 run 累计绝对值，客户端覆盖写）。token 不跨 core 边界，`Usage` 结构体删除。
- 模型数据表：插件自有，不走 core 配置注入；运行时目录用户版存在即整体接管出厂版，不合并。
- 不兜底原则：模型表解析失败即硬错，配置的模型名不在表里不检查不警告。少写代码优先于替用户擦屁股。（原"模型重名后写覆盖不检测"作废：注册表改按 (插件, 模型名) 建键，重名不再发生。）
  - 实况注（2026-08-16）：括号里的"注册表按复合键建"已随 ADR-0012 一并作废（表本身没了）。**重名不再发生**这个结论仍然成立，但现在的理由是清单现问现答、压根没有表（见 Flagged ambiguities 的"模型重名"条）。前半句"不兜底"两条不变。
- 插件 type **已废除**（原 protocol / tool / permission / session 四类，ADR-0001）：能力由非空函数指针决定，core 侧落在[[能力槽]]上。`plugin.json` 与 `plugin_api_t` 均删除 `type` 字段，`GET /plugins` 改报 `capabilities`（由实现派生，不可能撒谎）。
  - 实况注（2026-08-16）："能力由**非空函数指针**决定"是 ADR-0011 的模型（能力 = `plugin_api_t` 上的非空字段），已被 ADR-0012 决策 3 取代：`plugin_api_t` 收敛为固定五项，容器经 `capabilities()` 交出一张 `{名字, 函数}` 表，core 按名字符串查（realugin `src/loader.cpp` 的 `find_capability`）。**新增能力 = 新增一个键**，不动结构体、不破 ABI。type 已废除与 `GET /plugins` 报 `capabilities` 两条不变。
- 冲突不静默：能力槽独占、具名条目经[[命名空间]]隔离、最终名字撞车即加载失败并点名。`register_*` 撞名返回 `PLUGIN_ERR`（原先一律返回 OK，插件不知道自己踩了谁）。
  - 实况注（2026-08-16）：`register_*` 这套 API **整个不存在了**（ADR-0012 取代 ADR-0011 决策 8，连同三张注册表、撞名记账、`unregister_entries` 一并删除），故"撞名返回 `PLUGIN_ERR`"已无所指。前半句三条不变；另外**能力槽冲突改为"空置并点名"，不再卸载插件**（ADR-0012 决策 11）。
- 跨边界内存**统一 core 分配**：core API 提供 `alloc`，凡交给 core 的内存一律经它分配、由 core 释放；`plugin_api_t` 的 `free` 删除。`list_models` 的"内存归插件、core 只读"特例一并取消（改为拷贝一份）。少一条规则胜过省一次拷贝——跨 dlopen 边界的堆错误两侧可能链着不同 allocator，定位成本远高于几 KB memcpy。
  - 实况注（2026-08-16）：**"统一 core 分配 + 清单改为拷贝一份"已被 ADR-0012 决策 5 取代**，现行是**两类规则**（见 [[借阅]]）：插件长期持有的（工具清单、模型清单、能力表等静态表）走**借阅**——`const` 指针、有效期 = 容器在位时长、**两侧都不释放、不拷贝**；本次调用现造的（请求 JSON、工具结果）走**转移**——经 `core->api->alloc` 分配、core 释放。判据是"这份数据本来就在插件手里，还是这次现造的"。代码与 SDK 均按此实现（realugin `include/realugin/plugin_api.h` 开头的两类规则说明，各能力签名处逐个标注"借阅"；realugin `src/loader.cpp` 的 `api_alloc`/`api_release` 注释写明"转移类一律 core 分配"）。**因此本条末尾的"改为拷贝一份"与 [[借阅]] 词条 _Avoid_ 的「`拷贝一份`（清单类不拷）」直接冲突，以词条为准。** 保留的只有那半句动机：跨 dlopen 边界的堆所有权要么归一侧、要么两侧都不碰。
- 当前 provider 由配置 `provider` 显式指定（见[[当前 provider]]），不由依赖链推导。
- 配置键落点判据：**`plugins.*` 决定装不装载，顶层决定跑起来用哪个**。故 `provider` 在顶层（与 `model` 同性质：在已加载/已注册的东西里选一个，`/provider` `/model` 都能不重启切换），`plugins.unprefixed` 在 `plugins.*`（决定 init 时注册出什么名字，装配期的事）。被 `plugins.disabled` 的插件根本不 dlopen；未被选中的 [[Provider 壳]]照常加载并初始化，只是不进槽。
- 插件元数据：独立 JSON 文件（`plugin.json`），含名称/描述/版本/ABI 版本/前置依赖。
- 配置产物与打包产物分离：用户可改的一律在运行时目录（`~/.realagent/settings.json`、`~/.realagent/models.json`），插件目录只放打包产物（`plugin.json` + 动态库 + 出厂 `models.json`），重装即覆盖，不劝用户改。
- 配置机制（ADR-0010）：分层两级——**代码里的默认树打底，`settings.json` 覆盖**。默认树（`config_defaults()`）是键清单的唯一来源，每个键带注释说明作用与取值形状；配置文件不再是唯一来源，只是覆盖层。**无 env 覆盖、无必需键**：配置缺失不是错误状态，只是取到默认值，core 不校验缺了什么。配置文件存在但解析失败仍是硬错（启动即退出）。core 的默认值一律不带供应商身份，值写空字符串——**不用假 URL 占位**：整条链路以 `empty()` 判断"未配"，非空占位值会让 [[Provider 壳]]的兜底失效。真实端点/模型名的默认值留在壳里（ADR-0004 不变）。插件初始化时由 core 注入合并后的配置，插件不自行解析配置。
- 配置写回：`persist` **点对点**——读文件、只改目标键、原子写回，不 dump 内存树。默认值因此永不渗进用户的 `settings.json`，文件里只有用户自己配过的东西。文件不存在按空对象处理；坏 JSON 则不写并返回失败（宁可 `/model` 不生效，也不能拿内存树覆盖掉读不懂的用户数据）。
- 配置合并粒度：**对象递归合并，数组与标量整个替换**。数组不合并是刻意的——`disabled: ["deepseek"]` 的语义是"就禁这一个"，不是"在默认基础上再加一个"。
- 不做配置热重载（ADR-0010）：启动时读一次，之后 core 不再看 `settings.json`。手改配置需重启 core。取消的理由是运行时重读引出的问题没有干净解：core 是常驻服务、TUI 是独立进程，stderr 报错没人看得见；报错退出会断掉所有客户端且有误杀风险（部分编辑器与 shell 重定向"先清空再写"，事件循环有概率读到半截文件）。没有运行时重读，就没有运行时坏文件。
- 协议插件可配项：api_key / base_url / model / small_model，**全部可缺省**（缺省即空串，由 [[Provider 壳]]兜底）。装了壳时只配 `api_key` 就能跑。base_url 与 api_key 同级——代理/网关用户（OpenRouter / one-api / 内网中转）必须能自定义端点。
- 模型档位（[[Model Tier]]）：`model` 主模型 + `small_model` 小模型两档，共用 base_url / api_key。档位只换模型名，不换端点凭证——跨供应商小模型不做（真需要时再让 dialog 携带端点 override）。core 侧 `Config::model(ModelTier)` 是唯一知道键名的地方，协议插件/SDK/ABI 无感。**core 不做档位间回落**：`small_model` 没配就是空字符串，原样经 `dialog["model"]` 下传，填什么由 [[Provider 壳]]决定（壳只看见一个空模型名，分辨不出是哪一档——档位到 core 为止）。裸协议层无壳时空模型名由服务端报错，core 不代为判断。
- 会话目录不是配置项：`.realagent/sessions` 是 core 自己的落盘路径（`Config::session_dir()`，相对 cwd），写死在 core 里，settings.json 写它不生效。
- 斜杠命令：插件可注册（通用能力，不限 type）。首版只留接口不实现；`/quit` core 内置，`/new` `/resume` 由 session-manager 插件提供。`/model` core 内置（改的是配置，不是插件能力）：无参列 [[Model]] 注册表，带名切主模型并写回 settings.json；只认注册表里的模型——交互式选择就该从已知的里挑。
  - 实况注（2026-08-16）：`/new` `/resume` 现为 **core 内置**（`handle_command`，见 `core/src/main.cpp`），session-manager 已删，会话持久化由 core 直接落地（见下条 [[Session]] 实况注）。`/quit` **不归 core**——退出的是客户端进程，现为 TUI 本地命令（与 `/statusline` 同类）。`/model` 的"注册表"已随 ADR-0012 取消，改为向[[当前 provider]]现问现答，"只认清单里的模型"这条行为不变。至今**无任何插件**提供 `command.list` / `command.execute`。
- 首版插件清单：见 `docs/plugins.md`（6 个插件：v1-messages + deepseek 壳协议链 / core-tools / perm-allow-all / perm-ask / session-manager）。
  - 实况注（2026-08-16）：**5 个**——session-manager 已删（理由见目录结构一节）。`docs/plugins.md` 已是 5 个的口径。
- 通信协议：见 `docs/PROTOCOL.md`（可靠流请求-响应 + 推送流，全可靠 + 0-RTT）。
- 架构：core 为常驻服务，客户端（TUI/未来 gui）通过 **QUIC/HTTP3** 连接（ADR-0006）。REST 语义（POST /message、POST /approval-response 等）；推送流为 HTTP/3 长生命周期单向流（SSE 语义），**全可靠**（增量与事件无差别，QUIC 可靠流原生保证，无自建确认机制）。0-RTT 快握手。支持公网部署。
- QUIC 库：core 用 **Cloudflare quiche**（QUIC + HTTP/3 一体，`core/CMakeLists.txt:17-19`）；TUI 用 quic-go。msquic 于 2026-08-09 被弃（纯传输层无 H3 语义），ADR-0006 当时记的替代品是 ngtcp2 + nghttp3，但落地的是 quiche——**此次换库无 ADR 记录，理由未知**（ADR-0006 与本条原文均未更新，ADR-0002 已在用"quiche 非线程安全"作论据）。
- 出站 Provider 请求：core 用 libcurl（HTTP/1.1 客户端，请求 DeepSeek）；入站客户端通信走 QUIC。
- 工具结果：JSON 回传，结构为 `status + messages`。
- JSON 实现：参考 `~/revlm/backend/include/util/json.hpp`（boost::json 封装：默认 `{}`、链式 operator[]、宽容 parse、optional 提取器）。
- DeepSeek 接入：端点 `https://api.deepseek.com/anthropic`，模型 `deepseek-v4-flash`（或 `deepseek-v4-pro`），API key 见 platform.deepseek.com。工具调用与流式完整支持；`cache_control` 被忽略（验证首版无需 vendor 层）。
- 参考资料：`OPENCODE_RESEARCH.md`（OpenCode 架构调研）。

## 目录结构

```
realagent/                  # 主仓库（core + tui + docs）
├── core/                   # C++ QUIC/HTTP3 服务（ADR-0006）
│   ├── include/            #   公共头文件：config.hpp / json.hpp + agent/ extension/ server/
│   ├── src/
│   │   ├── extension/      #   宿主词汇：CoreHost + 管线槽位解析（slots.cpp，ADR-0013）
│   │   ├── agent/          #   agent loop、事件流、状态、工具执行、审批
│   │   ├── server/         #   QUIC/HTTP3 服务（quiche）、推送流、审批端点
│   │   ├── config.cpp      #   配置：默认树 + settings.json 覆盖 + 点对点写回
│   │   └── main.cpp        #   启动、事件循环、端点回调、斜杠命令
│   ├── tests/
│   └── CMakeLists.txt
├── tui/                    # Go + Bubble Tea 客户端（ADR-0007）
│   ├── cmd/realagent-tui/
│   └── internal/
├── docs/                   # ADR 等文档
├── CMakeLists.txt          # 顶层：core 构建 + go build（TUI）
├── CONTEXT.md
└── OPENCODE_RESEARCH.md    # （未重建）

realugin/                   # 独立 git 仓库（插件体系，ADR-0013）
├── include/realugin/       #   plugin_api.h（C ABI，插件侧唯一依赖）+ host.hpp / loader.hpp
├── src/loader.cpp          #   发现 / dlopen / ABI 校验 / 能力索引 / deps DAG / 启停级联
└── cmake/AddPlugin.cmake   #   realugin_add_plugin() —— 插件工程的建库助手

realagent-plugins/          # 独立 git 仓库（插件单开）
├── v1-messages/            #   协议层：提供「生成请求」「解析响应」两段，供应商中立
├── deepseek/               #   供应商壳：提供「改请求」「计价」两段 + 模型清单
├── core-tools/             #   工具：read/edit/bash（在 plugins.unprefixed 名单内，用短名）
├── perm-allow-all/         #   权限：占权限槽
└── perm-ask/               #   权限：审批链路测试（裁决 ask）——与 perm-allow-all 二选一
```

共 5 个容器。`session-manager/` 已于 2026-08-16 删除：它交出的 `/new` `/resume` 与 core 内置的同名命令重复且只是空壳，两个命令现由 core 直接处理（`core/src/main.cpp:242/246`）。

`core/src/` 下 `ai/` `tools/` `permission/` 三个目录**不存在，也不再规划**（2026-08-16 核实）：
- `ai/`——Provider 抽象整体外移到插件，core 只按[[管线]]依次调四段能力，没有留给"AI 层"的职责（`core/include/ai/` 确实存在但是**空目录**，是早期骨架的残留）。
- `tools/`——工具注册表随 ADR-0012 删除，工具清单向容器现问现答；执行调度在 `agent/executor.cpp`。
- `permission/`——权限裁决是插件（ADR-0005），core 侧只剩审批协调器，在 `agent/approval.cpp`（`AwaitApproval` 状态机 + `permission_request` 帧）。
