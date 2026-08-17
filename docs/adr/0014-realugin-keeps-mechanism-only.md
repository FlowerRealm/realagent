# ADR-0014：realugin 只留机制，能力词汇归 realagent；C ABI 加前缀，ABI 5

- 状态：已采纳（2026-08-16）
- 相关：ADR-0013（插件体系抽出为 realugin）、ADR-0012（能力是一个函数）、ADR-0001（C ABI 插件）

## 背景

ADR-0013 把插件体系抽成独立仓库时，承认了一处妥协：`PLUGIN_CAP_*` 那组键和
它们的签名仍留在 `plugin_api.h` 里，降格为"面向 LLM agent 宿主的默认词汇表"。

复查发现这个妥协没站住：

- 半个 SDK 头是 realagent 的话。`usage.meter` 的注释在讲"token 用量 → 钱（USD）"，
  `plugin_permission_t` 在讲工具危险与否，文件开头画着 LLM 管线的 ASCII 图。
  换个宿主，这些一句都不成立。
- **文档在撒谎**。`loader.hpp`、README、ADR-0013 三处都写着"加载器全程不解释任何能力键"，
  而 `PluginManager` 实际硬编码了三个：`emit()` 里的 `event.observe`、`tools()` 里的
  `tool.list`、`commands()` 里的 `command.list`，外加 `_` 与 `:` 两套前缀分隔符。
- `BOOST_DESCRIBE_STRUCT(PluginInfo, ...)` 也在库里，注释还写着"字段名与顺序即宿主对外的
  响应契约"——那是 realagent 的 `GET /plugins`，库却因此把 Boost.Describe 强加给每个宿主。
- C ABI 的公开标识符前缀是 `PLUGIN_` / `plugin_`。C 没有 namespace，前缀就是 namespace，
  而一个公开库占用 `PLUGIN_OK` / `PLUGIN_ERR` 这种名字等着撞车；宏尤其危险——
  预处理器没有作用域，撞了是静默替换，不像 typedef 撞了会当场编译报错。

## 决策

**1. realugin 只留机制，一个业务词汇都不留。**

`plugin_api.h` 现在只有：能力表、宿主→插件 API（emit / log / get_config / alloc /
release / providers / import）、`realugin_plugin_api_t` 五项、ABI 版本、导出符号。

realagent 的词汇搬进自己的 SDK 头 `core/sdk/realagent/agent_caps.h`：能力键、
管线四段与工具/命令/权限/模型的签名、`realagent_tool_t` / `realagent_command_t` /
`realagent_result_t` / `realagent_permission_t`。插件工程 `find_package(realagent)`
拿 `realagent::sdk`（它 INTERFACE 依赖 `realugin::sdk`）。

**2. loader 唯一会自己解释的键，键名由宿主给。**

扇出必须留在库里（ADR-0013 的理由不变：入队与扇出两路都在 `PluginManager::emit` 内，
宿主漏不掉一路），但键名不该由库定。于是新增 `Host::broadcast_capability()`：
返回键名则扇出，返回 `nullptr`（默认）则不扇。签名 `realugin_broadcast_fn` 是
`(plugin_t*, const char* type, const char* payload)`——顺手消掉了 `plugin_event_t`，
两个字符串不必包一层结构体。

**3. 具名条目视图与撞名检查搬到 core。**

`ToolView` / `CommandView` / `tools()` / `commands()` / `check_names()` 离开
`PluginManager`，成为 `extension/slots.cpp` 里的 `tools_of()` / `commands_of()`。
前缀分隔符（工具 `_`、命令 `:`）本来就是 core 的规矩：工具名要能进 LLM 的函数名位，
斜杠命令的习惯写法是 `<容器>:<命令>`。loader 仍算出 `Plugin::prefix`（它问过
`Host::unprefixed`），但怎么用不管。

撞名检查改由新增的 `Host::validate(p, mgr)` 承接：容器 init 完宿主还有什么要说的，
返回非空即判该容器失败并卸载。"条目叫什么、怎么算撞"是宿主的词汇。

**4. 响应契约回到 core。**

`realugin::PluginInfo` 变回朴素结构体，`status` 从字符串改为 `enum class Status`
（附 `to_string`）。`GET /plugins` 的字段名与顺序落在 `slots.cpp` 的 `PluginWire`
上——描述宏跟着契约走，不跟着加载器的内部快照走。realugin 因此不再依赖 Boost.Describe。

- 实况注（2026-08-16）：**`PluginInfo` 与 `PluginWire` 均已删除**（ADR-0015）。
  "响应契约归 core、realugin 不依赖 Boost.Describe"这一条不变，落点变了：
  契约现在就是 `plugins_json` 里那串 `boost::json::object` 字面量，不再有中间结构体。

**5. C ABI 公开标识符一律 `realugin_` / `REALUGIN_`，含入口符号。**

`plugin_create` → `realugin_plugin_create`。它是唯一真住在动态符号表里的名字，
最该带前缀。realagent 自己的词汇同理走 `realagent_` / `REALAGENT_`。

**6. ABI 4 → 5。** 结构布局没变，但入口符号改名、词汇头搬家，旧插件源码与二进制
都得重来。不 bump 的话旧 .dylib 会以"缺少导出符号"报错，不如直接把版本对上。

## 顺带修掉的

- **`unload()` 的 use-after-free**：签名是 `const std::string& name`，而调用点常传
  `p->name`——`plugins_.erase(it)` 销毁 Plugin 之后，函数后半段清能力索引时还在读它。
  改为按值传参。缺依赖那条路径原本必踩，现在有用例覆盖（ASAN 干净）。
