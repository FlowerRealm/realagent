# ADR-0020：TUI 进 altscreen，历史从会话记录动态渲染

- 状态：已采纳（2026-08-28）
- 取代：ADR-0008（TUI 历史归终端管，不用 altscreen）
- 依赖：ADR-0019（多 agent）

## 背景

ADR-0008 把历史交还终端：定型的行用 `tea.Println` 打进原生 scrollback，Bubble Tea 只重绘底部活动区。当时的理由成立且充分——终端已经有滚动、选中、复制、`Cmd+F`，重写一遍纯属浪费；输出退出后仍在，可管道、可 `tee`。

[[ADR-0019]] 之后多了一件 ADR-0008 的世界里没有位置的事：**切 agent**。

**scrollback 是一条只能追加的时间线，它表达不了「换一个源」。** 用户从 agent A 切到 B，B 的历史无处安放；更糟的是 B 在后台跑的那段时间，它的行**从来没被打进这个终端过**——scrollback 里根本没有那些内容，怎么翻都翻不到。

同一个洞今天就已经在，只是没人撞：TUI 的 `resume` 分支只渲染会话清单（`tui/cmd/realagent-tui/main.go:665`、`renderSessions`），**从来没重放过恢复出来的对话**。用户 `/resume` 完，core 侧历史回来了，屏幕上一片空白。altscreen 不制造这个洞，只是让补它变成必须。

## 决策

### 1. 进 alternate screen，自建 viewport

`bubbles/viewport` 负责滚动。**活动区不再是「底部那块」，它就是整个屏幕。**

### 2. 不开 mouse mode

滚动绑方向键 / PgUp / PgDn。**滚轮不靠 mouse mode**——现代终端（iTerm2 / WezTerm / kitty）的 alternate scroll 会在 altscreen 里把滚轮转成方向键，正好喂给 viewport。

