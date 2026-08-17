# 首版插件清单（Phase 1）

> 本文档列出第一阶段（core + tui）交付的插件。形态与生命周期见 ADR-0001（C ABI 动态库）、ADR-0005（权限插件化）、ADR-0011（能力槽）、**ADR-0012（能力是一个函数；管线取代嵌套；core 不建注册表）**。
> 共 5 个容器。**容器没有类型**——下文按能力归类只是给人看的组织方式，core 只看它交出的能力表（ADR-0012）。

## 插件通用约定

- **形态**：C ABI 动态库（`.so`/`.dylib`），运行时目录扫描 + `dlopen` 加载。
- **元数据**：容器目录下独立 `plugin.json`（名称/描述/版本/ABI 版本/`deps`；Provider 壳另有 `protocol`）。**无 `type` 键**。容器名须匹配 `^[a-zA-Z0-9-]+$`——禁止下划线，否则命名空间前缀无法无歧义解析。
- **能力表**：容器经 `capabilities()` 交出一张 `{名字, 函数}` 表，**一个能力 = 一个名字 + 一个函数**。`plugin_api_t` 固定五项（`abi_version`/`name`/`init`/`destroy`/`capabilities`），**新增能力 = 新增一个键**，不动结构体、不破 ABI、不重编无关容器。core 认识的键：`request.build` / `request.refine` / `response.parse` / `usage.meter` / `tool.list` / `tool.execute` / `tool.interrupt` / `command.list` / `command.execute` / `permission.decide` / `model.list` / `event.observe`；不认识的键 core 不解释（容器之间的私约，经 `import` 取用）。
- **管线**：一次 LLM 调用由 core 分段调用，容器互不认识、互不调用：

  ```
  对话 ──request.build──▶ 粗请求 ──request.refine──▶ 精请求 ──[core 发出]──▶ 响应流
                                                                          │
        事件 ◀──usage.meter── usage ◀──response.parse────────────────────┘
  ```

  四段的归属由用户选定的 `provider` 一次确定：它提供改请求与计价，它 `plugin.json` 的 `protocol` 字段指名的容器提供生成请求与解析响应。
- **依赖**：`deps` 声明**容器**（不是能力）。core 按 `deps` 建反图、自入度 0 起 BFS 逐层 init；剩余节点即环，点名失败。禁用一个容器会**级联停用**依赖它的容器（级联者不写入禁用清单）。
- **命名空间**：容器交出的工具，对外名字为 `<插件名>_<工具名>`、命令为 `<插件名>:<命令名>`（前缀由 core 拼，容器只认自己的本名）。配置 `plugins.unprefixed` 名单内的插件用短名（首版含 `core-tools`）。每个条目恰好一个对外名字，最终名字撞车即加载失败。
- **内存两条规则**：容器长期持有的（工具清单、模型清单等静态表）走**借阅**——`const` 指针，有效期 = 容器在位时长，两侧都不释放；本次调用现造的（请求 JSON、执行结果）走**转移**——经 `core->api->alloc` 分配、core 释放。
- **配置注入**：core 统一收集配置（代码默认树打底 < `settings.json` 覆盖，不读 env），插件初始化时注入合并结果，插件不自行解析。
- **事件订阅**：能力键 `event.observe`，单入口，插件内按 type 字符串分发。可合并——所有订阅者收到同一份事件流（与推给客户端的完全一致），**旁听不是拦截**，无返回值。同步调用、跑在 emit 的线程上（运行期 = agent 线程），回调里做慢事情会卡住 agent；回调内可以 `emit`，事件照送客户端但不再回灌插件（core 有重入守卫）。
- **版本校验**：单一接口版本号 `PLUGIN_ABI_VERSION` 强校验，不符则拒绝加载。
- **SDK 与加载器来自 realugin**（ADR-0013）：头文件 `<realugin/plugin_api.h>`，CMake 用随它安装的 `realugin_add_plugin()`；容器发现、`dlopen`、能力索引、`deps` DAG、启停级联都在 realugin 里。core 只留自己的词汇——管线四段与权限槽的解析（`core/src/extension/slots.cpp`）、配置/禁用清单/免前缀名单（`CoreHost`）。
- **自动发现目录**：项目级 `.realagent/extensions/` 与全局 `~/.realagent/extensions/`（对齐 CONTEXT.md 配置约定）。
- **查询**：`core->api->providers(能力名)` 回答"现在有谁提供它"。能力索引在全部 `plugin_create` 之后、任何 `init` 之前建成，因此答案与 init 顺序无关、永远完整——容器据此判断现在有什么，再决定自己怎么做。

---

## 协议与供应商（管线上的两组能力）

`/v1/messages` 是一种协议，不是某家公司的私有物——Anthropic、DeepSeek、OpenRouter 等多家公司都实现同一端点。因此"协议固有的"与"供应商身份"分属两个容器，各自提供管线上的能力：

