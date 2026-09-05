# ADR-0023：加入 MCP —— 外部进程只声明接口，不注入代码

- 状态：已采纳（2026-08-30）
- 依赖：ADR-0016（插件体系废除，工具并入 core）、ADR-0017（协议按方向三分家）、ADR-0018（不为纯文本另开工具）、ADR-0019（agent 必传 workdir）、ADR-0021（组随客户端生灭）、ADR-0010（启动读一次，不热重载）、ADR-0022（两处来源，同名近的覆盖远的）
- 不取代任何 ADR

## 背景

ADR-0016 在 2026-08-25 铲平了插件体系。五天后加 MCP，第一件要回答的事仍旧是那句：
**这不就是插件换了个名字吗。**

不是。逐条对 ADR-0016 那张税目表：

| ADR-0016 列的税目 | 插件 | MCP |
| --- | --- | --- |
| C ABI | 手写 70 行 JSON 提取 + 30 行转义 | 无。两边各用各的 JSON 库，中间是一行行的字节 |
| 借阅 / 转移 | 每个跨界 `const char*` 都要管所有权 | 无跨界指针。进程隔离 |
| 异常不得穿越 ABI | 边界上转状态码，否则 `terminate` 整个常驻服务 | 无边界。server 崩了 core 不动一根汗毛 |
| 能力槽解析 | 130 行回答「管线四段谁来干」 | 管线一段不换。MCP 只填一个洞：工具 |
| 外部仓库 | 加载器 + SDK + 规范 + 两套 CMake | 协议是公开的，server 是别人写的 |
| 撞名检查 | `O(n²)` 防不存在的第三方 | 前缀取配置里的键，JSON 对象的键天然唯一 |

**判据只有一条：core 里有没有多出一行不是自己写的代码。**
插件是 core `dlopen` 别人的 `.so` 然后调它的函数；MCP 是 core 拿到一份别人给的 JSON，
照着它往一条管道里转发。**这一点在数据结构上直接看得出来：整张工具表 `dump()` 得出来。**
函数指针 dump 不出来，JSON 可以。

而 ADR-0016 那条决定性的理由——「插件体系服务过的用户数是 0，五个容器全是本项目自己写的、
与 core 同源发布」——在这里不成立：MCP server 现在就有几百个，`npx` 一条命令装，
**一个都不是本项目写的**。那条从没有人跨越过的边界，这次有人在跨。

## 决策

### 1. MCP 工具就是 Tool，不另立名词

模型收到的清单里 `read` 和 `fs__read_file` 完全同形：一份清单、同样的 Schema、同样的 `tool_use`，
挑一个调，没有第二种调法。给它们两个名词，是把 core 的实现分区泄漏进领域语言。

差别落在字段上：一个 Tool 知道自己归谁执行。`Executor::execute` 因此只多**一个**分支——
判的是「代码在 core 里还是在别的进程里」，一个真区别，不是补丁。
**N 个 MCP 工具共用同一个实现（转发），这正说明它们不是 N 段代码，是 N 份声明。**

### 2. 工具表与启动规格都是 JSON，不是结构体

`ToolDef` 结构体删除。表是一个 `nlohmann::json` 数组，每个元素就是端点要的那个工具对象，
外加一个 `_core` 键装 core 私有的字段（`label` / `dangerous` / `server` / `remote_name`）。
`build_dialog` 从「循环 + 逐字段搬运 + `parse(parameters)`」变成「拷一份，`erase("_core")`」。

理由不是省代码，是那两次转换**互相抵消**：MCP 递来 JSON → 拼进结构体 → 再拼回 JSON。
而 `parameters` 本来就是一个 JSON 字符串，编译器一天都没检查过它——
所谓「结构体的类型安全」在这个字段上是假的，它只是把一次 parse 藏在了每次 `build_dialog` 里。

**配置同理，`McpServerCfg` 也删掉。** 一条启动规格就是一份归一过的 JSON 对象
（`{"name", "command", "args", "env"}`），归一只在 `parse_entry` 里做一次。理由是同一条：
MCP 的配置信封是事实标准，写的人照着别家的例子粘过来；缩进结构体等于把同一份数据
再抄一遍字段名，而 core 对这四个键做的事只有两件——拿去 `fork/exec`、`dump()` 出来当连接的键。
两件都不需要静态类型，`execvp` 要的本来就是 `char*`。

