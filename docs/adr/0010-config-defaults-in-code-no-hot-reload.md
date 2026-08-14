# ADR-0010: 默认配置树进代码，配置文件降为覆盖层，取消热重载

- 状态：已接受
- 日期：2026-08-14
- 影响：ADR-0004（供应商默认值下沉到壳，本 ADR 补充 core 侧的键清单归属）

## 背景

原配置机制是「无内置默认、无回落」：`settings.json` 是唯一来源，必需键（api_key / base_url / model / small_model）缺一个 core 就退出。

这套设计有一个当时没看见的后果：**它让 Provider 壳里已经写好的兜底代码永远跑不到**。`deepseek.cpp` 的 `init` 里有

```cpp
if (p->base_url.empty()) p->base_url = "https://api.deepseek.com/anthropic";
if (p->model.empty())    p->model    = "deepseek-v4-flash";
```

加上 `refine_body` 填 model、`refine_headers` 补 Authorization，壳里有一整套「空即未配，我来兜」的默认层。但用户不配 `base_url`，core 在壳加载之前就已经退出了。ADR-0004 说「供应商默认值下沉到壳」，core 的必需键校验把这句话架空了。

同时，键清单本身没有唯一来源：`kRequired[]` 只列了 4 个必需键，`plugins.disabled` 这个键哪儿都没登记，只有 `loader.cpp` 里一句硬编码的 `to_json()["plugins"]["disabled"]` 知道它存在。开发者没有任何一处可以读到「一共有哪些配置项、各自什么作用」。

## 决策

1. **默认配置树写在代码里**（`config_defaults()`），它是键清单的唯一来源。每个键带注释说明作用与取值形状。`load()` 以默认树打底，再把 `settings.json` 合并上去——配置文件从「唯一来源」降为「覆盖层」。

2. **不做必需键**。删除 `kRequired[]`、`Config::required_keys()` 和启动时的缺键校验。配置缺失不再是错误状态，只是「取到默认值」。

3. **core 的默认值一律不带供应商身份**，值写空字符串，取值形状写在注释里。真实默认端点/模型名留在 Provider 壳（ADR-0004 不变）。**不用假 URL 占位**：整条链路（`deepseek.cpp` 的 `init` / `refine_body` / `refine_headers`）都以 `empty()` 判断「未配」，任何非空占位值都会让壳的兜底失效。空字符串不是占位符，它是这条链路的协议。

4. **core 不做档位间回落**。`small_model` 没配就是空串，原样经 `dialog["model"]` 下传，填什么由壳决定。壳只看见一个空模型名，分辨不出是哪一档——档位知识到 core 为止。

5. **`persist` 改为点对点**：读 `settings.json` → 只改目标键 → 原子写回。不再 dump 内存树。默认值因此永远不会被写进用户的配置文件，用户文件里只有他自己配过的东西。文件不存在按空对象处理；文件是坏 JSON 则不写、返回 false（宁可 `/model` 不生效，也不能拿内存树覆盖掉读不懂的用户数据）。

6. **取消运行时热重载**。删除 `reload_if_changed()` 及 `mtime_` 字段。启动时读一次，坏 JSON 报错退出（这条不变）。手改 `settings.json` 需要重启 core 才生效。

7. **statusline 帧改由主循环按载荷变化推送**。事件循环每轮比对 `statusline_payload(ctx).dump()` 与上次推送的值，不同才推。

8. **不引入环境变量层**。分层就两级：代码默认 < 配置文件。

## 候选方案与权衡

### 「这个键必须由用户配」怎么表示

1. **保留显式 required 清单**——两份真相（清单 + 默认树），正是要消灭的东西。
2. **空串 = 必配 / `json::null` = 必配**——都是在默认树里编码「必需」这个概念。走到一半发现前提错了：必需键校验本身就是 bug 的成因，不该保留它的任何变体。
3. **压根不做必需（选定）**——配置缺失不是错误，是取默认值。校验代码整段消失。

### 配置缺失的报错落在哪