- **`api_get_config` / `api_providers` 的 `static` 返回缓冲区**改 `thread_local`：
  插件本来就可以从自己的线程调进来，共用一个 static 就是数据竞争。
  "返回值到本线程下次调用为止"这条契约补进 `plugin_api.h`。
- **`plugin.json` 只解析一次**：原先 `discover()` 解析一遍（丢掉 `extra`），
  `open_plugin()` 再读一遍文件只为拿 `extra`——多一次 IO，还开了个两次之间文件被改、
  `deps` 与 `extra` 来自不同内容的缝。
- **`known_` 改 `std::deque`**：`discover` / `find_known` 交出的指针要在后续 `push_back`
  之后仍然有效，`vector` 扩容会把它们变成悬垂指针。
- **库不抢 stderr**：loader 自己的诊断改走 `Host::log`（此前它给插件备了 log 却从不自用）。
- **`load_all()` 拆成三段**（发现 / `open_pending` / `init_in_dependency_order`）。
- **静态库开 `POSITION_INDEPENDENT_CODE`**，宿主是共享库时才链得上。
- **realugin 补测试**：14 个用例覆盖拓扑 init、缺依赖、依赖环、暗边拒绝、`extra_deps`
  成边、`Host::validate` 判失败、init 失败连坐、禁用清单、级联停用与恢复、扇出与重入守卫、
  manifest 各种坏法。realugin 补 LICENSE（MIT）。
- **realagent 的 `protocol_plugin` 测试不再要求两个仓库并排**：容器目录改由 CMake 变量
  `REALAGENT_PLUGIN_DIR` 指定（默认仍是并排布局），运行时还可用同名环境变量覆盖。

## 补做：让别的仓库真的用得上（2026-08-16）

标准是两条：**新开一个与 agent 无关的仓库能直接调用，工具链齐全**；**在那个仓库里
管理插件很方便**。照这两条实测一遍，又挖出一批问题——全部靠跑代码验证，不是读文档
推断：

- **`find_package` 之后目标名是错的**。`EXPORT_NAME` 没设，导出的是
  `realugin::realugin_sdk` / `realugin::realugin_loader`，README 里写的
  `realugin::loader` 根本不存在。
- **`~` 不展开**。`extension_dirs()` 返回 `~/.myapp/extensions`，
  `std::filesystem::is_directory` 直接返回 false——零容器、零日志、零错误，
  最难查的那种失败。新增 `expand_user()`。
- **`realugin_install_plugin` 写死了 realagent 的目录布局**（`<src>/<name>/plugin.json`）。
  换个平铺布局的仓库，`cmake --install` 到最后一步才报 `file INSTALL cannot find ...`。
  改为按序猜两种常见布局，都不中就在 configure 期就 FATAL_ERROR，并支持
  `MANIFEST <path>` / `FILES ...`。
- **vendoring 模式拿不到建库助手**：`add_subdirectory(realugin)` 进来的工程里
  `realugin_add_plugin` 不存在（它只随 config 包安装）。顶层 `CMakeLists.txt`
  现在自己 `include(cmake/AddPlugin.cmake)`。
- **同名容器静默遮蔽**：两个目录各有一个同名容器时，后者悄悄覆盖前者，`failures=0`、
  零日志。改成 PATH 语义（先出现的目录赢）并 WARN 点名。
- **`extra` 丢键**：`plugin.json` 里 `"priority": 10` / `"tags": [...]` / `"cfg": {...}`
  全被静默丢弃。不把它们强转成字符串塞进 `extra`（否则 `"10"` 是串还是数说不清），
  改为把原文留在 `Manifest::raw` / `Plugin::raw`。
- **C++26 / CMake 3.30 是没来由的门槛**：代码里最高只用到 `std::expected`（C++23）。
  降到 C++23 + CMake 3.25，宿主工程的编译器选择宽得多。

同时补齐工具链：`add_builtin()`（编进宿主的容器与磁盘容器同权，同一张依赖图）、
`load_one()` / `unload_one()`（单个装卸，不动禁用清单）、Windows 的
`LoadLibrary` 分支、`examples/minimal-host`（一个渲染文字的宿主 + 两个插件，
其中一个 `import` 另一个，兼作"库真的通用吗"的回归）、GitHub Actions
（macOS + Linux × 普通构建 / ASAN+UBSAN / 装出去再从外面用一次）、
`CHANGELOG.md`、`.clang-format`、`realugin.pc`。

**主动卸载不再记成故障**：`unload_one` / `disable` 之后容器状态是 `Disabled` 而不是
`Failed`——`unload()` 原先把"用户点了停用"和"init 挂了"都走 `fail()`，清单里一片红。
顺带删掉了 `PluginManager::cascaded_`：只写不读的死状态。

加载器用例从 14 条加到 19 条（新增 `load_one`/`unload_one`、内建容器、`~` 展开、
同名遮蔽、`raw` 与非字符串 `extra`），ASAN + UBSAN 干净。

## 后果

- **插件要改两处**：`#include <plugin_api.h>` → `#include <realagent/agent_caps.h>`，
  以及全部标识符换前缀。五个容器已改完，`abi_version` 一并抬到 5。
- **多一个 find_package**：插件工程现在要 `find_package(realugin)` + `find_package(realagent)`。
  realagent 的 `cmake --install` 只装 SDK 头与 config 文件，不装 core 可执行文件。
- **realugin 可以给别人用了**。它现在真的只是一个插件库：C ABI + 加载器 + 依赖图，
  换个宿主一行不用改。