顺带把 `identity()` 那八行也删了：**那份对象自己就是键**，`dump()` 一下就完。
少一个手写的序列化函数，也就少一个漏掉字段的机会——比如哪天多一个 `cwd`，
忘了加进 `identity()` 的话，两份不同的配置会安静地共用同一个进程。

**未知键在归一处落下，这不只是「读过即弃」**：留着一个被忽略的 `timeout`，
同一个 server 会因为它起两个进程。

`enabled` 也不再是规格上的一个字段：`{"enabled": false}` 让 `parse_entry` 返回 `nullopt` 且 `err` 留空，
调用方照样 `erase`。关掉和坏掉的动作本来就是同一个——从表里抹掉——于是它既不必是一条能跑的规格，
下游也不需要再过滤一次。

**类型上的判断交给 json 库，不自己拼。** 既然规格就是 JSON，检查它的活也该是库的：
`at()` 找不到键抛 `key 'command' not found`，`get<std::string>()` 抛
`type must be string, but is number`，`parse()` 抛的那句带行号列号和「本来期待什么」。
一句都不用维护，而且比手写的准——手写的说不出「是 `args` 的第 0 个元素」。

而且**确实省代码**：`hub.cpp` 的代码行 173 → 159。手写的三条 `is_string()` 检查、
`has_template()` / `template_hit()` 那一对辅助函数，全部并进一个 `str()` lambda——
每个进规格的字符串都过那一道，类型由库判、`${...}` 由我们判，一次走完。
**失败因此只有一个出口**：业务上的错也 `throw`，跟库抛的走同一个 `catch`，
四条 `err = ...; return nullopt;` 收成一条。

顺带办了三件手写判断办不到的事：

| | 手写 `is_string()` / `find()` | 交给库 |
| --- | --- | --- |
| `"args": [1, 2]` | 被 `is_string()` **悄悄丢掉**，用户拿到一个不带参数的 server，一个字提示都没有 | 当场说清哪一条坏在哪 |
| `{ this is not json` | 「不是合法 JSON」，用户自己去数括号 | `parse error at line 1, column 4 ... expected string literal` |
| `{"enabled": "yes"}` | `value()` **抛穿整个 core**——那条路上一个 `catch` 都没有，一份手滑的配置带走所有 agent | 接住，记一条，core 照常起 |

第三条是原来就在的 bug，不是这次改出来的。`parse_entry` 因此自己接住所有异常：
它对外的契约是 `optional` + `err` 一种，调用方不必知道它还会抛。

叫 `_core` 不叫 `_meta`：`_meta` 是 MCP 自己的保留字段，**server 递过来的工具对象里就可能带着它**。
另开一个键，是为了不把两家的数据搅在一起。

MCP 的 `title` / `annotations` / `icons` / `outputSchema` / `execution` 一律不收——端点的工具定义里
没有它们的位置。**core 从 server 手里取三样（`name` / `description` / `inputSchema`），其余自己写。
外部遵守协议，内部 core 知道该写什么。**

### 3. 工具结果是一个块数组

`{"content": [...], "isError": bool}`，就是 MCP 定的那个形状。内置六个交出来的是单个 `text` 块。

这不是迁就 MCP：一个工具本来就不一定只有一段文字好说（读一张图、截一张屏），
内置的那几个迟早也要用到它。用同一个形状换来的是——**丢东西的地方变成了知道自己在丢什么的那一层**。
能不能带图片是端点协议的事：`anthropic-messages` 的 `tool_result.content` 接受
`text` / `image` / `document` / `search_result` 块数组，图片原样传得过去；
两套 OpenAI 协议的工具消息只收字符串，在 `llm/upstream/<协议>.cpp` 里压平成占位。
**同一段代码不该在 MCP 那道边界上替端点做决定。**

`structuredContent` 忽略——规范自己写着它 SHOULD 同时出现在 text 块里，收两遍就是同一份数据进两次上下文。

