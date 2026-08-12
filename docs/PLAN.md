# 实现规划（Phase 1）

> 里程碑拆解 + 技术风险清单 + 调研发现。规划中发现的问题在此暴露，逐项拷问后回填。
> 架构决策见 CONTEXT.md 与 docs/adr/（0001-0007）。

## 里程碑总览

```
M0 骨架与构建链      → M1 插件加载链   → M2 工具注册执行
M3 Agent Loop/事件流 → M4 Provider 插件 → M5 QUIC 通信
M6 TUI              → M7 集成与测试
```

依赖关系：M0 → M1 → M2 → M3 → M4 → M5 → M6，M7 收尾。M3/M4 可部分并行（事件流引擎与 Provider 插件相对独立，但 M4 的响应解析要喂给 M3 的事件流）。

## 调研背景速览（详细调研见各 ADR 与 OPENCODE_RESEARCH.md）

| 项目 | 语言 | 核心参考点 |
|---|---|---|
| Pi（earendil-works/pi） | TypeScript | 极简核心 + 插件架构；agent 事件流；ExtensionAPI；JSONL 会话树 |
| OpenCode（已归档） | Go | Bubble Tea TUI；Provider 抽象；SQLite；pub/sub 事件 |
| Codex（openai/codex） | Rust/TS | app-server 架构；沙箱执行；patch 系统；skills/plugins 分层扩展 |
| Goose（block/goose） | Rust | MCP 扩展 + 自定义发行版；V8 内嵌 |

## M0：骨架与构建链

**交付物**
- 目录结构落地（CONTEXT.md 中的布局：core/ tui/ plugins/ docs/）
- 双语言构建编排：CMake 统一入口，自定义 target 调 `go build`（TUI），core + plugins 走原生 CMake
- C++26 协程可行性 spike（Clang 19+ / brew llvm）
- boost::json 封装（revlm 风格）、spdlog 接入
- 插件 SDK 头文件 v0：plugin.json 结构、类型枚举、生命周期接口初版

**已定实现约定**
- 测试框架：C++ 用 Catch2（单头集成），Go 用内置 testing
- API key：`.realagent/settings.json` 的 `api_key`（唯一来源，不读 env、无默认）

**技术要点**
- JSON 封装参考 `~/revlm/backend/include/util/json.hpp`：boost::json 子类，默认 `{}`、链式 `operator[]`（读缺键返回 null 不抛异常、写自动构造对象/数组）、宽容 parse（全文失败后截取首尾 `{}` 重试——为 LLM 输出带额外文本设计）、optional 提取器（as_string/as_int64 返回 optional 不抛异常）。
- Clang C++26 协程：C++20 协程核心完全支持（P0912R5，macOS 全支持）；P2561 非抛出协程未实现但本项目不依赖（见 ADR-0003）。

**风险**
- [x] **R1: Clang 19+ 的 C++26 协程支持度**——**已解除（2026-08-09）**：Clang 对 C++20 协程核心完全支持；P2561 未实现但本项目不依赖（异步引擎基于 C++20 协程能力即可）。ADR-0003 已修正。
- [x] R2: msquic 与 C++/CMake 集成——**已废弃**：QUIC 库改为 ngtcp2 + nghttp3（ADR-0006）。

## M1：插件加载链

**交付物**
- plugin.json 解析（名称/描述/版本/ABI 版本/前置依赖/type）
- dlopen 加载 + ABI 版本强校验（PI_ABI_VERSION，不等则拒绝并提示重编）
- 类型分发：protocol / tool / permission / session 四种生命周期接口
- 前置依赖声明解析 + 嵌套组装（依赖注入）
- perm-allow-all、session-manager 两个插件骨架

**技术要点**
- 插件加载链是第一阶段的**交付关键路径**（core 零内置工具，一切能力来自插件）。
- 插件配置由 core 统一注入（env > settings.json > 默认），插件初始化接口接收配置节。
- 事件订阅：单入口 `on_event(handle, event*)`，插件内按 type 字符串分发。

**风险**
- [x] **R3: 插件事件订阅接口**——**已定（2026-08-09）**：单入口 + 插件内分发（ADR-0001）。
- [ ] R4: 嵌套组装时机——加载时静态组装 vs 调用时动态解析（影响插件间引用的生命周期管理）

