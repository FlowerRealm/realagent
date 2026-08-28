/*
 * context.hpp — core 运行上下文
 *
 * 配置 + 事件出口，两样东西，一个结构体。agent / executor / 工具都要用它们，
 * 到处传两个参数不如传一个引用。
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

struct CoreContext {
    Config *config = nullptr;
    EmitFn emit_fn;
};

} // namespace realagent