`status` 那个 int 一并删掉：它唯一的非 0/1 值来自 `bash` 的退出码，
而它的两个消费者（`is_error` 与 TUI）都只判 `!= 0`。`tool_execution_end` 帧照旧带 `status`，
值取 `isError ? 1 : 0`——协议契约不动。

### 4. 连接是进程级的；配置两处来源，一个 schema

**不按 workdir 分家。** 这不是省进程，是协议本身的形状：

- `roots` 于 `2026-07-28` 废弃（SEP-2577），理由是「语义模糊、与 tool 参数和 server 配置重叠」，
  迁移路径是「把目录放进 tool 参数、资源 URI 或 server 配置」——**工作区身份被整个移出了连接**
- 规范写死「一个 stdio 进程不是一次会话」，且「工具集**不得**随连接而变」

按目录复制连接，既拿不到不同的工具，也没有任何协议渠道能告诉对方它在哪个目录。

**配置两处来源，同名近的覆盖远的**（ADR-0022 §2 那条判据原样搬过来）：

```
~/.realagent/mcp.json              跟着人走
<workdir>/.realagent/mcp.json      跟着仓库走，可进版本库
```

**两处是同一个 schema、同一个解析器、同一条合并规则**——信封逐字照抄事实标准
（`mcpServers` → 名字 → `command` / `args` / `env`），外加一个 `enabled`（缺省 true，两处都认）：

```json
{ "mcpServers": {
    "fs":     { "command": "npx", "args": ["-y","@modelcontextprotocol/server-filesystem","/Users/realm"] },
    "github": { "enabled": false }
} }
```

**同名整条覆盖，不逐字段合并。** 理由跟 ADR-0017 拒绝把端点束拆成四个开关是同一条：
一条启动规格是一束共变的东西（命令、参数、环境），逐字段合并能拼出一个谁都没写过的启动命令。
项目级那三种写法——带 `command` 是覆盖、只有 `enabled:false` 是关掉、全局没有的是新增——
一个形状表达三件事，不需要第二种语法。

**连接的键是那份配置本身**，不只是名字：两个仓库各自定义了 `fs` 且参数不同，那就是两个进程；
参数完全一样，共用一个。这句话字面成立——键就是那份归一过的 JSON `dump()` 出来的串（见 §2）。

**不做变量展开。** Claude Code / Cursor / VS Code 各有一套（`${CLAUDE_PROJECT_DIR}` /
`${workspaceFolder}` / `${input:...}` / `${VAR:-默认}`），互不兼容。它们需要它，是因为那些
配置文件要进版本库、密钥不能写死；而更硬的一条是 ADR-0010 的原话——**「没有 env 那一层」**。
支持它就是把那一层从侧门请回来。

**未知键一律忽略，不报错**（`timeout` / `cwd` / `enabled_tools` / `headers` / `oauth` 全部读过即弃）。
先例是配置合并「只认默认树里有的键，多余的键读不到也不碍事」。副产品：JSON 没有注释，
而「未知键忽略」白送了一个（`"//": "..."`）。

**`type` 要认**，非 `stdio` 跳过并说清是「本项目不支持」，不是「你配错了」——
不认它的话，一份 `type: "http"` 的配置会让 core 拿一个不存在的 `command` 去 fork。

**生死跟着组**（ADR-0021）：第一个组建起来时同步连上、`tools/list`、清单当场定死；
最后一个组关掉时断开。组才是所有权边界。

**落地时的实况**：组（ADR-0021）已采纳但**还没建**——`on_group_close` 是个空壳，
`Agents::create` 不带 `client_id`。所以今天持有那份连接的是 **agent**，不是组。
不假造一个组：换成组只是换谁拿着那个 Lease。计数本来就不用自己写——池里存 `weak_ptr`，
持有者拿 `shared_ptr`，最后一个松手连接自己关，谁持有都是同一套机制。

清单**不在一趟中间重拉**。规范给了 `ttlMs` / `cacheScope`，也给了 `subscriptions/listen` 收
`notifications/tools/list_changed`——两个都不用，理由是 prompt cache：清单变形等于每轮重新缓存。
规范自己要求 server 按确定顺序返回工具，写的理由就是 *"improves LLM prompt cache hit rates"*。