## M2：工具注册与执行

**交付物**
- 工具注册表（宏 + JSON schema 字符串注册，如 `PI_TOOL_DEFINE("read", "{...}")`）
- 严格顺序执行调度（单 agent 内工具一个接一个）
- 权限检查点（core 钩子 → perm-allow-all）
- core-tools 插件：read / edit / bash（**一个插件注册三个工具**）
- 工具结果 JSON：`{status, messages}` 结构

**技术要点**
- write 不单独存在——write 是 edit 的 `+x-0` 特例（空文件追加 = 创建），LLM 创建文件用 edit。
- edit 工具描述写清「目标文件不存在时创建新文件；old_string 为空表示从空文件/开头追加」——创建语义直接进描述，让模型学会创建文件用 edit。
- bash stdout 作为 tool_output 帧走推送流（实时可见），命令完整输出在 tool_result 帧回传。
- 权限检查点独立于工具实现——同一检查点适用于所有危险工具（write/bash）。

**风险**
- [x] **R5: edit 的 +x-0（write）语义**——**已定（2026-08-09）**：创建语义写进工具描述，M2 实现时实测调优文案。
- [x] R6: bash 流式输出——**已定（2026-08-09）**：stdout 走 tool_output 帧（推送流，全可靠），tool_result 帧回传完整输出。

## M3：Agent Loop + 事件流

**交付物**
- 异步事件流引擎：agent_start / turn_start / message_* / tool_execution_* / turn_end / agent_end
- 单 agent，工具严格顺序
- 中止（abort）信号
- 会话状态 + JSONL 读写（新建/恢复/列表，无 fork）

**技术要点**
- **Loop 形态**：`while(1)`——调 LLM → 逐个收事件 → 产生事件时广播给订阅者列表（fan-out 到推送流/插件/日志）→ 顺序执行工具 → 结果回传 → 直到 LLM 完成。协程仅用于 LLM 流式请求的异步等待。
- 会话 JSONL schema：每行一个 JSON 对象，append-only；type 区分 user / assistant / tool_call / tool_result；call_id 关联工具调用与结果；含 id / ts / content / model 字段。
- Steering：第一版只支持中止（abort），不支持中途插话。

**风险**
- [x] **R7: 事件流实现形态**——**已定（2026-08-09）**：while(1) + 事件广播 fan-out（ADR-0002）。
- [ ] R8: 中止传播——LLM 流式中断 + 工具执行中断的统一信号模型
- [x] R9: JSONL 会话 schema——**已定（2026-08-09）**：类型区分 + call_id 关联，append-only。

## M4：Provider 插件

**交付物**
- v1-messages 插件（协议层）：`/v1/messages` 构造 + SSE 解析（成对，供应商中立，无默认端点/模型）
- deepseek 壳插件（供应商壳）：`deps: v1-messages`，包住协议层，兜底 DeepSeek 默认（嵌套链，ADR-0004）
- libcurl 出站流式请求（HTTP/1.1，出站 Provider 通信）
- 请求管线（core 收集 → 协议链入口构造 → 发出 → 解析 → 事件流）

**技术要点**
- 协议/供应商拆分：`/v1/messages` 是协议（多家公司共用），协议层不识别供应商；供应商默认值下沉到壳。
- 端点与模型名无默认：core 侧必配（缺则启动失败），协议层与壳都不该再兜底。
- 可配置：api_key / base_url / model / small_model，全部经 `.realagent/settings.json`，不读 env。base_url 与 api_key 同级重要（代理/网关用户必须能自定义端点）。
- DeepSeek 兼容端点细节（已查证）：tools/tool_use/tool_result 全支持；tool_choice 四模式全支持；stream 全支持；`cache_control` 被忽略；thinking 的 budget_tokens 忽略；thinking 块带 signature（回传历史用）。
- 壳不做模型映射：`claude-*` 原样透传（供应商特殊逻辑一律不进壳）。

