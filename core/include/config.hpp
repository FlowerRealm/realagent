/*
 * config.hpp — 配置（core 统一收集，注入给插件）
 *
 * 优先级：env > .realagent/settings.json > 默认值
 * 插件初始化时经 ra_core_api.get_config 读取，插件不自行解析配置。
 */
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"

namespace realagent {

class Config {
public:
    // 加载：默认值 + settings.json（项目级 .realagent/ + 全局 ~/.realagent/）+ env 覆盖
    static Config load();

    // 读取配置项（env > settings.json > 默认）
    std::string get(std::string_view key, std::string_view default_value = "") const;
    bool has(std::string_view key) const;

    // 插件发现目录（项目级 .realagent/extensions + 全局 ~/.realagent/extensions）
    std::vector<std::string> extension_dirs() const;

    // 会话存储目录（.realagent/sessions）
    std::string session_dir() const;

    // 合并后的完整配置（JSON，注入给插件 init 用）
    json to_json() const;

private:
    json settings_;  // 合并后的配置树（settings.json + 默认）
};

// 环境变量读取（找不到返回空）
std::string getenv_or(std::string_view name, std::string_view fallback = "");

// 查找项目根（向上找 .realagent/ 或 .git），返回项目目录；找不到返回空
std::string find_project_root(std::string_view start_dir);

} // namespace realagent
