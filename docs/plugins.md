# 首版插件清单（Phase 1）

> 本文档列出第一阶段（core + tui）交付的插件。插件形态与生命周期见 ADR-0001（C ABI 动态库）、ADR-0004（Provider 插件化）、ADR-0005（权限插件化）。
> 共 4 个插件，分四类（protocol / tool / permission / session）。

## 插件通用约定

- **形态**：C ABI 动态库（`.so`/`.dylib`），运行时目录扫描 + `dlopen` 加载。
- **元数据**：插件目录下独立 `plugin.json`（名称/描述/版本/ABI 版本/前置依赖/type）。
- **配置注入**：core 统一收集配置（env > `.realagent/settings.json` > 默认），插件初始化时注入，插件不自行解析。
- **事件订阅**：单入口 `on_event(handle, event*)`，插件内按 type 字符串分发。
- **版本校验**：单一接口版本号 PI_ABI_VERSION 强校验，不符则拒绝加载。
- **自动发现目录**：项目级 `.realagent/extensions/` 与全局 `~/.realagent/extensions/`（对齐 CONTEXT.md 配置约定）。

---

## 1. `deepseek-messages`（type = protocol）

Anthropic `/v1/messages` 协议的完整适配器：请求构造 + SSE 响应解析（**成对**，ADR-0004）。core 保持零协议知识，本插件是唯一懂 Anthropic 协议的组件。

**端点与模型**：
- 默认端点 `https://api.deepseek.com/anthropic`
- 默认模型 `deepseek-v4-flash`（可选 `deepseek-v4-pro`）

**可配置项**（core 统一注入，env > settings.json > 默认）：

| 配置 | env 变量 | 默认 |
|---|---|---|
| api_key | `ANTHROPIC_API_KEY` | — |
| base_url | `DEEPSEEK_BASE_URL` | `https://api.deepseek.com/anthropic` |
| model | `DEEPSEEK_MODEL` | `deepseek-v4-flash` |

> **base_url 与 api_key 同级重要**：代理/网关用户（OpenRouter / one-api / 内网中转）必须能自定义端点。

**职责**：
- 请求构造：对话信息 → Anthropic `/v1/messages` JSON 请求体（system / messages / tools / tool_choice / stream）。
- 响应解析：SSE 流 → 结构化事件（message_delta 文本增量 / tool_use / 停止原因）经 C ABI 回传 core。

**DeepSeek 兼容端点已查证细节**（2026-08-09，官方文档）：
- tools 字段（name/input_schema/description）全支持；`cache_control` 被忽略
- tool_choice 四模式（none/auto/any/tool）全支持；`disable_parallel_tool_use` 忽略
- tool_use / tool_result 块全支持（is_error 忽略）
- stream 全支持；thinking 的 budget_tokens 忽略
- claude-haiku/sonnet 名映射到 deepseek-v4-flash，claude-opus 映射到 deepseek-v4-pro

**嵌套**：首版不单独实现 vendor 精修层（DeepSeek 兼容端点走标准 `/v1/messages` 字段，cache_control 被忽略——vendor 层无收益）。嵌套结构（ADR-0004）保留为架构接口，出现第二个供应商时再实现 vendor 插件（声明前置依赖包裹本插件）。

---

## 2. `core-tools`（type = tool）

一个插件注册三个工具，走统一注册接口（对 core 来说只是注册表条目，与第三方 .so 无差别）：

| 工具 | 职责 | 安全属性 |
|---|---|---|
| `read` | 读文件 → 内容回传 | 只读 |
| `edit` | 改文件；`+x-0`（空文件追加）= 创建文件 | 危险（过权限检查点） |
| `bash` | 执行 shell 命令 | 危险（过权限检查点） |

**无独立 `write` 工具**——write 是 edit 的 `+x-0` 特例（空文件追加 = 创建）。LLM 创建文件用 edit，工具描述写清创建语义（"目标文件不存在时创建新文件；old_string 为空表示从空文件/开头追加"）。

**执行细节**：
- 单 agent 内工具严格顺序执行（ADR-0002）。
- bash stdout 实时走 tool_output 帧（推送流，全可靠）；命令完整输出在 tool_result 帧回传。
- 工具结果 JSON 结构：`{status, messages}`。
- 参数 Schema：宏 + JSON 字符串字面量（如 `PI_TOOL_DEFINE("read", "{...}")`）。

**权限**：read 只读不触发；write/bash 触发权限检查点 → perm-allow-all 插件裁决（首版永远允许）。

---

## 3. `perm-allow-all`（type = permission）

永远允许的最小权限策略。目标不是安全，而是打通 `core 检查点 → 权限插件裁决 → 工具执行` 的链路验证（ADR-0005）。

**与未来审批的关系**：
- 首版：裁决永远 = 允许（不触发 UI）。
- 未来：真实审批插件返回 ask 时，core 作为发起方向 TUI/gui 发询问（permission_request 帧走推送流），界面渲染对话框，裁决经 `POST /approval-response` 回传。
- 权限插件不感知任何交互界面，core 是中枢（ADR-0005）。

---

## 4. `session-manager`（type = session）

会话管理插件：注册 `/new` `/resume` 命令（首版只留命令注册接口，不实现具体命令逻辑）。`/quit` 由 core 内置（生命周期类命令不依赖插件）。

**命令归属**（ADR-0001）：
- `/quit` → core 内置
- `/new` `/resume` → 本插件（type = session）
- 斜杠命令是通用注册能力，不限插件 type；首版只留接口不实现

---

## 验证闭环

四个插件构成全链路验证：

```
core → deepseek-messages（构造请求）→ libcurl → DeepSeek API
     ← SSE 解析 → 事件流（agent loop）→ LLM 调用工具
     → core-tools（read/edit/bash）→ 权限检查点 → perm-allow-all 裁决 → 执行
     → tool_result 回传 → 下一 Turn
     （会话管理：session-manager 提供 /new /resume）
```
