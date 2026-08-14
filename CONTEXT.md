# CONTEXT.md — realagent 术语表

> 本文件只收录本项目的领域术语。通用编程概念不收录。
> 定义描述事物"是什么"，不描述"做什么"。

## Agent Loop（代理循环）

一次对话的驱动核心。一个 **Turn**（轮次）= 一次 LLM 调用 + 该调用产生的所有工具执行；LLM 继续调用工具则进入下一 Turn，直到 LLM 停止调用工具为止。

_Avoid_: `agentic loop`、`对话循环`

## Turn（轮次）

一次 LLM 调用及其后续的全部工具执行。Turn 是 Loop 的最小推进单位。

_Avoid_: `iteration`、`round`

## Tool（工具）

LLM 可调用的具名函数。带名称、描述、参数 Schema（JSON Schema，插件侧用宏 + JSON 字符串字面量定义），执行后返回结构化结果。错误通过抛异常/返回错误信号传达，而非塞进正常结果里。

_Avoid_: `function`、`command`（Tool ≠ 用户斜杠命令）

## Event（事件）

Loop 向订阅者（TUI、扩展、日志）发布的生命周期消息。典型序列：
`agent_start → turn_start → message_start → message_update* → message_end → tool_execution_start → tool_execution_end → turn_end → agent_end`

事件是**异步事件流**的一部分（ADR-0002）：生产方与消费方解耦，扩展通过订阅事件获得能力。异步引擎的动机是**多 agent 并发编排**，而非单 agent 工具并行。

_Avoid_: `callback`（事件是广播的，回调是一对一的）

## Multi-Agent（多代理编排）

异步事件流的核心动机：多个 agent 实例并发调度（如主 agent 派生子任务）。每个 agent 内部仍保持工具严格顺序执行。

_Avoid_: `sub-agent`（特指派生出来的子 agent，Multi-Agent 是泛指）

## Background（后台运行）

长时工具（bash 等）的规划机制：工具可在后台运行，agent 不阻塞等待，用户可查看进度。未来并入，不属于第一阶段核心。

_Avoid_: `async tool`（工具本身是同步顺序执行的，后台只是运行策略）

## Extension（扩展）

向 core 注入能力的外部代码单元：可注册 Tool、命令、事件处理器。以 **C ABI 动态库**（`.so`/`.dylib`）形式存在，运行时由 core 目录扫描 + `dlopen` 加载。见 [[ADR-0001]]。

_Avoid_: `plugin`（本项目统一用 Extension 命名）

## Provider（模型提供商）

LLM 后端的统一抽象。**适配本身走插件机制**（ADR-0004）：core 收集对话信息 → 协议层插件构造请求 → 供应商壳兜底默认 → core 发出。core 不内置任何具体协议知识，也不认任何供应商身份。

_Avoid_: `backend`、`LLM client`（指 core 内置模块时）

## Model Tier（模型档位）

一次 LLM 调用用哪一档模型：**主模型**（`model`，对话主链路）或**小模型**（`small_model`，标题/摘要一类杂活）。档位只是模型名的选择，两档共用同一 base_url / api_key ——不是独立的供应商配置。core 侧 `Config::model(ModelTier)` 解析档位，结果照旧经 `dialog["model"]` 透传给协议链，[[Provider 壳]]与协议层对档位无感。两档均为必需配置——不配就起不来，没有回落。

_Avoid_: `fast model`、`模型 profile`（档位不带端点凭证，不构成完整 profile）

## v1-messages（协议层）

`/v1/messages` 是一种协议，不是某家公司的私有物——Anthropic、DeepSeek、OpenRouter 等多家公司都实现同一端点。协议层插件只懂协议固有内容（请求结构 + SSE 解析 + thinking 块），不识别任何供应商、不设端点/模型默认值。

_Avoid_: `anthropic api`（Anthropic 是一家公司，协议本身供应商中立）

## Provider 壳（供应商适配）

