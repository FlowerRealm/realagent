/*
 * model.hpp — 模型元数据（ai 模块）
 *
 * 一个模型"是什么"：名称、归属供应商、上下文窗口。纯数据，无行为。
 *
 * core 定义类型，不内置任何数据（ADR-0009）：实例全部来自插件的 list_models。
 * 模型数据表是插件自己的文件（含单价），插件自读自解析、自己算钱，只把这三个
 * 字段报上来——**单价不在这里**：core 不算钱，存了没有消费方。
 *
 * 注册表不是白名单：用户在 settings.json 配了表外的模型，照发不误，不检查、
 * 不警告。供应商发新模型时不该被自己的 agent 锁在门外。
 */
#pragma once

#include <cstdint>
#include <string>

#include "../json.hpp"

namespace realagent {

struct Model {
    std::string name;     // 发给端点的模型 ID
    std::string owned_by; // 供应商身份
    int64_t context = 0;  // 上下文窗口（token）；0 = 未知，core 不解释
};
/* 严格解构（strict_from）：插件报的清单缺字段即该插件加载失败并点名。
 * 数据是人手写的模型数据表转来的，打错字要当场死，不静默补空值。 */
BOOST_DESCRIBE_STRUCT(Model, (), (name, owned_by, context))

} // namespace realagent
