/*
 * config.hpp — 配置（core 统一收集，注入给插件）
 *
 * 优先级链：env > 平铺字段 > 内置默认
 *  - env：      ANTHROPIC_API_KEY→api_key、DEEPSEEK_BASE_URL→base_url、
 *               DEEPSEEK_MODEL→model、DEEPSEEK_SMALL_MODEL→small_model
 *  - 平铺：     旧式 base_url / api_key / model 字段（向后兼容）
 *  - 默认：     https://api.deepseek.com/anthropic / deepseek-v4-flash
 *
 * 模型档位（ModelTier）：主模型 model 干正事，小模型 small_model 干杂活。
 * 两档共用 base_url / api_key——档位只换模型名，不换端点凭证。
 * small_model 不设独立内置默认：未配置即回落主模型（回落就是默认）。
 *
 * 插件初始化时经 ra_core_api.get_config 读取，插件不自行解析配置。
 * 线程安全：内部 mutex 保护合并树——agent 线程并发 get / 事件循环 set、persist 均安全。
 * persist() 原子写项目 .realagent/settings.json（tmp+rename），env 注入值不入盘
 * （避免把 ANTHROPIC_API_KEY 等烘焙进文件）。
 * 变更 API（set/persist）仅服务于插件启停持久化。
 */
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"

namespace realagent {

/* 模型档位：一次 LLM 调用用哪一档模型 */
enum class ModelTier {
    Main,  // 主模型（对话主链路）
    Small  // 小模型（标题/摘要一类杂活；未配置则回落主模型）
};

class Config {
public:
    // 加载：默认值 + settings.json（项目级 .realagent/ + 全局 ~/.realagent/）+ env 覆盖
    static Config load();

    // 读取配置项（env > 平铺 > 默认）
    std::string get(std::string_view key, std::string_view default_value = "") const;
    bool has(std::string_view key) const;

    // 按档位取模型名（Small 未配置时回落主模型——键名只此一处知道）
    std::string model(ModelTier tier) const;

    // 变更合并树（不落盘，供插件禁用清单等运行时改动）
    void set(std::string_view key, const json& v);
    // 原子写项目 .realagent/settings.json（保留文件原有内容，env 注入值不入盘）
    bool persist();

    // 插件发现目录（项目级 .realagent/extensions + 全局 ~/.realagent/extensions）
    std::vector<std::string> extension_dirs() const;

    // 会话存储目录（.realagent/sessions）
    std::string session_dir() const;

    // 合并后的完整配置（JSON，注入给插件 init 用）
    json to_json() const;

private:
    json settings_;                  // 合并后的配置树（settings.json + 默认 + env）
    std::vector<std::string> env_keys_;  // env 注入的键（persist 时不写入文件）
    // mutex 不可拷贝/移动，用 shared_ptr 包装保持 Config 可拷贝（load() 按值返回）
    mutable std::shared_ptr<std::mutex> mutex_ = std::make_shared<std::mutex>();

    // —— 内部（调用方已持锁）——
    bool persist_locked();
};

// 环境变量读取（找不到返回空）
std::string getenv_or(std::string_view name, std::string_view fallback = "");

// 查找项目根（向上找 .realagent/ 或 .git），返回项目目录；找不到返回空
std::string find_project_root(std::string_view start_dir);

} // namespace realagent