在协议层之上包裹的薄壳插件：声明 `deps` 包住协议层，core 据此解析协议链入口；运行时兜底该供应商的默认配置（端点/模型/凭证），不做协议改写、不做模型映射。同一协议层下可叠加多个供应商壳（DeepSeek、OpenRouter…），core 按依赖链自动选入口。

供应商壳与协议层是**嵌套链**关系（装饰器模式）：壳 init 中经 `get_dependency` 取内层接口表，build_request 调内层构造初步请求再补默认，parse_feed 默认透传、按需拦截改写（外层有权修改内层输出，见 [[Plugin Nesting]]）。

_Avoid_: `vendor shim`、`provider plugin`（壳是供应商身份 + 兜底，不是协议层）

## Protocol Plugin（协议层插件）

负责把对话信息按特定协议构造为请求体、并解析该协议响应的插件（**成对**：构造 + 解析）。现阶段 `/v1/messages`（多家公司共用，详见 [[v1-messages]]）。协议层供应商中立，默认配置下沉到 [[Provider 壳]]。

_Avoid_: `provider plugin`（协议层 ≠ 供应商层，见下）

## Provider Adapter（供应商适配）

在协议层之上包裹的薄壳插件（见 [[Provider 壳]]）：声明前置依赖包住协议层，core 据此解析协议链入口；兜底该供应商的默认配置（端点/模型/凭证），不做协议改写、不做模型映射。复用同一协议层，新增供应商只写新壳。

供应商壳与协议层插件是**嵌套链**关系（装饰器模式）：壳经 `get_dependency` 取协议层接口表，build_request 调协议层初步构造再补默认，parse_feed 默认透传、按需拦截改写。core 不内置任何供应商默认值。

计价属于供应商身份，落在壳里：壳拦下协议层解析出的 token 用量事件，按本次模型查自己的[[模型数据表]]乘出钱数，只向上 sink [[Cost]]，token 到壳为止不再上传。壳同时经 `list_models` 把模型清单（不含单价）报给 core。

## Plugin Nesting（插件嵌套）

插件间的包裹关系：外层插件持有内层插件引用，调用时逐层传递、逐层修改。核心场景是供应商适配包裹协议层插件。外层可对内层输出做任意修改。

组装由**声明驱动**：插件元数据声明前置插件依赖，core 加载时自动组装并依赖注入，插件自身不做运行时互查。

_Avoid_: `layered plugins`（嵌套是洋葱状，不是扁平分层）

## 架构原则：钩子与策略分离（core 哲学）

core 只提供**检查点/钩子**（工具执行前、请求构造时），所有**策略外置为插件**：Provider 适配是插件（ADR-0004）、权限审批是插件（ADR-0005）。core 是编排者，不是决策者。新需求的判断标准：这是 core 的编排职责，还是可替换的策略？策略一律进插件。

_Avoid_: `core 内置策略`（除非 ADR 明确拍板）

## 插件元数据（Plugin Metadata）

插件携带的声明信息：名称、版本、前置插件依赖、接口版本。core 据此自动组装嵌套链。独立 JSON 文件（`plugin.json`）。

_Avoid_: `manifest`

## Module（模块）

core 内部的职责分区（ai / agent / tools / extension），以目录 + 命名空间划分，**不**编译为独立库。C++ 多库拆分成本高，单库分模块是刻意选择。

_Avoid_: `package`、`library`（指 core 内部模块时）

## Session（会话）

一段连续的对话记录，含消息历史、状态、元数据。以 **JSONL 文件**持久化（一行一条消息/事件），目录结构支持会话树（分支/fork）。人可读、可 diff、可进 git。

_Avoid_: `conversation`（除非与 Session 区分出明确差异）

## Session Tree（会话树）

会话间的父子关系，支持从任一会话 fork 分支。JSONL 文件目录结构天然表达。

_Avoid_: `branch`（Git 语义混淆）