不再有统一的报错点，落点随场景而异：装了 Provider 壳的用户，壳兜底后一切正常，只差 `api_key`（服务端返回 401，错误信息本身可读）；只装裸协议层的用户，空 `base_url` 拼出相对 URL，libcurl 报 `URL using bad/illegal format`。第二种报错质量差，但那是「不装壳直连」这条小众路径的固有代价，不值得为它把校验层请回来。

### 运行时改坏 settings.json 怎么办

1. **保留旧配置，只写 stderr（原实现）**——core 是常驻服务，TUI 是独立进程，stderr 输出在启动 core 的那个终端里。用户在 TUI 里改坏配置，屏幕上什么都不会发生，等于没人看见。
2. **保留旧配置 + 推一帧错误给客户端**——可见性有了，会话不死。但保留了热重载全部的机械复杂度。
3. **报错退出**——一致，但 core 退出即所有客户端断线，且没人负责重启；还有误杀风险（部分编辑器与 shell 重定向是「先清空再写」，事件循环有概率读到写了一半的文件）。
4. **取消热重载（选定）**——整个问题消失。没有运行时重读，就没有运行时坏文件。代价是手改配置要重启 core，对一个本地常驻进程来说可以接受。

### statusline 帧怎么触发

原实现是意外链路：`/model` 写文件 → `persist` 把 `mtime_` 置 0 → 下一轮 tick 重读整个文件 → `reload_if_changed` 返回 true → 推帧。`main.cpp` 那一处是全项目唯一推 statusline 的地方，删掉热重载会静默带走 `/model` 的状态栏更新。

1. **每个改配置的地方自己推帧**——两个调用点分散在 `main.cpp` 和 `loader.cpp`，将来加第三个必然有人忘。
2. **抽 `apply_config()` 入口 + `CoreContext::on_config_changed` 钩子**——忘不了，但要新增一个字段、一个函数，且 `plugins.disabled` 这种与状态栏无关的变更也会触发，得再判断 key。
3. **主循环比对载荷（选定）**——不需要入口函数，不需要钩子字段，`loader.cpp` 一行不动。信号也更准：原方案 `touch` 一下文件（内容没变）就会推帧，新方案只有载荷真的不同才推。`plugins.disabled` 不影响载荷，自然不推，无需任何 key 判断。开销可忽略（poll 超时 1000ms，`statusline_payload` 是一次 mutex + 一次 map 查找 + 一个小 JSON dump）。

### 合并配置文件的粒度

顶层键整个替换（原实现）在默认树引入嵌套默认后是定时炸弹：默认树里 `plugins` 下若有第二个键，用户只写 `plugins.disabled` 就会静默丢掉它，且不报错——值变成类型默认值。改为**递归合并对象，数组与标量整个替换**。数组不合并是刻意的：`disabled: ["deepseek"]` 的语义是「就禁这一个」，不是「在默认基础上再加一个」。

## 后果

- `Config` 类瘦身：去掉 `mtime_` 字段、`reload_if_changed()`、`required_keys()`、`mtime_of()`；`set()` 与 `persist()` 合并为点对点的 `persist(key, value)`（全项目仅两处调用，且一直成对出现）。
- **行为契约变更**：`PROTOCOL.md` 原先承诺「`/model` 切档与手改 settings.json 是同一条路」，现在只剩 `/model` 一条。手改配置需重启 core。
- 零配置可启动。装了 Provider 壳时，用户只需要配 `api_key` 就能跑起来——这是壳的兜底层第一次真正生效。
- 用户的 `settings.json` 保持精简：点对点写只碰目标键，默认值不会渗进去。代价是这个文件不再是「所有可配项的完整快照」，那份清单在代码的默认树里。
- `small_model` 未配时，实际行为是由壳填成它的默认模型（`refine_body` 看到空模型名就填 `p->model`）。这与原先「不存在小模型回落主模型这种隐式默认」的表述相反，CONTEXT.md 已相应改写：core 不回落，但下游会填。
