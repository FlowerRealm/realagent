# ADR-0015：一个容器一个 `Plugin`，删掉 `PluginInfo` / `Manifest` / `PluginWire`

- 状态：已采纳（2026-08-16），**已被 ADR-0016 取代**（插件系统于 2026-08-25 废除，能力并入 core）
- 相关：ADR-0014（realugin 只留机制）、ADR-0013（插件体系抽出为 realugin）、ADR-0012（能力是一个函数，不建注册表）

## 背景

复查 realugin 的加载器时发现：同一个容器的身份在内存里躺着三份。

| | `name` | `description` | `version` | `deps` | `dir` | `extra` / `raw` | `builtin` |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `Manifest`（`manifests_` map） | ✓ | ✓ | ✓ | ✓ | | ✓ | |
| `PluginInfo`（`known_` deque） | ✓ | ✓ | ✓ | ✓ | ✓ | | ✓ |
| `Plugin`（`plugins_` vector） | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |

三份不是最糟的。最糟的是**拷贝方向是双向的**：`discover` 把 `Manifest` 抄进
`PluginInfo`，`open_plugin` 把 `PluginInfo` 抄进 `Plugin`，拿到能力表之后又把
`capabilities` 与 `builtin` 抄回 `PluginInfo`。数据从 A 抄到 B 抄到 C、C 再抄回 A，
就是"谁是真相"从来没定下来的症状。一致性全靠每个改动点都记得同步——当时确实同步对了，
但那是纪律撑住的，不是结构撑住的。

副产品也都是同一个病的疹子：`Host::extra_deps` 的合并逻辑写了两遍（磁盘容器一遍、
内建容器一遍）；`find` 与 `find_known` 两套查找；`known_` 必须是 `deque` 而不是
`vector`，只因为 `PluginInfo*` 交出去之后不能被扩容搞悬垂；`unload` 要一边改
`PluginInfo` 的状态、一边从 `plugins_` 里 erase，于是注释得专门交代"`name` 按值传，
否则 erase 之后引用就悬垂了"。

那 `PluginInfo` 是不是至少还有存在理由？原先有一条：`list()` 要返回值语义的快照，
因为 `Plugin` 会随卸载被销毁。但这条理由本身就是三份结构造出来的——只要节点不随
卸载消失，指针就一直有效。

## 决策

**一个容器一个 `Plugin` 节点，仅此一份。**

发现即建节点，直到下次全量重载才消失。还没打开的、打开失败的、被禁用的都在同一张表里，
用 `Plugin::status` 区分。**没在跑的容器不是另一种东西，是同一种东西的另一个状态**——
消除特殊情况优于为特殊情况加一个类型。

- `PluginInfo` 删除。身份、状态（`status` / `error` / `capabilities`）、运行时句柄
  （`dl_handle` / `api` / `instance` / `caps` / `prefix`）都是 `Plugin` 的字段。
- `Manifest` 整个删除，不是搬进 `.cpp` 藏起来——`parse_manifest` 现在直接把
  `plugin.json` 解出来的字段写进节点，中间不再摆一个结构体接一道手。`manifests_`
  map 随之消失。`Manifest::abi_version` 也一并没了：它解析过、存过、**从没被读过**，
  真正的 ABI 校验比对的是插件二进制报的 `realugin_plugin_api_t::abi_version`；
  manifest 里那个数留着只会多一份可能对不上的真相，现在只查"这个键在不在"。
- `known_` / `plugins_` / `builtins_` / `builtin_order_` 四个容器合成一个
  `std::vector<std::unique_ptr<Plugin>> all_`（发现序）。`unique_ptr` 是因为节点地址
  交出去过（`Cap::owner`、`open_pending` 的返回值），扩容或删元素都不能让它们悬垂。
- 「已打开的容器」不再单独存一份，`plugins()` 现算：`instance != nullptr` 即已打开。
  这与"不建注册表、现问现答"（ADR-0012）是同一条规矩，只是这次用在加载器自己身上。
- `unload` 不再从任何容器里 erase：`destroy` 把运行时那几项清零、状态改写，节点留在表里。

