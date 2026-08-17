# ADR-0013：插件体系抽出为 realugin，core 只留宿主词汇

- 状态：已采纳（2026-08-16）
- 相关：ADR-0001（C ABI 插件）、ADR-0011（能力槽）、ADR-0012（能力是一个函数；管线取代嵌套）

> ⚠️ **本文的"机制/词汇"分界已随 ADR-0014 收紧（2026-08-16）。** 下表里「`PLUGIN_CAP_*` 留在 `plugin_api.h` 当默认词汇表」那一条已废止：能力键与签名整体搬到 realagent 的 `core/sdk/realagent/agent_caps.h`，realugin 一个业务词汇都不留；工具/命令视图与撞名检查也从加载器搬回 core（`Host::validate`），事件扇出的键名改由 `Host::broadcast_capability()` 报上去。C ABI 标识符加 `realugin_` 前缀（含入口符号），ABI 4 → 5。其余决策不变。

## 背景

到 ADR-0012 为止，插件体系已经稳定成两块互不相干的东西，却住在同一个文件里：

- **机制**：容器怎么被发现、`dlopen`、ABI 强校验、能力表读取、能力索引、`deps` DAG 拓扑 init、启停级联、`core_api` 七件套。这套代码不认识 LLM、不认识 provider，换个宿主照样能用。
- **词汇**：`request.build` / `request.refine` / `response.parse` / `usage.meter` 这条管线归谁干、`provider` 配置怎么读、`protocol` 这个键什么意思。这套只有 realagent 认得。

两者混在 `core/src/extension/loader.cpp`（683 行）里，唯一的胶水是 `Config`——加载器为了读扫描目录、禁用清单、免前缀名单、`models_path`，直接持有 `Config*`。另一边 `realagent-plugins` 靠相对路径 `../realagent/core/sdk` 找 SDK 头：两个仓库必须并排摆放才编得过。

## 决策

**插件体系整体抽出为独立仓库 [realugin](https://github.com/FlowerRealm/realugin)，realagent 经 `find_package(realugin)` 导入。**

分界线是"这句话换个宿主还成不成立"：

| | 去向 |
| --- | --- |
| `plugin_api.h`（C ABI，ABI 4 原样不动） | `realugin::sdk` |
| 发现 / dlopen / ABI 校验 / 能力索引 / DAG init / 启停级联 / `core_api` | `realugin::loader` |
| 工具与命令视图（加前缀、撞名检查） | `realugin::loader` |
| 事件扇出（`event.observe` + 重入守卫） | `realugin::loader` |
| 管线四段与权限槽的解析、`current_provider`、`models_json` | core（`extension/slots.cpp`） |
| 配置、禁用清单、免前缀名单、`models_path` | core（`CoreHost`） |

加载器不再认识 `Config`，改问 `realugin::Host` 的十个虚函数。其中八个带默认实现——最小宿主只需实现 `extension_dirs` 与 `get_config`。core 的实现是 `CoreHost`（`core/include/extension/slots.hpp`）。

**加载器不解析任何"槽位"。** 谁来干哪段活是宿主的词汇：容器集合变化时（`load_all` / `enable` / `disable` 收尾）加载器回调 `Host::on_reload`，宿主在那里用 `providers_of` / `find` / `cap_of` 自己钉住要的函数。core 的 `resolve_slots()` 就挂在这个回调上。

**`PLUGIN_CAP_*` 那组键留在 `plugin_api.h` 里，但降格为"默认词汇表"。** 它们不是机制的一部分：加载器全程不解释任何能力键，只在 `Host::knows_capability` 说不认识时点一句名（防拼错）。

## 顺带消掉的特殊情况

`protocol` 曾是 `Plugin` 的一个字段，只为 Provider 壳存在，并在两处特殊照顾：发现时手工追加一条 deps 边，`api_import` 的暗边检查写成 `self->protocol == container || deps 里有`。

现在 `plugin.json` 里除五个必需键之外的顶层字符串键一律收进 `Plugin::extra`，加载器不解释；宿主经 `Host::extra_deps` 声明其中哪些也算依赖边。core 返回 `{extra["protocol"]}`，于是 `protocol` 从头到尾就是一条普通的 deps 边——`api_import` 的那半行条件不存在了，图上依然没有暗边。

## 后果

- **插件不受影响**：ABI 仍是 4，`plugin_api.h` 内容逐字未改。唯一变动是包含路径 `<plugin_api.h>` → `<realugin/plugin_api.h>`，以及 CMake 改用随 realugin 一起安装的 `realugin_add_plugin()`——两个仓库不必再并排摆放。
- **事件出口收拢**：原先"入队 + 扇出"这条规矩写在 `main.cpp` 的 lambda 里，宿主写错就会漏掉一路。现在两路都在 `PluginManager::emit` 内，`Host::emit` 只管送客户端。
- **多一个安装步骤**：realugin 需先 `cmake --install` 到前缀（默认 `~/.local`，realagent 的顶层 CMakeLists 已把它加进 `CMAKE_PREFIX_PATH`）。选本地 `find_package` 而非 submodule/FetchContent：库语义最干净，代价是改 ABI 时要重装一次。
- realugin 引入 Boost.JSON（解析 `plugin.json`）。**仅宿主侧**——`realugin::sdk` 是纯 C 头，插件侧依然零依赖。
