# ADR-0001: 扩展机制采用 C ABI 动态库

- 状态：已接受
- 日期：2026-08-09

## 决策

core 的扩展（Extension）机制采用 **C ABI 动态库**：插件编译为 `.so`/`.dylib`，通过稳定的 C 接口（纯数据结构 + 函数指针）与 core 交互，运行时目录扫描自动发现并 `dlopen` 加载。**明确不使用 C++ ABI**——C++ ABI 跨编译器/标准库实现不稳定（名字改编、异常、STL 布局无统一保证），C ABI 是唯一在 macOS/Linux/不同编译器间都稳定的接口形式。

## 调研对标（本项目的扩展设计参考了什么）

| 项目 | 扩展机制 | 对本案的启示 |
|---|---|---|
| **Pi**（earendil-works/pi，86k stars） | TypeScript 模块，jiti 动态加载（免编译），导出工厂函数 `(pi: ExtensionAPI) => void`。ExtensionAPI 提供 `registerTool`（工具）/ `registerCommand`（斜杠命令）/ `registerProvider`（模型提供商）/ `registerShortcut` / `registerFlag` / `registerMessageRenderer`（自定义 TUI 渲染）/ 事件订阅（`pi.on(event, handler)`），**可覆盖内置工具**（read/bash/edit/write/grep/find/ls）。自动发现：`~/.pi/agent/extensions/*.ts` 与 `.pi/extensions/*.ts`（项目级需 trust 后加载）。 | 本项目插件机制的主要参考：插件能力面（工具/命令/Provider/事件）、目录自动发现、覆盖内置工具的"来源无差别"原则 |
| **Codex**（openai/codex，105k stars） | 多层扩展：skills（`.SKILL.md` 指令包）、subagents（`.toml`）、plugins（`plugin.json` 打包 skills + MCP servers）、hooks（shell 脚本）、AGENTS.md。Rust 侧有 `core-plugins` crate。 | plugin.json 作为插件元数据容器的想法（本项目用独立 JSON 文件） |
| **OpenCode**（opencode-ai/opencode，已归档） | MCP（stdio/SSE）作为唯一扩展点，配置于 `.opencode.json` | MCP 是语言无关但走子进程 + 协议，C ABI 动态库对单进程 core 更直接 |
| **Goose**（block/goose） | MCP + 自定义发行版（CUSTOM_DISTROS.md：预配置 providers/extensions/branding） | "内置插件随发行版分发"的想法（本项目 plugins/ 目录） |

**为什么不用这些现成方案**：Pi 的 TS 模块同进程加载依赖 JS 运行时（本项目是 C++）；MCP 是进程间协议（引入子进程管理、协议开销，且需要 MCP 客户端实现）；静态注册（编译期）违背插件独立演进。C ABI 动态库是 C++ 生态做插件的标准做法，兼顾"动态发现 + 独立编译 + 语言无关"。

## 候选方案与权衡

1. **C ABI 动态库（选定）** —— 插件独立编译，语言无关（C/Rust/Go 皆可写插件），动态发现贴近 Pi 的扩展模型。代价是接口边界需精心设计，靠版本号管理兼容。
2. **静态注册（编译期）** —— 零运行时复杂度、类型安全，但插件无法独立演进，加插件必须重编整个二进制，违背插件生态初衷。
3. **内嵌脚本引擎（Lua/Wren）** —— 上手快、隔离好，但多一层解释器运行时，且核心数据需跨语言边界往返。

选定方案 1。

## 插件形态（细分决策，均 2026-08-09）

### 插件类型（type 字段）

> ⚠️ **本节已被 ADR-0011 取代（2026-08-15）。** `type` 从未约束过任何东西——加载期只校验 `plugin.json` 的字符串等于 `api->type` 枚举的名字，从不校验枚举与函数指针是否一致；`tool` 在执行路径上零作用，`session` 全仓库零处判断。现改为：能力由非空函数指针决定，core 侧落在**能力槽**上，槽位独占。下文保留原文备查。

插件元数据必须带 `type` 字段，决定插件的生命周期接口表：

- `protocol` —— Provider 链的协议层（请求构造 + 响应解析**成对**，见 ADR-0004）
- `tool` —— 工具插件（注册一个或多个工具）
- `permission` —— 权限策略（挂 core 检查点，见 ADR-0005）
- `session` —— 会话管理插件（注册 `/new` `/resume` 等会话命令）

### 插件元数据