### 5. 只说 `2026-07-28`，没有握手

**不做旧纪元。** `2025-11-25` 及更早那套 `initialize` 握手，core 一行都不写。

于是**连接只有两步**：

```
1. fork/exec 起进程
2. 直接发 tools/list
      ├─ result（resultType: "complete"）  → 清单到手，连上了
      ├─ error -32022                      → 它不说这个版本，data.supported 进日志，跳过
      └─ 超时 / stdout 关了                 → 跳过，原话进 stderr
```

**连 `server/discover` 都不发。** 规范：*"Clients **MAY** call it before sending any other
requests, but are not required to: a client is free to invoke any RPC inline and handle
`UnsupportedProtocolVersionError` if its preferred version is not supported."*
我们只要工具清单，而版本不合的信号会长在我们本来就要发的那个请求上——
多发一个 `server/discover` 是多一次往返、多一条代码路径，换不到任何我们要用的信息。

**每个请求带 `_meta`**：

```json
"_meta": {
  "io.modelcontextprotocol/protocolVersion": "2026-07-28",
  "io.modelcontextprotocol/clientCapabilities": {},
  "io.modelcontextprotocol/clientInfo": { "name": "realagent", "version": "..." }
}
```

那个**空的 `clientCapabilities` 就是全部的能力故事**。规范规定 server **MUST NOT** 发客户端
没声明支持的 `inputRequests`，需要而没有的会回 `-32021 MissingRequiredClientCapability`。
于是 MRTR（`InputRequiredResult` + `requestState` 重发）那一整条反向链路永远不会触发——
**不是「跳过了一个功能」，是那条代码路径根本不存在，而且不存在是协议保证的、错误是有定义的。**
`tasks` 同理：不声明，server 就不能回 `resultType: "task"`，长跑工具老老实实阻塞到超时。

`resultType` **只认 `"complete"`**。`"input_required"` 与 `"task"` 出不来（见上），
其余一律非法——规范原话 *"A `resultType` of any value unrecognized by the client MUST be
considered invalid."*

**无状态换回来的一条：崩了直接重启。** 规范：*"If the server process exits unexpectedly,
the client SHOULD restart it. Because the protocol is stateless, any in-flight requests are
simply lost and the client can retry them against the fresh process."*
旧纪元做不到这件事——重启就丢了那次 `initialize` 建立的会话，客户端还不知道对面记了什么。
这是 modern-only 的正收益，不只是「少写代码」。

### 6. 名字：非法字符换掉，长度不管

`name` = `<mcp.json 的键>__<server 那头的原名>`，**非法字符换成 `_`，不截断**。
前缀取配置的键，不取 `serverInfo.name`——规范明说那个不保证跨 server 唯一。
Anthropic 的文档也正好推荐这种命名：*"prefix names with the service (for example, `github_list_prs`)"*。

端点对工具名有字符集与长度约束（`anthropic-messages` 是 `^[a-zA-Z0-9_-]{1,64}$`），
撞上了就让请求爆。`llm.cpp` 的 `http_status_error` 已经把端点的错误原文捞出来送进
`turn_end`——**端点自己会指名道姓说哪个名字不合法，比 core 编一句更准，而且不用维护**。
而前缀那一半正是用户自己写的键，他改得动。

**替换那一行是抄的**，抄的是端点第一方客户端：Claude Code 的二进制里编着
`/^[a-zA-Z0-9_-]{1,64}$/`，配着 `_.replace(/[^a-zA-Z0-9_-]/g, "_")`；
codex 的 `sanitize_responses_api_tool_name` 与 opencode 的 `sanitize` 是同一个字符类。
三个独立实现、同一条规则，其中一个是端点自家的客户端。

**这一条是被证据推翻后改的。** 起草时写的是「不 sanitize，端点不收就让它爆」，
理由是「举不出一个真这么命名的 server，属于臆想出来的问题」。举不出是因为没去找：
官方 **Go SDK 自己的 `examples/server/everything`，10 个工具里 5 个带空格和括号**
（`greet (with Icons)`、`elicit (form)`）。MCP 规范自己也写着
*"Tool names SHOULD NOT contain spaces"*——而写规范那伙人的示例就带着。

