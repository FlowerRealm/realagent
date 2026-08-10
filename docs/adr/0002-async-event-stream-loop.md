# ADR-0002: Agent Loop 采用异步事件流引擎

- 状态：已接受（动机澄清 2026-08-09）
- 日期：2026-08-09

## 决策

Agent Loop 采用**异步事件流引擎**（完整 Pi 风格）：核心是一个事件生产器，向外发布异步可迭代的事件流；消费方（TUI、扩展、日志）与生产方完全解耦。事件类型对齐 Pi：`agent_start / turn_start / message_start / message_update / message_end / tool_execution_start / tool_execution_end / turn_end / agent_end`。

## 调研对标：Pi 的 Agent 运行时（本项目主要参考）

Pi 的 `pi-agent-core` 包提供状态化 Agent 框架（事件流 + 工具执行 + 状态管理），关键机制：

- **消息管线**：两阶段转换后才到达 LLM——`transformContext`（可选：裁剪旧消息/注入外部上下文）+ `convertToLlm`（必须：过滤 UI-only 消息、自定义类型转标准 LLM 格式）。LLM 只懂三种角色：`user` / `assistant` / `toolResult`。
- **两个入口**：`agentLoop(prompts, context, config, ...)` 启动新循环；`agentLoopContinue(context, ...)` 从现有上下文恢复（最后一条必须是 user 或 toolResult）。
- **Turn 结构**：一个 Turn = 一次 LLM 调用 + 全部工具执行。工具被调用时自动继续循环，结果回传 LLM 进下一 Turn。`shouldStopAfterTurn` 可在完整 Turn 后阻止下一次 LLM 调用。
- **工具执行模式**：parallel（默认，preflight 串行验证后并发执行）或 sequential（一个接一个）；单工具可声明 `executionMode` 覆盖，某工具声明 sequential 则整批串行。
- **生命周期钩子**：`beforeToolCall`（参数验证后可拦截执行、可 attach `terminate:true`）、`afterToolCall`（可改结果或 attach terminate）。
- **终止提示**：工具结果 / 被拦截的 beforeToolCall 结果 / afterToolCall 覆盖都可返回 `terminate:true`，跳过自动的后续 LLM 调用；仅当整批工具结果都设置时才生效。
- **错误处理**：工具失败应**抛异常**而非返回错误内容；异常被捕获后作为 `isError:true` 的工具错误回传 LLM。
- **Steering 与 Follow-up 队列**：steering 消息在工具运行中插入（工具跑完后注入，LLM 下轮响应）；follow-up 消息排队在 agent 本应停止之后。两种模式：`one-at-a-time`（默认）与 `all`。
- **状态**：AgentState（systemPrompt / model / thinkingLevel / tools / messages / isStreaming / streamingMessage / pendingToolCalls / errorMessage）。

OpenCode 的 Agent Loop（参考）：stateless agentic loop——`Run(userMessage) <-chan AgentEvent` 产生 goroutine，`processGeneration()` 无限循环：`streamAndHandleEvents()` 流式收事件 → finish reason == ToolUse 则 append assistant msg + 工具结果后循环，否则 Done。`Cancel()` 通过 context 取消，工具执行收 "Tool execution canceled by user" 错误。

## 候选方案与权衡

1. **同步驱动 + 事件回调（后台线程）** —— 最简，`agent.run(input)` 同步推进，回调更新 UI。代价：中断/steering（工具执行中途插入新用户输入）、多 agent 并发协作都需要手工拆线程，扩展 hook 受限于回调签名。
2. **异步事件流引擎（选定）** —— 生产/消费完全解耦，天然支持**多 agent 并发编排**（核心动机）、steering 消息插入。代价：C++ 需要协程或事件队列基础设施，复杂度显著更高。
3. **单线程全同步** —— 工具执行时 UI 卡死，不可用。

选定方案 2。

## 动机澄清与第一版范围（2026-08-09）

**动机澄清**：异步引擎的意义在于**多 agent 并发编排**——每个 agent 内部工具**严格顺序执行**（一个接一个），但多个 agent 之间并发调度。长时工具（如 bash）后续通过 background 后台运行机制处理，不依赖工具并行。

**第一版范围**：第一版只交付**单 agent**，异步事件流的架构红利先行用于 TUI 流式渲染与扩展 hook 解耦；多 agent 编排接口预留，作为第二版里程碑。

**事件流实现形态（2026-08-09）**：Agent loop 就是一个 `while(1)`——调 LLM → 逐个收事件 → 产生事件时广播给订阅者列表（fan-out 到推送流/插件/日志）→ 顺序执行工具 → 结果回传 → 直到 LLM 完成。协程仅用于 LLM 流式请求的异步等待。无 generator / 队列-消费者 / 回调链等额外形态。

## 后果

- 需要引入异步基础设施（协程，见 ADR-0003）。
- 所有对外扩展点（扩展 hook）都建立在事件流之上——扩展订阅事件而非调用回调。
- core 与 TUI 通过事件流解耦，TUI 只是事件消费者之一。
- **单 agent 工具严格顺序执行**，无文件级互斥需求（不会同 agent 内并发写文件）。
- 长时工具（bash 等）规划 background 后台运行机制，不并入工具并行方案。