## Thinking（思考）

assistant 消息中的一种 content block 类型（`{"type":"thinking","thinking":...,"signature":...}`），承载模型的推理过程（DeepSeek v4 reasoning）。抽象对话里原样保留，协议插件负责与 Anthropic `thinking` 块互转；core 流式转发 `thinking_update` 事件，TUI 渲染为 dim 斜体块。signature 用于回传历史时校验，缺失时省略。

_Avoid_: `reasoning_content`（DeepSeek 原生 API 字段名，Anthropic 兼容端点映射为 thinking 块）

## Model（模型元数据）

一个模型"是什么"：名称、归属供应商、上下文窗口。**不含计价**——单价是 [[Provider 壳]] 的私事，core 只收 [[Cost]] 的最终数字。

与 [[Model Tier]] 划清：Tier 是"这次调用用哪一档"，Model 是"那一档指向的模型是什么货色"。

数据归**插件**所有，不是 core 的配置：[[Provider 壳]]自己读自己的模型数据表（[[模型数据表]]），只把 core 用得着的部分经 `list_models` 报上来。**单价不报**——core 不算钱，存了没有消费方。

core 不内置任何模型数据，也不校验用户配的模型名在不在表里——**表是参考资料，不是白名单**：配了表外的模型照发不误，不检查、不警告、不兜底。

_Avoid_: `模型清单`（指单个模型时）、`provider model`

## 模型数据表（models.json）

[[Provider 壳]]自有的模型数据文件，含单价在内的全部字段。两个位置，**不合并**：包内 `models.json` 是出厂数据（打包产物，只读，随插件升级更新）；运行时目录 `~/.realagent/models/<插件名>.json` 是用户接管版，**存在即整体接管**，出厂数据一概不看。

解析严格：缺字段、格式错即插件加载失败并报错原文，不跳过坏条目、不补默认值。

_Avoid_: `模型配置`（它不走 [[配置]] 那套 core 注入机制，是插件自己的数据）

## Cost（花费）

一次 run 花掉的钱（USD）。由 [[Provider 壳]] 按本次调用的模型查自己的[[模型数据表]]算出——**单价与 token 用量都不出插件**，core 只收最终数字。core 按 agent 实例跨 turn 累加，随 [[status_update]] 帧下发；一次用户输入起算清零，帧内为累计绝对值。

_Avoid_: `usage`、`token 统计`（core 侧已无 token 概念，只有钱）

## status_update（运行态帧）

插件向客户端报运行态数据的推送帧，**开放键集**：插件报什么键就是什么键，core 除 `cost`（需累加）外一律不认识、原样转发。落点是 [[Status（状态行）]]——本次 run 的实时数字，run 结束即消失。

_Avoid_: `cost 帧`（单值帧型是死路，加第二个值就得破坏客户端）

## Statusline（状态栏）

输入框**下方**的常驻栏：`🤖 model | 📁 dir | 🌿 git`。内容是**这次会话的身份信息**——哪个模型、哪个目录、哪个分支，会话期内基本不变，故启动拿一次即可，不常驻刷新循环。展示偏好（显示哪几段、emoji 还是 nerd font）纯客户端状态，core 不认。

_Avoid_: `状态行`（那是活动区里的另一条，见下）

## Status（状态行）

[[活动区（Live Region）]]里的读秒行：`⠋ 思考中… (1m23s · $0.0123 · esc 中断)`。内容是**本次 run 的实时数字**（读秒、[[Cost]]），数据经 [[status_update]] 帧推送。run 结束整行消失——它描述"正在发生的事"，事结束了就没有它。

_Avoid_: `statusline`（那是输入框下方那条常驻栏）

## 活动区（Live Region）

TUI 底部由 Bubble Tea 每帧重绘的区域：未定型行 + 审批框 + 斜杠菜单 + 读秒状态行 + 输入框。其上方是**终端原生 scrollback**——已定型的行由 TUI 打进去后不再归 TUI 管（[[ADR-0008]]）。活动区绝不能高过终端。

