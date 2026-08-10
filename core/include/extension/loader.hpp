/*
 * loader.hpp — 插件加载器（extension 模块）
 *
 * 职责（ADR-0001）：
 *  - 扫描目录（项目级 .realagent/extensions + 全局 ~/.realagent/extensions）
 *  - 解析 plugin.json（名称/描述/版本/ABI/前置依赖/type）
 *  - dlopen + ABI 强校验（PLUGIN_ABI_VERSION）+ 创建实例 + init
 *  - 按前置依赖声明组装嵌套链（ADR-0004）
 */
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../config.hpp"
#include "../../sdk/plugin_api.h"

namespace realagent {

struct Plugin;

/* 已注册的工具（工具对 core 只是注册表条目，来源无差别） */
struct ToolEntry {
    plugin_tool_t def;
    struct Plugin* owner; /* 拥有该工具的插件（C++ 侧） */
};

/* 已注册的斜杠命令 */
struct CommandEntry {
    plugin_command_t def;
    struct Plugin* owner;
};

/* core 运行上下文：配置 + 注册表 + 事件出口。插件经 plugin_core_t.ctx 访问。 */
struct CoreContext {
    const Config* config = nullptr;
    std::unordered_map<std::string, ToolEntry> tools;
    std::unordered_map<std::string, CommandEntry> commands;
    /* 事件出口：把事件推给客户端（TUI/gui 推送流）。M5 接入，首版可为空。 */
    std::function<void(const std::string& type, const std::string& payload)> emit_fn;
};

/* 加载的插件实例 */
struct Plugin {
    std::string dir;           // 插件目录
    std::string name;          // 插件名（plugin.json name）
    std::string description;
    std::string version;
    std::string type_name;     // protocol / tool / permission / session
    std::vector<std::string> deps; // 前置依赖声明（plugin.json deps + init 中 depends_on）

    void* dl_handle = nullptr; // dlopen 句柄
    const plugin_api_t* api = nullptr;
    plugin_t* instance = nullptr;
    plugin_core_t core_handle{}; // 传给插件的 core 句柄

    CoreContext* core_ctx = nullptr; // 指向所属 CoreContext
};

class PluginManager {
public:
    explicit PluginManager(CoreContext& ctx);
    ~PluginManager();

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    /* 扫描 + 加载 + ABI 校验 + init + 嵌套组装。返回失败数量。 */
    int load_all();

    /* 逆序 destroy + dlclose */
    void shutdown();

    const std::vector<std::unique_ptr<Plugin>>& plugins() const { return plugins_; }
    Plugin* find(const std::string& name) const;

private:
    void load_one_dir(const std::string& dir_path);
    void load_plugin(const std::string& plugin_dir);
    void assemble_nested();

    CoreContext& ctx_;
    std::vector<std::unique_ptr<Plugin>> plugins_;
};

} // namespace realagent
