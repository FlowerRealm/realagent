/*
 * context.hpp — core 运行上下文
 *
 * 配置 + 模型数据表 + 事件出口。三样东西，一个结构体。agent / executor / 工具都要用它们，
 * 到处传三个参数不如传一个引用。
 *
 * 模型数据表在这里而不在 Agent 里（ADR-0019）：它是**进程级只读数据**，启动读一次。
 * 按值当 Agent 的成员，N 个 agent 就是 N 份表。
 *
 * 事件出口只有一条：main 挂上去的那个入队函数（agent 线程 emit → 事件循环 flush
 * 到推送流，ADR-0002 线程模型）。没有扇出、没有订阅者，事件的去处只有客户端。
 */
#pragma once

#include <functional>
#include <string>

#include "config.hpp"

namespace realagent {

/* 事件出口：type + JSON 载荷字符串 */
using EmitFn = std::function<void(const std::string &type, const std::string &payload)>;

class Pricing;

struct CoreContext {
    Config *config = nullptr;
    const Pricing *pricing = nullptr;
    EmitFn emit_fn;
};

} // namespace realagent