_Avoid_: `视口`、`viewport`（本项目不自建滚动缓冲）

## 定型（Freeze）

一行渲染内容不再变化、可以打进 scrollback 的状态。判据只有一条：**追加式文本的贪心折行前缀稳定**，故除最后一个折行外全部定型（[[ADR-0008]]）。

_Avoid_: `finalize`（原指把 streaming 消息收进列表，行模型下已无此概念）

---

## 已拍板（暂存，随决策更新）

- 项目定位：AI 编码 agent，第一阶段 core + tui（均 C++），gui 后续。
- 架构基调：极简核心 + 插件/扩展架构，参考 Pi（earendil-works/pi）。
- core 分层参考：ai（Provider 抽象）→ agent（Loop/状态/事件）→ tools（注册与执行）→ extension（扩展宿主）。
- core 为**单一库**，模块以目录 + 命名空间划分，不拆独立库。
- 扩展机制：C ABI 动态库（ADR-0001）。
- 语言标准：C++26，异步基于协程（ADR-0003）。
- 会话持久化：JSONL 文件 + 会话树。
- 网络传输：core 内置 libcurl（同步 + SSE 流式）。
- 日志：spdlog（第三方依赖）。
- core 第三方依赖：libcurl + spdlog 两个（FTXUI 属 TUI 层）。
- TUI：Go + Bubble Tea（ADR-0007）。参考 claude code / codex 客户端外观，无状态栏。
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
- 计价：全在 [[Provider 壳]]，core 只收 [[Cost]] 并按 agent 实例跨 turn 累加，经 [[status_update]] 帧下发（本次 run 累计绝对值，客户端覆盖写）。token 不跨 core 边界，`Usage` 结构体删除。
- 模型数据表：插件自有，不走 core 配置注入；运行时目录用户版存在即整体接管出厂版，不合并。
- 不兜底原则：模型表解析失败即硬错，模型重名后写覆盖不检测，配置的模型名不在表里不检查不警告。少写代码优先于替用户擦屁股。
- 插件 type：protocol / tool / permission / session 四类（ADR-0001）。
- 插件元数据：独立 JSON 文件（`plugin.json`），含名称/描述/版本/ABI 版本/前置依赖。
- 配置产物与打包产物分离：用户可改的一律在运行时目录（`~/.realagent/settings.json`、`~/.realagent/models.json`），插件目录只放打包产物（`plugin.json` + 动态库 + 出厂 `models.json`），重装即覆盖，不劝用户改。
- 配置机制：凭证与偏好的唯一来源是 `settings.json`（全局 `~/.realagent/` 打底，项目级 `.realagent/` 覆盖）。**无 env 覆盖、无内置默认、无回落**：必需键（api_key / base_url / model / small_model）缺一个 core 就退出并点名缺哪个；配置文件存在但解析失败同样是硬错。core 不认任何供应商身份，端点与模型名一律由用户配置。插件初始化时由 core 注入配置节，插件不自行解析配置。
- 协议插件可配项：api_key / base_url / model / small_model，全部必配。base_url 与 api_key 同级——代理/网关用户（OpenRouter / one-api / 内网中转）必须能自定义端点。
- 模型档位（[[Model Tier]]）：`model` 主模型 + `small_model` 小模型两档，共用 base_url / api_key。档位只换模型名，不换端点凭证——跨供应商小模型不做（真需要时再让 dialog 携带端点 override）。两档都必配，不存在小模型回落主模型这种隐式默认。core 侧 `Config::model(ModelTier)` 是唯一知道键名的地方，协议插件/SDK/ABI 无感。
- 会话目录不是配置项：`.realagent/sessions` 是 core 自己的落盘路径，写死在 core 里，settings.json 写它不生效。
- 斜杠命令：插件可注册（通用能力，不限 type）。首版只留接口不实现；`/quit` core 内置，`/new` `/resume` 由 session-manager 插件提供。`/model` core 内置（改的是配置，不是插件能力）：无参列 [[Model]] 注册表，带名切主模型并写回 settings.json；只认注册表里的模型——交互式选择就该从已知的里挑。
- 首版插件清单：见 `docs/plugins.md`（6 个插件：v1-messages + deepseek 壳协议链 / core-tools / perm-allow-all / perm-ask / session-manager）。
- 通信协议：见 `docs/PROTOCOL.md`（可靠流请求-响应 + 推送流，全可靠 + 0-RTT）。
- 架构：core 为常驻服务，客户端（TUI/未来 gui）通过 **QUIC/HTTP3** 连接（ADR-0006）。REST 语义（POST /message、POST /approval-response 等）；推送流为 HTTP/3 长生命周期单向流（SSE 语义），**全可靠**（增量与事件无差别，QUIC 可靠流原生保证，无自建确认机制）。0-RTT 快握手。支持公网部署。
- QUIC 库：core 用 **ngtcp2 + nghttp3**（标准 QUIC + HTTP/3 C 组合，2026-08-09 替换 msquic——msquic 纯传输层无 H3 语义）；TUI 用 quic-go。
- 出站 Provider 请求：core 用 libcurl（HTTP/1.1 客户端，请求 DeepSeek）；入站客户端通信走 QUIC。
- 工具结果：JSON 回传，结构为 `status + messages`。
- JSON 实现：参考 `~/revlm/backend/include/util/json.hpp`（boost::json 封装：默认 `{}`、链式 operator[]、宽容 parse、optional 提取器）。
- DeepSeek 接入：端点 `https://api.deepseek.com/anthropic`，模型 `deepseek-v4-flash`（或 `deepseek-v4-pro`），API key 见 platform.deepseek.com。工具调用与流式完整支持；`cache_control` 被忽略（验证首版无需 vendor 层）。
- 参考资料：`OPENCODE_RESEARCH.md`（OpenCode 架构调研）。