**字符换、长度不换**，因为判据不同：**受害者有没有方向盘**。
长度那一半是用户写的配置键，撞上了他改一个词就好，端点的报错还会指名道姓；
字符是 server 起的名字，他一个字都改不动，而一个这样的 server 能让**每一次请求**都 400。

仍旧不学 codex 那套 **截断 + sha1**：哈希出来的名字会随算法或长度而变，
而这个名字要永久写进会话记录。纯字符替换是确定性的——同一个名字每次跑出来都一样。

两个名字仍旧都留，但**不是为了 sanitize**：前缀本来就得加（两个 server 可以各有一个 `search`），
加了就跟原名不同，转发时要用回原名。两个字段并排住在同一个对象里——
ADR-0016 记的那笔账是名字的两半**分居两个模块**（权限传一个、执行传另一个，还要写段注释解释这不矛盾），
不是这个。

**别家的变量认出来就点名。** 官方 filesystem server 的 README 三个变体全带 `${workspaceFolder}`，
粘过来会原样传给 server 然后失败。不展开，但要把话说全：

```
[mcp] fs: args 里有 ${workspaceFolder}，那是 VS Code 的变量，本项目没有对应物
      （连接是进程级的，不属于任何目录）。那个参数是该 server 被允许触碰的范围，
      请改成绝对路径——写死它就是明确授一次权。跳过这个 server
```

**那个位置参数不是工作目录，是访问边界**（docker 那版更直白，它是 `--mount` 的 `src=`）。
一个进程级共享的 server，它的可及范围是一个全局问题；让它跟着「哪个 agent 恰好在调用」自动变，
等于让访问边界随调用方漂移。**要用户写死它不是委屈他，是让他明确地授一次权。**

### 7. MCP 工具一律带危险标记

不看 `annotations.readOnlyHint`。规范原话：*"clients **MUST** consider tool annotations to be
untrusted unless they come from trusted servers."* 一个第三方进程自称无害，不构成一次权限裁决。
同 `decide()` 里那句「认不出的值按 ask：配置写错时该多问一句，不该多放一次行」。

实测佐证：官方参考 server 的 `gzip-file-as-resource`（一个会去网上抓文件的工具）
自报 `readOnlyHint: true`。

**不做按 server 的工具白名单 / 黑名单**（codex 有 `enabled_tools` / `disabled_tools`）。
它解决的问题是「每次都问烦」，而现在这个问题的用户数是 0。真被问烦的那天才知道
他想要的是按工具、按 server、还是按参数——提前造，很可能造错。
何况那份手写名单是拿来比对一份对面随时可以改的清单，会安静地失配。

### 8. 坏 server 跳过，报错原文进 stderr

连不上、版本不对、握手超时的不进清单，core 照常起。挑的是 `models.json` 那条先例
（报错但不拒绝启动），不是 `settings.json` 那条（硬错退出）：为一个可选的外部进程拒绝建 agent，
是把次要功能提成必需品。

**用户当下看不见这件事**——core 是常驻服务，面前只有 TUI。这是一笔认下的债，
skill 今天有同一个窟窿（`[skill] ... 跳过` 也只进 stderr），一并另算。

### 9. 中断走协议，不杀进程

stdio 没有可关的流，规范要求发 `notifications/cancelled` 引用那个请求 id，
然后**无视迟到的响应**。**不杀 server 进程**——连接是进程级的，杀了会把别的 agent 一起弄断。

读循环**不需要** `poll()` 加自管道（起草时以为要）：它是一条专职线程，唯一要醒来的时机是
进程没了，而那时 stdout 到 EOF，它自己会退。中止不是叫醒读线程，是让**等的那一方**放弃。

外加一个硬超时。这个数是拍的，先取 300 秒，不做成配置项——
三家都把它做成了每 server 的旋钮，但那是给他们的用户用的，我们的用户数是 0。
配置里带着 `timeout` 的照样能粘进来，只是那个键被当未知键忽略。

**并发是必然的，不是选型。** 连接进程级共享 ⇒ 两个 agent 会同时调同一个 server；
规范也明说允许（*"clients may interleave unrelated requests on the same transport"*）。
于是三件事被逼出来：一个连接一条读线程、按 JSON-RPC `id` 认领、调用方阻塞在条件变量上。