**独立 JSON 文件**（`plugin.json`），字段：名称、描述、版本、ABI 版本、前置依赖声明。core 加载时解析元数据做版本校验与嵌套组装。文件格式不限 JSON 类（可扩展），不做二进制内嵌——元数据与代码分离，core 加载前即可读。

### 工具参数 Schema

**宏 + JSON 字符串字面量**（如 `PI_TOOL_DEFINE("read", "{...}")`）。与语言无关、零代码生成、零额外依赖；core 存 JSON 字符串，转发给 LLM 前轻量校验。放弃：
- 编译期模板反射 —— 仅 C++ 受益、模板报错难读，且跨语言插件（Rust/Go）享受不到；
- 独立 .json + 代码生成 —— 多一套构建工具链，违背极简。

### ABI 版本管理

**单一接口版本号（PI_ABI_VERSION）**，core 加载插件时强校验——插件编译版本与 core 支持版本不等则拒绝加载并提示重编。项目 <1.0 期间改接口即 bump 版本、全部插件重编，**不做兼容矩阵**。

### 能力注册

插件除主 type 接口外，可**注册斜杠命令**（通用能力，不限 type）。首版只留命令注册接口，不实现具体命令。命令归属：`/quit` 由 core 内置（生命周期类命令不依赖插件）；`/new` `/resume` 由会话管理插件（type = session）提供。

> ⚠️ **本节已被 ADR-0011 / ADR-0012 取代（2026-08-16 复核）。** 无"主 type 接口"之说，也**没有"注册"这回事**：`register_command` 及 core 侧的命令表已删除。任何容器都能提供命令，做法是在能力表里填 `command.list` + `command.execute` 两个函数（不是"注册"）。core 不建表，每次要用时向 `command.list` 拉一遍，用完即弃（ADR-0012）。命令名带命名空间前缀（`<容器>:<命令>`）。「命令是通用能力，不限容器」这一判断本身不变。
>
> 内置命令的实际清单与本节原文不符：`/new` `/resume` `/plugins` `/model` `/provider` 为 core 内置。`/quit` **不归 core**——退出的是客户端进程，core 是常驻服务、还连着别的客户端，它没有"退出"这个概念；现由 TUI 就地处理（与 `/statusline` 同类，见 PROTOCOL.md「命令」节）。`session-manager` 容器已于 2026-08-16 删除——它交出的 `/new` `/resume` 与 core 内置重复且只是空壳；会话持久化已由 core 直接落地（JSONL，PLAN.md R9），不再需要这个容器。

### 事件订阅

插件实现**单入口 `on_event(handle, event*)`**，core 分发所有运行事件，插件按 `event->type` 字符串内部分发。新增事件类型不破坏 ABI（type 是字符串，非枚举）。选择单入口而非固定回调清单（清单改接口要 bump 版本全量重编）或显式订阅 API（C ABI 需要双向注册 + 订阅状态管理，复杂）。

> ⚠️ **落点已随 ADR-0012 变更（2026-08-16）。** 单入口这一判断不变，但它不再是 `plugin_api_t` 上的一个字段，而是能力表里的一个键 `event.observe`（签名 `void(plugin_t*, const plugin_event_t*)`）——容器填了才收事件，没填 core 就不叫它。可合并能力：所有订阅者都收到同一份，顺序 = 加载序。**旁听，不是拦截**：无返回值，改不了也拦不下事件。
>
> 扇出点是 `CoreContext::emit_fn` 这一个出口（`PluginManager::observe`），所以插件看到的事件流与推送给客户端的完全一致。两条契约写在 `plugin_api.h` 的签名注释上：①**同步调用，跑在 emit 的线程上**（运行期事件 = agent 线程），回调里做慢事情就是卡住 agent；②回调内可以调 `core_api->emit`，事件照常送客户端但**不再扇出给插件**（core 有 thread_local 重入守卫），否则两个互相 emit 的订阅者就是无限递归。

## 后果

- 插件可用任意能产出 C ABI 动态库的语言编写。
- core 需要一套精心设计的 C 接口 + 版本号（见后续 ADR/SDK 文档）。
- **内置工具（read/edit/bash 等）第一阶段即全部以 C ABI 插件形态落地**，core 零内置工具。工具对 core 来说只是某个容器 `tool.list` 交出的一条，来源（内置 .so 或第三方 .so）无差别。插件加载链是第一阶段的交付关键路径。

  > ⚠️ 原文作"注册表条目"，ADR-0012 已删除 core 侧的工具/命令/模型三张表，改为现问现答。
- 插件初始化时由 core 注入其配置节（api_key / base_url / model 等），插件不自行解析配置（见 CONTEXT.md 配置机制）。