## 目录结构

```
realagent/                  # 主仓库（core + tui + docs）
├── core/                   # C++ QUIC/HTTP3 服务（ADR-0006）
│   ├── include/core/       #   公共头文件（对外 API）
│   ├── src/
│   │   ├── extension/      #   插件加载、C ABI 边界、plugin.json 解析
│   │   ├── agent/          #   agent loop、事件流、状态
│   │   ├── ai/             #   请求管线（协议插件协作）、SSE 解析骨架
│   │   ├── tools/          #   工具注册表、执行调度
│   │   ├── permission/     #   权限检查点（钩子）
│   │   └── server/         #   QUIC/HTTP3 服务（ngtcp2+nghttp3）、推送流、审批端点
│   ├── sdk/                #   插件 SDK 头文件（C ABI，自包含纯 C，对外发布）
│   ├── tests/
│   └── CMakeLists.txt
├── tui/                    # Go + Bubble Tea 客户端（ADR-0007）
│   ├── cmd/realagent-tui/
│   └── internal/
├── docs/                   # ADR 等文档
├── CMakeLists.txt          # 顶层：core 构建 + go build（TUI）
├── CONTEXT.md
└── OPENCODE_RESEARCH.md    # （未重建）

realagent-plugins/          # 独立 git 仓库（插件单开）
├── sdk/                    #   引用 core 仓库的 SDK 头（submodule 或拷贝）
├── v1-messages/           #   type = protocol（协议层：/v1/messages 构造 + 解析，供应商中立）
├── deepseek/               #   type = protocol（供应商壳：包住 v1-messages，兜底 DeepSeek 默认）
├── core-tools/             #   type = tool（read/edit/bash）
├── perm-allow-all/         #   type = permission
├── perm-ask/               #   type = permission（审批链路测试：裁决 ask）
└── session-manager/        #   type = session
```