实测的教训：**响应和通知共用同一条 stdout。** 第一版探针「写一行读一行」，
`tools/list` 读回来的是一条 `notifications/tools/list_changed`。
**「有东西回来了」不等于「我的问题被回答了」——只看 `id` 对不对得上。**

## 不做的

`resources`、`prompts`、`sampling`、`elicitation`、`tasks`、`roots`、
`progressToken`、`subscriptions/listen`、`structuredContent`、`server/discover`、
旧纪元（`initialize` 握手）、Streamable HTTP 传输、OAuth、变量展开、
skill 携带自己的 MCP server（ADR-0022 已拒，不翻案）。

其中四个是「不声明能力」的直接后果，不是额外的克制。`resources` 与 `prompts` 是主动拒的：
`read` 已经读文件（ADR-0018 拒过第七个工具的理由一字未改），
斜杠命令已经有一份实现（`command.cpp`），不给它开第二个来源。

`progressToken` 也不发——长跑的答案是超时，不是把超时钟交给对面去续；
规范自己也说 *"SHOULD always enforce a maximum timeout, regardless of progress notifications"*。

## 为什么不用现成的 C++ 库

**官方没有 C/C++ SDK**（Tier 1 是 TypeScript / Python / C# / Go / Rust）。第三方四个，全部 clone 实测：

| | ★ | 许可 | C++ | 协议版本（源码里数出来的） | 骨架行数 | 依赖 |
| --- | --- | --- | --- | --- | --- | --- |
| hkr04/cpp-mcp | 318 | MIT | 17 | `2025-03-26`×17、`2024-11-05`×12 | 41,333 | submodule |
| Neumann-Labs/mcp-cpp | 64 | **GPL-3.0** | 20 | `2025-11-25`×41 | 8,811 | nlohmann json |
| GopherSecurity/gopher-mcp | 145 | Apache-2.0 | 14 | `2025-06-18`×59、`2025-11-25`×25、**`2026-07-28`×18** | 123,835 | **OpenSSL + libevent** |
| caomengxuan666/cxxmcp | 20 | MIT | 17 | 版本串混乱（含 `2025-01-01`、`2026-05-24` 这类不存在的修订号） | 48,171 | conan + third_party |

`mcp-cpp` 技术上最合身（8.8k 行、C++20、同一个 `nlohmann::json`），卡在 GPL-3.0。
`gopher-mcp` 是唯一真做了现代纪元的，代价是 123,835 行外加两个 `find_package`。

算式：**core 现在 5,049 行**（不含 vendored 的 `json.hpp` / `fkYAML.hpp`），
而我们真正需要的是 **一个客户端的三个方法**——起进程、按 id 认领、
`tools/list` / `tools/call` / `notifications/cancelled`，JSON 库已经 vendored。约 350 行。

**为了省 350 行往一个 5,049 行的项目里塞 123,835 行**，正是 ADR-0016 那本账反着算一遍。
何况这四个库全是 server SDK 优先，而我们一个 server 都不需要建。

## 怎么测

**没有现成的对手可测。** 官方参考 server（`@modelcontextprotocol/server-everything` v2.0.0）
实测是旧纪元的——`server/discover` 回 `-32601`，只认 `initialize`。

`gopher-mcp` 的 **server 端**实现了 `2026-07-28`。**不链它，拿它当测试对手跑**：
一个 12MB 的框架作为 `find_package` 是灾难，作为 `tests/` 里 spawn 起来的一个二进制正好。

## 代价与保留

**能连上的 server 数不是 0，但也远不是全部。** 各 SDK 实测（源码里的常量，不是文档）：

| SDK | 最新协议版本 |
| --- | --- |
| **Go** | `latestProtocolVersion = protocolVersion20260728` —— **默认就是新纪元** |
| **Rust** | `V_2026_07_28`、`-32022`、`subscriptions/listen`、`_meta` 齐活 |
| TypeScript | `LATEST_PROTOCOL_VERSION = '2025-11-25'` —— 还没 |