这条是刻意的：**选中与复制没有任何库提供，它一直是终端的能力**，而开 mouse mode 会把它整个关掉（bubbletea [issue #162](https://github.com/charmbracelet/bubbletea/issues/162) *Allow both native text selection and mouse wheel scrolling*，至今开着，`WithMouseCellMotion` / `WithMouseAllMotion` 一开就 "disables text selection/highlighting completely"）。

不开 mouse mode，可见屏内的选中/复制照常工作。代价是滚出屏幕的内容要先滚回来才能选——真实，但比"选中复制全废"小得多。

**`j`·`k` 落地时去掉了**（起草时写过）：这个界面永远有一个输入行开着，`j` 与 `k` 是
用户正在打的字。vim 式滚动键要求有一个「非输入模式」，而引入模式是比"少两个快捷键"
大得多的代价。方向键在菜单没开着时归滚动，菜单开着时归菜单——一个 `if`，没有模式。

### 3. TUI 不在内存里存渲染后的行

**从会话记录读，动态渲染。** 判据与 ADR-0019 第 7 节的 idle 规则**逐字相同**：内存里那份是不是副本。是副本就能丢。

于是不管 core 里有 20 个还是 200 个 agent，TUI 的行缓冲恒为「当前可见的那一屏」。常驻的只有一个索引（每条消息在流里的位置、折行后占几行），量级 O(消息条数)，几十字节一条。

**终端改宽后历史可以重排了**——这是 ADR-0008 明写的那个代价（「所有 inline CLI 通病」）在本 ADR 下自动消失。索引在改宽时作废重建，仅此而已。

**实况注（落地时）：常驻的不是索引，是当前这个 agent 的行流（`[]line`，原始文本、不含 ANSI）。**
渲染后的行确实一行都不存——每帧按当前宽度重折，改宽重排因此成立。切 agent 时整个丢掉、
从 `GET /history` 重读一遍，所以本节要的那条不变量（**行缓冲与 agent 数量无关**）是成立的：
20 个还是 200 个 agent，内存里都只有正在看的那一个。

差别只在"单个会话很长"这一档：那份行流会跟着会话线性涨。上面那个索引方案才治得了它，
但它要一个能按行号往回取的分页端点，而现在的 `GET /history` 是一次全量。
**等真有人把一个会话跑到内存吃不消再做**——那时加的是分页，不是重写。

### 4. 新端点 `GET /history`，返回**事件帧序列**

TUI 是独立进程，可能还在另一台机器上，读不到 JSONL。

**参数走 JSON 体（`{client_id, agent_id}`），不走路径**：本项目的每个端点都这么读参数，
把 id 放进路径就是再养一套解析——第三种参数格式，两处必须永远一致。

**返回的是事件帧（`message_update` / `tool_execution_*` / `thinking_*` / …），不是抽象对话消息。** 于是 TUI **复用同一个渲染器**，一份代码。

返回消息形状看起来「core 侧零转换」，但那句省事是假的：转换没消失，只是从 core 挪到 TUI，而且挪成了两份必须逐像素一致的渲染代码。**同一段对话，实时看和翻历史看长得不一样，是那种没人报 bug 但一直硌人的东西。** 甲的转换在 core 侧、一处、可测（喂一段 JSONL，比对帧序列）。

### 5. 接缝在「最后一条已落盘的消息」

`Session::append` 是每条消息完成时即时追加，流式增量只活在事件里。所以 TUI 的视图 = **端点读回来的历史** + **事件帧喂进来的活尾巴**，接缝就在那条边界上。

**每个 agent 都有历史可读**，subagent 也不例外——它的会话落在 `<workdir>/.realagent/sessions/sub/`（[[ADR-0021]]），只是不进会话清单。所以 TUI 点进一个 subagent 照样翻得到它干过什么。

### 6. 「定型」这个概念消失

ADR-0008 的核心不变量（*追加式文本的贪心折行是前缀稳定的*）存在的唯一理由，是判断「哪些行可以立刻 `tea.Println` 进 scrollback」。**altscreen 下没有 `Println`，整个屏幕每帧重绘，也就没有「已提交／未提交」这条边界。**

随之删除的还有 ADR-0008 那条「提交保序」铁律——*同一时刻只许一条 `Println` 在飞，其余排队（`outbox`）*。没有 `Println`，就没有乱序，也就不需要那个队列。

保留的是**行模型**（`line{role, text}`、数据里不含 ANSI、样式在渲染最后一刻套上、`ansi.Wrap` 按显示宽折行）与**输入行编辑器**（`[]rune` + 光标下标）。那两样与 altscreen 无关，是 ADR-0008 里独立成立的部分。

## 代价

**这些是 ADR-0008 明确买到、本 ADR 卖掉的东西，逐条记下：**

- **退出即消失。** 今天跑完任务退出 TUI，记录还留在终端里可以往上翻。altscreen 之后屏幕一擦干净。缓解：记录本来就在 JSONL 里，但那要一个显式动作去读，不再是「往上滚一下」。
- **不能 `tee`、不能管道。** inline 输出可以重定向到文件，altscreen 不行。
- **滚出屏幕的内容要先滚回来才能选。**
- **滚轮依赖终端的 alternate scroll。** 老终端或关掉该特性的终端只能用键盘滚。

**换来的是：** 切 agent 可行、翻得到用户没在看的那些时段、改宽能重排历史、以及删掉「定型」与 `outbox` 两套机制。

## 迁移

`tui/cmd/realagent-tui` 的渲染路径整体重写：`tea.Println` + `outbox` 拆掉，换成 `viewport` + 每帧全量 `View()`。core 侧新增端点（`GET /session`，兼容 `GET /history`）与一段「JSONL → 事件帧」的转换（`Session::to_frames`，`core/src/agent/session.cpp`）。

ADR-0008 的单元测试 `TestFreezeIncrementalEqualsWhole` 随「定型」一起删除——它压的那条不变量不再有消费方。