**风险**
- [x] **R10: DeepSeek /v1/messages 与 Anthropic 标准差异**——**已解除（2026-08-09）**：工具调用 + 流式完整支持（见上）。
- [x] R11: 流式解析增量 → 事件流 message_update 的对接——**已完成（2026-08-10）**：thinking 三帧 + message_update + tool_use 全打通，经壳透传。

## M5：QUIC 通信

**交付物**
- ngtcp2（QUIC）+ nghttp3（H3/QPACK）server（core）+ quic-go client（TUI）
- HTTP/3 REST 端点：POST /message、POST /command、POST /approval-response、GET /sessions、POST /session
- 推送流（GET /events 长生命周期单向流，SSE 语义，全可靠）

**技术要点**
- 全可靠流：增量与结构化事件无差别，QUIC 可靠流原生保证，无自建确认机制（见 PROTOCOL.md 演进史）。
- 0-RTT 握手（重复连接握手与数据同发）。
- 数据报（RFC 9221）首版不使用，未来有真正可替换状态再引入。

**风险**
- [x] **R12: HTTP/3 应用层**——**已解决（2026-08-09）**：ngtcp2 + nghttp3（标准组合保互操作），弃 msquic（纯传输层无 H3 语义）。
- [x] **R13: 推送可靠性机制**——**已定（2026-08-09，全可靠流）**：全推送走 HTTP/3 可靠流（SSE 语义）。废弃数据报/时间戳水位/捎带发送（那是重复实现 TCP）。
- [x] R14: quic-go 支持度——**已解除**：自 v0.27 支持 RFC 9221 数据报，HTTP/3 成熟（首版不使用数据报）。

## M6：TUI

**交付物**
- Go + Bubble Tea 界面（参考 claude code / codex 客户端，**无状态栏**）
- quic-go 客户端
- 消息流渲染（流式打字效果，由推送流帧到达驱动）
- 输入框、审批对话框

**技术要点**
- TUI 职责纯粹：渲染 + 用户输入 + QUIC 客户端；一切 agent 逻辑在 core。
- Bubble Tea Model/Update/View 模式；异步事件（推送流）→ tea.Msg 桥接。

**风险**
- [x] R15: Bubble Tea 事件模型与 QUIC 异步事件流集成（异步事件 → tea.Msg 的桥接）——**已完成（2026-08-10）**：TUI 订阅 /events 推送流渲染流式打字 + 审批对话框（permission_request → y/n → POST /approval-response）。

## M7：集成与测试

**交付物**
- 全链路验证：工具注册 → LLM（DeepSeek）→ 权限 → 执行 → 回传
- 单元测试（C++ Catch2 + Go testing）
- 打包分发（core 二进制 + 插件 .so + tui 二进制）

---

## 风险清单汇总

| # | 问题 | 状态 |
|---|---|---|
| R1 | Clang C++26 协程支持度 | ✅ 已解除（C++20 协程核心足够） |
| R2 | msquic 集成 | ✅ 已废弃（换 ngtcp2+nghttp3） |
| R3 | 插件事件订阅接口 | ✅ 已定（单入口 on_event 分发） |
| R4 | 嵌套组装时机 | ✅ 已实施（2026-08-10）：拓扑序加载 + get_dependency 依赖注入 |
| R5 | edit +x-0 的 LLM 描述 | ✅ 已定（创建语义进描述） |
| R6 | bash 流式输出 | ✅ 已定（tool_output 帧走推送流） |
| R7 | 事件流实现形态 | ✅ 已定（while(1) + fan-out） |
| R8 | 中止传播模型 | ⬜ 待定（M3） |
| R9 | JSONL 会话 schema | ✅ 已定（类型区分 + call_id） |
| R10 | DeepSeek 协议差异 | ✅ 已解除（工具调用 + 流式全支持） |
| R11 | 流式解析 → message_update 对接 | ✅ 已完成（2026-08-10）：thinking 三帧 + message_update + tool_use 经壳透传 |
| R12 | HTTP/3 应用层 | ✅ 已解决（ngtcp2+nghttp3） |
| R13 | 推送可靠性机制 | ✅ 已定（全可靠流） |
| R14 | quic-go 支持度 | ✅ 已解除 |
| R15 | Bubble Tea 事件桥接 | ✅ 已完成（TUI 流式 + 审批对话框） |