```
v1-messages   提供 request.build（对话 → 粗请求）、response.parse（流 → 事件）
deepseek      提供 request.refine（粗请求 → 精请求）、usage.meter（用量 → 钱）、model.list
```

**两者互不认识**：deepseek 不调 v1-messages，也不知道粗请求是谁生成的。core 按管线依次调用。装了多个供应商容器时全部初始化，只有 `provider` 指名的那个上线（`/provider` 切换即换指针，无需重载）。

### 1a. `v1-messages`（协议容器）

提供两个**各自独立**的能力（不是一对）：`request.build` 与 `response.parse`。本容器**不识别任何供应商**——请求结构、SSE 事件、thinking 块都是协议固有内容，供应商差异（端点/模型/凭证默认值）在管线的下一段补。

**职责**：
- `request.build`：对话信息 → 粗请求 JSON `{url, headers, body}`（system / messages / tools / tool_choice / stream）。url 只有路径、model 可为空、可无凭证。
- 响应解析：SSE 流 → 结构化事件（message_update 文本增量 / thinking 三帧 / tool_use / 停止原因）经 C ABI 回传 core。
- thinking 块：协议固有内容，原样回传历史（带 signature，缺失时省略）。

**配置**：base_url / api_key / model 均**不设供应商默认值**——端点留空、模型留空、无 Authorization。由管线上的"改请求"那一段补，或直接用配置直连（本容器可独立工作：没有 provider 时它的粗请求原样发出）。裸用且不配 `base_url` 时拼出的是相对 URL，libcurl 会报 `URL using bad/illegal format`——这是"不装供应商容器直连"这条路径的固有代价，core 不为它做启动校验（ADR-0010）。

### 1b. `deepseek`（供应商容器，`protocol: v1-messages`）

DeepSeek 供应商身份，提供三个能力：
1. **`request.refine`**：收一个粗请求、还一个精请求——url 补端点 `https://api.deepseek.com/anthropic`、model 留空则填 `deepseek-v4-flash`、缺 Authorization 则补凭证。**它不认识 v1-messages，也不调任何插件**；
2. **`usage.meter`**（ADR-0009）：token 用量 → 钱。按本次生效的模型（在 refine 时记下）查自己的模型数据表算金额，core 把结果作为 `status_update {"cost"}` 下发——**token 到此为止不再上传**；
3. **`model.list`**：把 `name/owned_by/context` 报给 core（**单价不报**，借阅）。

`plugin.json` 的 `protocol: "v1-messages"` 声明它的精修是照着哪个协议的请求形状写的——选中本容器为 provider，即同时定下管线的另外两段。

**无任何供应商特殊逻辑**：不做模型映射（`claude-*` 原样透传）、不重写请求结构，只填空处。

**模型数据表**：路径由 core 给（`get_config("models_path")`）——用户接管版 `~/.realagent/models/deepseek.json` 存在就用它，否则用包内出厂版 `models.json`，两者不合并。解析严格：文件在但格式错、条目缺字段即 init 失败（core 随即卸载该插件）。

**新增一个供应商**（如 OpenRouter）：另写一个容器，能力表里填 `request.refine` / `usage.meter` / `model.list`，`plugin.json` 写 `protocol: "v1-messages"`，配置 `provider: openrouter` 即可。**core 不重编、SDK 不动、其它容器不动**——这是 ADR-0012 能力表的直接收益。

**可配置项**（core 统一注入：代码默认树打底 < `settings.json` 覆盖，全部可缺省）：

| 配置 | core 默认值 | 缺省时的实际行为 |
|---|---|---|
| api_key | `""` | 不发 Authorization 头，服务端 401 |
| base_url | `""` | 容器 init 填 `https://api.deepseek.com/anthropic` |
| model | `""` | `request.refine` 填 `deepseek-v4-flash` |
| small_model | `""` | 空模型名下传，同样由 `request.refine` 填默认（core 不回落主模型） |

> core 的默认值一律是空串，**不带供应商身份、也不用假 URL 占位**：兜底链路以 `empty()` 判断"未配"，任何非空占位值都会让兜底失效。真实默认值只存在于供应商容器里。
> 装了本容器时，用户只需要配 `api_key` 就能跑起来。
> **base_url 与 api_key 同级重要**：代理/网关用户（OpenRouter / one-api / 内网中转）必须能自定义端点。

**DeepSeek 兼容端点已查证细节**（2026-08-09，官方文档）：
- tools 字段（name/input_schema/description）全支持；`cache_control` 被忽略
- tool_choice 四模式（none/auto/any/tool）全支持；`disable_parallel_tool_use` 忽略
- tool_use / tool_result 块全支持（is_error 忽略）
- stream 全支持；thinking 的 budget_tokens 忽略

---

## 2. `core-tools`（工具）

一个容器提供 `tool.list` + `tool.execute` + `tool.interrupt` 三个能力，交出三个工具。**core 不建工具表**——每轮对话现问现答（ADR-0012）。本容器在配置 `plugins.unprefixed` 名单内，**故 LLM 见到的是短名** `read` / `edit` / `bash`；不在名单内的插件其工具名为 `<插件名>_<工具名>`（Messages API 约束工具名匹配 `^[a-zA-Z0-9_-]{1,64}$`，斜杠点号非法）：

