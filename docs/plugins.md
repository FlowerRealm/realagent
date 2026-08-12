# 首版插件清单（Phase 1）

> 本文档列出第一阶段（core + tui）交付的插件。插件形态与生命周期见 ADR-0001（C ABI 动态库）、ADR-0004（Provider 插件化）、ADR-0005（权限插件化）。
> 共 6 个插件，分四类（protocol / tool / permission / session）。

## 插件通用约定

- **形态**：C ABI 动态库（`.so`/`.dylib`），运行时目录扫描 + `dlopen` 加载。
- **元数据**：插件目录下独立 `plugin.json`（名称/描述/版本/ABI 版本/前置依赖/type）。
- **配置注入**：core 统一收集配置（env > `.realagent/settings.json` > 默认），插件初始化时注入，插件不自行解析。
- **事件订阅**：单入口 `on_event(handle, event*)`，插件内按 type 字符串分发。
- **版本校验**：单一接口版本号 PI_ABI_VERSION 强校验，不符则拒绝加载。
- **自动发现目录**：项目级 `.realagent/extensions/` 与全局 `~/.realagent/extensions/`（对齐 CONTEXT.md 配置约定）。
- **依赖注入**：插件 init 中经 `core->api->get_dependency(name, &api, &inst)` 取前置插件接口表（嵌套组装，ADR-0004）；core 按依赖序加载保证内层就绪。

---

## 协议链（type = protocol，嵌套）

`/v1/messages` 是一种协议，不是某家公司的私有物——Anthropic、DeepSeek、OpenRouter 等多家公司都实现同一端点。因此协议被拆成两层（ADR-0004 嵌套链）：

```
deepseek 壳（外层，供应商身份 + 默认配置兜底）
   └─ 包住 ─→ v1-messages（内层，协议固有：请求构造 + SSE 解析 + thinking 块）
```

core 只调协议链入口（最外层、未被其他协议插件依赖者），链内包裹由插件自身经 `get_dependency` 完成。

### 1a. `v1-messages`（内层，type = protocol）

`/v1/messages` 协议的完整适配器：请求构造 + SSE 响应解析（**成对**，ADR-0004）。本插件**不识别任何供应商**——请求结构、SSE 事件、thinking 块都是协议固有内容，供应商差异（端点/模型/凭证默认值）由外层壳负责。

**职责**：
- 请求构造：对话信息 → `/v1/messages` JSON 请求体（system / messages / tools / tool_choice / stream）。
- 响应解析：SSE 流 → 结构化事件（message_update 文本增量 / thinking 三帧 / tool_use / 停止原因）经 C ABI 回传 core。
- thinking 块：协议固有内容，原样回传历史（带 signature，缺失时省略）。

**配置**：base_url / api_key / model 均**不设供应商默认值**——端点留空、模型留空、无 Authorization。由外层壳兜底，或直接用配置直连（协议层本身可独立运行）。

### 1b. `deepseek`（外层壳，type = protocol，deps: v1-messages）

DeepSeek 供应商壳：包住 `v1-messages`。壳只做两件事——
1. **声明供应商身份**：`deps: ["v1-messages"]` 让 core 解析协议链入口为本壳；
2. **兜底供应商默认配置**：端点默认 `https://api.deepseek.com/anthropic`、模型默认 `deepseek-v4-flash`、缺 Authorization 补凭证。

**无任何供应商特殊逻辑**：不做模型映射（`claude-*` 原样透传）、不重写请求结构。协议层留空处填默认，其余原样透传；`parse_feed` 纯透传（响应解析无供应商差异）。

**嵌套链职责**（ADR-0004）：core 按依赖序加载（`v1-messages` 先），`deepseek` init 中 `get_dependency("v1-messages", ...)` 取内层接口表，`build_request` 调内层构造初步请求→壳兜底默认→产出最终请求；`parse_feed` 调内层。新增第二个供应商（如 OpenRouter）时另写一个壳声明 `deps: ["v1-messages"]`，复用同一协议层，core 据依赖自动选入口。

**可配置项**（core 统一注入，唯一来源 `settings.json`，全部必配）：

| 配置 | 说明 |
|---|---|
| api_key | 凭证，无默认 |
| base_url | 端点，无默认 |
| model | 主模型名，无默认 |
| small_model | 小模型名，无默认（不回落主模型） |

> 缺任一项 core 启动即失败并点名——core 不猜端点、不猜模型，也不读 env。
> **base_url 与 api_key 同级重要**：代理/网关用户（OpenRouter / one-api / 内网中转）必须能自定义端点。

**DeepSeek 兼容端点已查证细节**（2026-08-09，官方文档）：
- tools 字段（name/input_schema/description）全支持；`cache_control` 被忽略
- tool_choice 四模式（none/auto/any/tool）全支持；`disable_parallel_tool_use` 忽略
- tool_use / tool_result 块全支持（is_error 忽略）
- stream 全支持；thinking 的 budget_tokens 忽略

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

## 3. `perm-allow-all` / `perm-ask`（type = permission）

最小权限策略插件。`perm-allow-all` 永远允许，目标不是安全，而是打通 `core 检查点 → 权限插件裁决 → 工具执行` 的链路验证（ADR-0005）。`perm-ask` 是审批链路测试插件：每次裁决返回 ask，触发 core → 客户端的审批询问流程。

**与未来审批的关系**：
- `perm-allow-all`：裁决永远 = 允许（不触发 UI）。
- `perm-ask`：裁决永远 = ask → core 作为发起方向 TUI/gui 发询问（permission_request 帧走推送流），界面渲染对话框，裁决经 `POST /approval-response` 回传。
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

协议链 + 工具 + 权限构成全链路验证：

```
core → deepseek（壳：兜底默认）→ v1-messages（协议层：构造请求）→ libcurl → /v1/messages 端点
     ← SSE 解析（v1-messages：thinking/message_update/tool_use）→ 逐层透传回 core
     → core-tools（read/edit/bash）→ 权限检查点 → perm-allow-all / perm-ask 裁决 → 执行
     → tool_result 回传 → 下一 Turn（thinking 块带 signature 原样回传历史）
     （会话管理：session-manager 提供 /new /resume）
```