**已经跑通过一次真对手**：编 Go SDK 自己的 `examples/server/everything`，
用本项目的 `McpClient` 连它——没有握手、`tools/list` 一次到位、`tools/call` 打通，
一行没改。那 10 个工具的名字也正是上面 §6 那条被推翻的证据来源。

但拿 TypeScript SDK 写的 server 仍旧是大多数，它们今天连不上。这个赌的理由：
realagent 用户数 0，「连不上」的当下成本也是 0；反过来，**为一个正在退场的纪元写代码，
代价是每天都在付的**——`roots` / `sampling` / `logging` 已废弃，MRTR 是明确的 breaking change。
TypeScript SDK 补上 `2026-07-28` 的那天，那批 server 跟着一次依赖升级就都过来了。
ADR-0016 那条判据反着用一次，答案是一样的。

**唯一一个没验的事实**：`inputSchema` 里的 `$schema`。实测官方参考 server **每个工具**
都带 `"$schema": "http://json-schema.org/draft-07/schema#"`，而我们把整个 schema 原样
塞进端点的 `input_schema`。Anthropic 的文档说 schema「MAY include a `$schema`，默认 2020-12」，
但**它收不收 draft-07 没有一手验证**（写这份 ADR 的机器上没有凭证）。

**现状是原样转发**，理由跟工具名那条一致：不为一个没确认的失败提前造兜底。
`test_mcp_hub` 里有一条断言钉住它，改主意时那条会指出改了什么。

真要证实，一次不计费的请求就够——`count_tokens` 校验请求形状但不跑模型：

```bash
curl -s https://api.anthropic.com/v1/messages/count_tokens \
  -H "x-api-key: $ANTHROPIC_API_KEY" -H "anthropic-version: 2023-06-01" \
  -H "content-type: application/json" -d '{
    "model":"claude-opus-5",
    "messages":[{"role":"user","content":"hi"}],
    "tools":[{"name":"t","description":"d","input_schema":{
      "$schema":"http://json-schema.org/draft-07/schema#",
      "type":"object","properties":{}}}]}'
```

收下 → 现状不动；`400` → `hub.cpp` 拼表那一处剥掉 `$schema`，一行。

**那条口子跟工具名那条不一样**，值得单记一笔：名字太长用户改得动（前缀是他写的键），
而 `$schema` 是 server 的，用户一个字都改不了。真被拒的话，**一个 server 就能让每一次请求 400**，
而端点报的错未必指得到它头上。这是「让他爆」在这里唯一一处理由不够硬的地方。

**分页要处理**（`tools/list` 的 `cursor` / `nextCursor`）。不处理的话，工具多的 server 只看得见第一页。

**清单撑爆请求**是真事，两家都撞上了（codex 给了 `MAX_AGENT_PLUGIN_MCP_SPEC_BYTES = 8_000`
和 `Hidden` 曝光档，opencode 干脆把 MCP 工具压进一个 `execute` 工具的描述里）。
**本项目不截断**，同 ADR-0022 对 system prompt 的作风：上限是用户自己给的，不想要就从
`mcp.json` 里删。日志里印一句「MCP: 3 servers, 47 tools」，让人看得见自己在付什么。

**第三方描述进 `dialog["tools"]`**，那是一个被缓存、被前置的位置。没有替代方案——
工具没有描述模型就不会用。它跟内置工具的描述同处一格，风险记下，不假装没有。
（server 写给模型看的那段 `instructions` 收不到：它在 `server/discover` 的结果里，而我们不发。）

**项目级 `mcp.json` 能带 `command`**，意味着 `git clone` + 开个 agent = 执行了那个仓库指定的命令，
而 fork 之前没有闸门。skill 没有这个问题——跑它的是模型手上的 `bash`，走 `dangerous` 检查。
这笔账认下，`enabled` 是将来加闸门时现成的位置。

**`spawn` / `send_message` 的定义与实现分居两个模块**（描述在 `tools/spawn.cpp`，
实现在 `agent/executor.cpp`，中间靠一句字符串比较缝上）——那是本来就在那儿的账，
跟 MCP 无关，**本 ADR 不碰它**。MCP 只往 `Executor::execute` 加一个分支。
