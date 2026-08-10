# ADR-0003: 语言标准采用 C++26，异步基于协程

- 状态：已接受（可行性验证 2026-08-09）
- 日期：2026-08-09

## 决策

项目语言标准为 **C++26**，异步事件流基于 **协程（coroutine）** 实现。

**工具链**：编译器采用 **Clang 19+（brew llvm）**——macOS 原生工具链生态（与 Xcode/系统库兼容好），对 C++26 协程支持持续跟进中。构建系统 CMake（预设管理）。Go TUI 侧（ADR-0007）不受此约束。

## 调研对标

| 项目 | 语言 | 异步/事件模型 |
|---|---|---|
| Pi | TypeScript | async iterable（原生异步，JS 天然） |
| OpenCode | Go | goroutine + channel（`<-chan AgentEvent`） |
| Codex（Rust 侧） | Rust | tokio 异步运行时 |
| Goose | Rust | tokio |

C++ 没有语言级 channel/goroutine，协程是最贴近的异步表达。

## 候选方案与权衡

异步事件流（ADR-0002）在 C++ 里的三条实现路径：

1. **自研事件队列 + 线程池** —— 零依赖，但手写调度/线程安全/取消，易引入并发 bug。
2. **C++ 协程（选定）** —— 语法表达事件流优雅。**协程自 C++20 起即支持异常**（经 `promise_type::unhandled_exception()`，未处理异常在 resume 点重新抛出）。Clang 对 C++20 协程（P0912R5）完全支持。
3. **第三方事件循环（libuv/Asio）** —— 生产级可靠，但核心依赖第三方异步库，C ABI 边界要包一层。

选定方案 2。

## 可行性验证（2026-08-09）——关键查证

**查证过程**：查询 clang.llvm.org/cxx_status.html 的 C++26（C++2c）特性表——
- **P2561（非抛出协程）在 Clang 的 C++26 特性表中不存在（未实现）**。
- **C++20 协程（P0912R5）标记 "Partial"**，附注："Fully supported on all targets except Windows, which still has some stability and ABI issues"。macOS 上完全支持。

**结论**：本项目异步事件流基于 C++20 协程核心能力即可实现（async await / co_await / promise 机制），**不依赖 P2561 等未实现的 C++26 协程新特性**。早期文档中"协程不支持异常（C++26 才补全）"为**错误表述**——协程从 C++20 起即支持异常（经 `unhandled_exception()`）。ADR-0003 不依赖未实现特性，**风险解除**。

## 后果

- 要求较新的编译器和标准库（Clang 19+ / brew llvm）。
- 协程代码与 C ABI 扩展边界的交互方式需要专门设计——**扩展侧不暴露协程，仍是同步函数指针 + 事件投递**（ADR-0001 的 on_event 单入口）。
- 全项目代码风格基于 C++26，后续代码审查、示例、文档都以此为准。
