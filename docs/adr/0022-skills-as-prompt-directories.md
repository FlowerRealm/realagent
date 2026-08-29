# ADR-0022：加入 Skill —— 提示词目录，不是插件的第二次转世

- 状态：已采纳（2026-08-29）
- 依赖：ADR-0016（插件体系废除，工具是 core 里一张静态表）、ADR-0018（不为纯文本另开 `read_skill`）、ADR-0019（agent 必传 workdir）、ADR-0010（配置读一次，不热重载）
- 不取代任何 ADR

## 背景

ADR-0016 在 2026-08-25 把整套插件体系铲平，理由是"它描述的那个东西一个用户都没有"。四天后加 skill，
第一件要回答的事不是"怎么做"，是**"这不就是插件换了个名字吗"**。

不是。区别不在规模，在 **core 里有没有多出一条执行路径**：

| ADR-0016 列的税目 | 插件 | skill |
| --- | --- | --- |
| C ABI、借阅 / 转移 | 每个跨界 `const char*` 都要管所有权 | 没有跨界指针——只有文件路径和字符串 |
| 命名空间前缀 | 工具对外一个名、对内一个名 | 不注册任何东西，没有名字空间 |
| 能力槽解析 | 130 行回答"管线四段谁来干" | 管线不变，一段都不换 |
| 撞名检查 | `O(n²)` 防不存在的第三方 | 同名就近的赢，一行 |
| 异常不得穿越 ABI | 边界上转状态码，否则 `terminate` | 没有边界 |
| 外部仓库 | 加载器 + SDK + 规范 + 两套 CMake | 一个目录、一份 markdown |

插件是 core 加载并调用别人的代码；skill 是 core 把一段文字的位置告诉模型。
执行的永远是模型手上那六个工具。**"用不上的抽象没有折旧价值"那条结论没被推翻——
skill 根本不是一个抽象，它是一份数据。**

真问题也是现成的：一个仓库特有的干活方式，现在只有两个落点——编译进 system prompt（改一次要发一次版），
或者用户每次对话手打一遍。

## 决策

**skill = 一个目录 + 一份 `SKILL.md`，纯提示词。core 不加载、不执行、不给它任何执行语义。**

### 1. 形状不由本项目定义

照抄公开的 [Agent Skills 规范](https://agentskills.io/specification)：YAML frontmatter + Markdown 正文，
必需字段 `name` 与 `description`，`name` 必须与父目录名一致，`scripts/` `references/` `assets/` 为可选目录。
规范里的渐进披露（启动只加载 name + description，激活后才读正文）就是本项目要的那条路径，一字不改。

**不发明第二种格式**：用户从别处抄一份 skill 丢进目录里，就该能用。

### 2. 两处来源，同名近的覆盖远的

- `~/.realagent/skills/<name>/SKILL.md` —— 跟着人走
- `<workdir>/.realagent/skills/<name>/SKILL.md` —— 跟着仓库走，可进版本库

这不违反"配置约定：只有全局 `~/.realagent/`"。项目级 `settings.json` 被砍（2026-08-28）的理由是那句
问不出答案的话——"同一个 core 里 N 个 agent，N 份项目配置合并给谁用"——**因为配置是进程级的，
而 core 没有 cwd**。skill 的消费者是一个 agent，agent 恰好有一个 workdir，同一个问题在这里有唯一答案。
同一条判据下 `<workdir>/.realagent/sessions` 早就是项目级的了。

### 3. agent 创建时扫一次

清单挂在 `Agent` 上，与 workdir 同期确定、同期不变。改了 skill 开个新 agent，跟 ADR-0010
"启动读一次、不热重载"是同一个作风。

**不在 `build_dialog` 里扫**：那会让一趟中间 system prompt 变形，prompt cache 当场碎掉，
而目录内容在一趟中间变的概率约等于零。

### 4. 不加第七个工具

system prompt 里每个 skill 一行：名字、描述、绝对路径。正文要不要读由模型决定，用现成的 `read` 读。
ADR-0018 为纯文本拒绝过 `read_skill`，理由（工具每多一个，模型每次调用就多一次选错的机会）在这里一字未改。

代价诚实记下：模型读正文时会看到行号与行 hash 前缀。那是**读那一次**的噪声，
不是**每次调用**都要付的选择成本，两者不同量级。

零 skill 时 system prompt 一个字不变。

### 5. frontmatter 用 fkYAML，不手写

起草时的方案是手写三十行、只认 `name` 与 `description`。它错在一处：`SKILL.md` 是从互联网抄来的
第三方输入，形状不由 core 说了算。实测现成 skill 里约三成的 `description` 用 YAML 折叠标量
（`description: >`，值在后续缩进行）——手写解析器碰上它不会报错，它会**安静地把描述设成 `>`**。

**"不兜底"反对的是替用户擦屁股，不是反对按格式的真实定义去解析它。**

fkYAML 单头文件 vendored 在 `core/include/fkYAML.hpp`，与 `json.hpp` 同一条路子：不必安装、不必链库，
`find_package` 仍旧是 libcurl / spdlog / quiche 三个。

`name` 取目录名——规范要求两者一致，读出来再比对只是给自己造一类要处理的错误；
解析器唯一的活是拿到 `description`。规范对 `name` 的字符约束（小写、不得连续连字符、≤64）**core 不校验**，
那是给写 skill 的人看的，与"配置的模型名不在表里不检查不警告"是同一条规矩。

### 6. 坏 skill：跳过，报错原文

先例有两条，挑的是 `models.json` 那条（报错但不拒绝启动），不是 `settings.json` 那条（硬错退出）。
为一份可选的提示词拒绝创建 agent，是把次要功能提成了必需品。

"不跳过坏条目"在这里不适用，区别在**单位**：`models.json` 是一份文件里的 N 个条目，跳过坏条目
会让同一份文件半真半假；skill 是 N 份**各自独立的文件**，一份坏了不让另一份变得可疑。

## 代价与保留

- **system prompt 变长**。上限是用户自己给的：规范限定描述 ≤1024 字符，十个 skill 最坏 10KB。**不截断**，
  跟 `read` 不限大小同一个作风。
- **全局 skill 对每个 agent 生效**，subagent 也算。想让某个 agent 不受影响，只能不装那个全局 skill——
  这是渐进披露的定价：清单是全的，正文才是按需的。
- **`scripts/` core 不管**。规范允许 skill 目录里放脚本，但跑脚本的是模型手上的 `bash`，
  照 `dangerous` 那条走权限检查。"skill 不是可执行体"与"skill 目录里可以有脚本"不冲突——执行的不是 core。
- **没做的**：`/skills` 斜杠命令、skill 启用 / 禁用开关、skill 携带自己的工具或 MCP server。
  前两个的答案是"装了就用，不想用就删目录"；第三个就是插件，ADR-0016 那本账重算一遍答案不会变。
