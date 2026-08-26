# ADR-0016：废除插件系统，能力并入 core

- 状态：已采纳（2026-08-25）
- 取代：ADR-0001（C ABI 扩展）、ADR-0004（Provider 适配走插件）、ADR-0005（权限插件化，仅"插件化"那半边）、ADR-0011（能力槽取代插件类型）、ADR-0012（能力是函数，不建注册表）、ADR-0013（插件体系抽出为 realugin）、ADR-0014（realugin 只留机制）、ADR-0015（一个容器一个 Plugin）

## 背景

到 2026-08-25 为止，插件体系服务过的用户数是 **0**。

线上共 5 个容器（`core-tools` / `v1-messages` / `deepseek` / `perm-allow-all` / `perm-ask`），
全部由本项目自己编写、与 core 同源发布、同时编译、同时安装。**没有第三方容器，
也没有任何人在两次发布之间单独换掉过其中一个。**

为了跨越一条从没有人跨越过的边界，我们付了这些税：

| 税目 | 代价 |
| --- | --- |
| C ABI | `core_tools.c` 里手写了 70 行 JSON 字符串提取 + 30 行 JSON 转义——同一个进程里 core 正链着 Boost.json |
| 借阅 / 转移 | 每个跨界 `const char*` 都要 `std::free(const_cast<char*>(...))`；`own()` 存在的唯一理由是把两种分配来源统一成一种 |
| 命名空间前缀 | 工具对外叫 `core-tools_bash`、对内叫 `bash`，于是权限传对外名、execute 传本名，`executor.cpp` 里专门有一段注释解释这不矛盾 |
| 能力槽解析 | `resolve_slots` 130 行，回答"管线四段各由谁来干"——五年内答案都会是同一个 |
| 撞名检查 | 每 init 完一个容器全量拉一次工具/命令清单，`O(n²)` 比对，防的是不存在的第三方 |
| 异常不得穿越 ABI | SSE 解析要在边界上把异常转成状态码，否则 `terminate` 整个常驻服务 |
| 外部仓库 | realugin（加载器）+ `realagent::sdk`（能力词汇表）+ `plugin.json` 规范 + 两个仓库的 CMake 找包 |

这些代价都是真的，换来的可扩展性是假的——没有人扩展过。

## 决策

**删除插件机制，五个容器的实现直接编进 core。**

- `v1-messages` + `deepseek` → `core/src/llm/llm.cpp`：`build_request` / `SseParser` / `Pricing`
- `core-tools` → `core/src/tools/tools.cpp`：`read` / `edit` / `bash`（C 重写为 C++）
- `perm-allow-all` + `perm-ask` → 配置键 `permission`（`ask` / `allow-all` / `deny`）+ `Executor` 里一个 `switch`

同时删除：realugin 依赖、`core/sdk/realagent/agent_caps.h`、`core/src/extension/`、
`GET /plugins`、`POST /plugins/enable|disable`、`/plugins` 与 `/provider` 斜杠命令、
配置项 `plugins.disabled` / `plugins.unprefixed`、TUI 的插件面板与渲染。

## 三处"多态"分别怎么落地

**协议 vs 供应商**：这条边界本身是真的（`/v1/messages` 是公共协议，端点与凭证是供应商的），
但它不需要两个动态库来维持。协议层照旧不认识任何供应商——它读 `cfg.base_url` 与
`cfg.api_key`，换一家就是改配置里的一行。"供应商身份"这个概念就此消失：DeepSeek 从一个
容器退化成默认配置值 + 模型数据表里的两行。

**默认值可以是真的了**。从前 `config_defaults.hpp` 一律留空串，并且明写着"别拿假 URL 占位"——
因为整条兜底链路靠 `empty()` 判断"用户没配过"，填任何非空值都会让壳的兜底当场失效。
壳没了，那条暗协议随之作废，默认端点与默认模型现在就写在默认树里，装完即可用。

**本次用的是哪个模型**：从前这条信息 core 不传也不知道，要靠 provider 壳在 `request.refine`
时把它偷偷记在自己的成员变量里，计价时再读出来——两次调用之间藏着一个隐式的状态传递。
现在计价直接收模型名作参数（就是 `dialog["model"]`，调用方本来就知道）。
这不是"顺手简化"，这是那个特殊情况本来就不该存在。

**权限**：`perm-allow-all` 与 `perm-ask` 各是一个动态库，各自 55 行，其中 50 行是
ABI 样板，唯一的差别是 `return` 后面那个枚举值。它们从来不能同时存在——权限槽独占，
装了两个就要靠 `plugins.disabled` 禁掉一个，否则槽位空置、所有危险工具默认拒绝。
两个互斥的动态库表达的是一个三值配置项。

## 代价与保留

**丢掉的**：第三方无需重编 core 即可扩展的能力。想清楚了：需要它的那天，
git 历史里有一份完整实现，realugin 也还在自己的仓库里。**用不上的抽象没有折旧价值，
提前留着它的成本却是每天都在付的。**

**保留的**：`docs/adr/0009`（模型数据表与计价）与 `0002`/`0005` 的实质结论仍然成立——
它们讲的是"钱怎么算"和"审批链路谁发起"，与那些东西住在动态库还是 `.cpp` 里无关。
被取代的只是"以插件形式实现"这半句。

## 迁移

用户的 `~/.realagent/settings.json`：

| 旧 | 新 |
| --- | --- |
| `plugins.disabled: ["perm-ask"]` | `permission: "allow-all"` |
| `plugins.disabled: ["perm-allow-all"]` | `permission: "ask"` |
| `plugins.unprefixed` | 删除（工具本来就没有前缀了） |
| `provider: "deepseek"` | 删除（`base_url` 就是它） |
| `~/.realagent/models/deepseek.json` | `~/.realagent/models.json` |

未迁移的旧键留在文件里不会报错——配置合并只认默认树里有的键，多余的键读不到也不碍事。
`~/.realagent/extensions/` 整个目录可以删掉。