### 对 realagent 的影响

- `PluginManager::list()` 返回 `std::vector<const Plugin*>`；`plugins_json` 照着
  节点取字段。`GET /plugins` 的响应契约（字段名、顺序、值）一字未动，仍归 core
  （ADR-0014 决策 4），只是不再经过一层 wire 结构体——见下。
- **`PluginWire` 删除**（`slots.cpp`）。它是同一批字段的第四份：8 个成员照抄
  `Plugin`，`plugins_json` 逐个搬进去（含 `capabilities` / `deps` 两个 vector 的整份
  拷贝），`to_json` 一次，然后扔掉。它比前三份轻——活不过一次响应，不存在"两边对不上"
  ——但它买的东西并不存在：说法是"字段名与顺序即结构体声明，core 不手写逐字段搬运"，
  可搬运本来就是手写的（8 个位置参数，`status` 还要手工 `to_string`）。声明式那一半
  从没成立，剩下的只有一个类型和一趟拷贝。现在 `plugins_json` 直接拼
  `boost::json::object`：字段名、顺序、每个值从哪儿来，都在同一行。
  `main.cpp` 那个只转发一次的 `plugins_payload` 一并删掉。
- `PluginManager::plugins()` 返回 `std::vector<Plugin*>`（不再是内部容器的引用），
  `tools_of` / `commands_of` 的循环里 `p.get()` 变 `p`。
- `Host::extra_deps(const Manifest&)` 变 `Host::extra_deps(const Plugin&)`。
  `CoreHost` 改问 `p.meta("protocol")`——与 `get_config` / `validate` 一样，
  宿主回调一律拿到整个容器节点。
- `capabilities_of()` 不再公开：能力表借的是插件内存（[[借阅]]），`dlclose` 之后
  问不着，所以打开时拍一次快照存进 `Plugin::capabilities`。于是"这个容器提供什么"
  只有一个问法，而且停用的容器照样答得出来——那恰恰是用户要看的。

### 同一趟扫出来的其余重复

- **`ToolView` / `CommandView` 合成 `EntryView<Def>`**（`slots.hpp`）。两个结构体只差
  `def` 的类型，`tools_of` / `commands_of` 两个函数 32 行只差三个 token：能力键、
  条目类型、前缀分隔符。这是代码重复不是数据重复（视图本来就现问现答、不留副本），
  但那三处差异抬到调用点的三个实参上之后，"工具用 `_`、命令用 `:`"这条规矩才是一眼
  看得见的，而不是埋在两段几乎一样的循环里。
- `core/include/ai/` 空目录删除（早期骨架残留，CONTEXT.md 里已注明"不存在也不再规划"）。
- **没动的**：TUI 侧的 `client.PluginInfo`（Go）。它是跨进程跨语言的反序列化目标，
  与 `Plugin` 同名同形是因为两边说的是同一份 JSON，不是同一份真相的副本——
  wire 契约在进程边界上本来就该各自有一份。

### 顺带收紧的一处语义

内建容器的名字不再会被磁盘上的同名目录顶掉。原先 `known_` 里一个失败的内建条目
会被同名磁盘容器覆写（要等下一次 `load_all` 才从 `builtins_` map 里恢复）；现在
内建节点就是那一份真相，磁盘同名目录一律记 WARN 遮蔽。

## 后果

- realugin：`loader.hpp` 少一个公开结构体，`host.hpp` 少一个，`loader.cpp` 一个不新增；
  少掉三处字段互抄、一处重复的 `extra_deps` 合并、一套 `find_known`。
  ABI 未变（都是 C++ 层）。
- 破坏 realugin 的 C++ 源码兼容：`PluginInfo`、`Manifest`、`capabilities_of` 消失，
  `list()` / `plugins()` / `Host::extra_deps` 签名变。realugin 尚未发布（ABI 5 未发布），
  唯一的用户是 realagent，一次改完，记在 CHANGELOG 的破坏性小节。
- 测试：realugin 19 个用例、realagent 3 个用例全绿，未新增用例——这次改的是同一批
  行为的组织方式，不是行为本身。
