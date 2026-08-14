/*
 * config.hpp — 配置（core 统一收集，注入给插件）
 *
 * 唯一来源：~/.realagent/settings.json（全局）。不看 cwd，不看项目目录，
 * 不存在项目级覆盖这回事。没有 env 覆盖，没有内置默认。
 *
 * 必需键（api_key / base_url / model / small_model）缺一即 load() 失败：
 * 配置是刚需——core 不猜端点、不猜模型、不回落。起不来好过连错地方。
 * 文件存在但解析不了同样是硬错，不静默跳过。
 *
 * 模型档位（ModelTier）：主模型 model 干正事，小模型 small_model 干杂活。
 * 两档共用 base_url / api_key——档位只换模型名，不换端点凭证。两档都必配。
 *
 * 会话目录不是配置项，是 core 自己的落盘路径：写死 .realagent/sessions。
 *
 * 插件初始化时经 ra_core_api.get_config 读取，插件不自行解析配置。
 * 线程安全：内部 mutex 保护配置树——agent 线程并发 get / 事件循环 set、persist 均安全。
 * persist() 原子写 ~/.realagent/settings.json（tmp+rename）。
 * 变更 API（set/persist）仅服务于运行时开关的持久化：插件启停、/model 切档。
 */
#pragma once

#include <expected>
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
    Small  // 小模型（标题/摘要一类杂活）
};

class Config {
public:
    // 加载：全局 settings.json + 项目级 settings.json（项目覆盖全局）。
    // 失败返回人话错误（缺哪个键 / 哪个文件解析不了），调用方直接打印退出。
    static std::expected<Config, std::string> load();

    // 必需配置键（缺一个 load 就失败）
    static std::vector<std::string_view> required_keys();

    // 读取配置项（无默认值：键不存在返回空串）
    std::string get(std::string_view key) const;
    bool has(std::string_view key) const;

    // 按档位取模型名（键名只此一处知道）
    std::string model(ModelTier tier) const;

    // 变更配置树（不落盘，供插件禁用清单等运行时改动）
    void set(std::string_view key, const json& v);
    // 原子写 ~/.realagent/settings.json
    bool persist();

    // 插件发现目录（全局 ~/.realagent/extensions，唯一来源）
    std::vector<std::string> extension_dirs() const;

    // 会话存储目录（core 常量，不可配置）
    std::string session_dir() const;

    // 模型数据表的运行时落点（用户接管版）：~/.realagent/models/<插件名>.json。
    // 内容是插件的数据（含单价），core 只认路径不认内容（ADR-0009）
    std::string models_path(std::string_view plugin_name) const;

    // 合并后的完整配置（JSON，注入给插件 init 用）
    json to_json() const;

private:
    json settings_;  // 配置树（全局 ~/.realagent/settings.json）
    // mutex 不可拷贝/移动，用 shared_ptr 包装保持 Config 可拷贝（load() 按值返回）
    mutable std::shared_ptr<std::mutex> mutex_ = std::make_shared<std::mutex>();

    // —— 内部（调用方已持锁）——
    bool persist_locked();
};

// 环境变量读取（找不到返回 fallback）。配置不走 env，此处只服务 HOME 一类进程环境。
std::string getenv_or(std::string_view name, std::string_view fallback = "");

} // namespace realagent