| 工具 | 职责 | 安全属性 |
|---|---|---|
| `read` | 读文件 → 内容回传 | 只读 |
| `edit` | 改文件；`+x-0`（空文件追加）= 创建文件 | 危险（过权限检查点） |
| `bash` | 执行 shell 命令 | 危险（过权限检查点） |

**无独立 `write` 工具**——write 是 edit 的 `+x-0` 特例（空文件追加 = 创建）。LLM 创建文件用 edit，工具描述写清创建语义（"目标文件不存在时创建新文件；old_string 为空表示从空文件/开头追加"）。

**执行细节**：
- 单 agent 内工具严格顺序执行（ADR-0002）。
- bash stdout 实时走 `tool_output` 帧（推送流，全可靠）：读循环每读到一段就 `core->api->emit` 一帧 `{call_id, stream, text}`，命令完整输出仍在 tool_result 里回传——两者不是二选一，帧是给人看的实时反馈。超上限后只吞不推（管道还得读干净，中途撒手等于给命令一个 SIGPIPE）。
- **可中断**：本容器提供 `tool.interrupt`。bash 子进程自成**进程组**（`setpgid`），中止时打的是整组——首次 SIGTERM 给命令一个自己收尾的机会，用户再按一次就 SIGKILL，命令拉起的子孙树不留孤儿。
  - `tool.interrupt` 是 SDK 里**唯一一处并发进入插件**的地方（`tool.execute` 还没返回，agent 线程正卡在里面），实现只"捅一下"就返回；reap 与拼结果留在 `tool.execute` 那条线上做。
  - 不提供这个能力的容器 = 它的工具不可中断，core 照实等它跑完，不假装成功。
- 工具结果 JSON 结构：`{status, messages}`；`messages` 经 `core->api->alloc` 分配，core 读完释放（转移）。
- 参数 Schema：宏 + JSON 字符串字面量（如 `PI_TOOL_DEFINE("read", "{...}")`）。

**权限**：read 只读不触发；write/bash 触发权限检查点 → perm-allow-all 插件裁决（首版永远允许）。

---

## 3. `perm-allow-all` / `perm-ask`（权限，二选一）

**权限槽独占**：两者都提供 `permission.decide`，同时装载则该槽**空置并点名双方**（dangerous 工具一律拒绝）。用哪个由 `plugins.disabled` 禁掉另一个决定。槽位独占也意味着只有一个裁决者——`PLUGIN_PERM_ALLOW` 就是字面意义的放行，不存在覆盖他人裁决的问题。

最小权限策略插件。`perm-allow-all` 永远允许，目标不是安全，而是打通 `core 检查点 → 权限插件裁决 → 工具执行` 的链路验证（ADR-0005）。`perm-ask` 是审批链路测试插件：每次裁决返回 ask，触发 core → 客户端的审批询问流程。

**与未来审批的关系**：
- `perm-allow-all`：裁决永远 = 允许（不触发 UI）。
- `perm-ask`：裁决永远 = ask → core 作为发起方向 TUI/gui 发询问（permission_request 帧走推送流），界面渲染对话框，裁决经 `POST /approval-response` 回传。
- 权限插件不感知任何交互界面，core 是中枢（ADR-0005）。

---

## 4. 斜杠命令

**命令归属**：
- `/new` `/resume` `/plugins` `/model` `/provider` → core 内置
- 斜杠命令是通用能力，任何容器都能提供（能力表里填 `command.list` + `command.execute`）；core 现问现答拉清单，按名分发。对外名为 `<容器>:<命令>`，进 `plugins.unprefixed` 名单则用短名。
- 首版**没有任何插件提供命令**——原 `session-manager` 容器已于 2026-08-16 删除：它交出的 `/new` `/resume` 与 core 内置重复，且只是回一句"会话存储尚未落地"的空壳。会话管理待 JSONL 会话存储落地时按那时的设计重建。

> `/quit` 曾被记为 core 内置，实际**不归 core**：退出的是客户端进程，core 是常驻服务、还连着别的客户端。现为 TUI 本地命令（与 `/statusline` 同类），照常出现在斜杠菜单里，但从不发给 core。

---

## 验证闭环

管线 + 工具 + 权限构成全链路验证：

```
core → v1-messages.request.build（对话 → 粗请求）
     → deepseek.request.refine（补端点/模型/凭证）
     → libcurl → /v1/messages 端点
     → v1-messages.response.parse（SSE → thinking/message_update/tool_use/usage）
     → deepseek.usage.meter（用量 → cost）→ status_update
     → core-tools（read/edit/bash）→ 权限检查点 → perm-allow-all / perm-ask 裁决 → 执行
     → tool_result 回传 → 下一 Turn（thinking 块带 signature 原样回传历史）
```

