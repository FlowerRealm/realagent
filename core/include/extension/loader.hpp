/*
 * loader.hpp — 插件加载器（extension 模块）
 *
 * 职责（ADR-0001）：
 *  - 扫描目录（项目级 .realagent/extensions + 全局 ~/.realagent/extensions）
 *  - 解析 plugin.json（名称/描述/版本/ABI/前置依赖/type）
 *  - dlopen + ABI 强校验（PLUGIN_ABI_VERSION）+ 创建实例 + init
 *  - 按前置依赖声明组装嵌套链（ADR-0004）
 *  - 插件生命周期管理（R15）：known_ 发现登记（loaded/disabled/failed）
 *    + 运行时 enable/disable（GET /plugins 数据源）
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
    Config* config = nullptr; // 非 const：plugins 禁用清单写入（Config::set/persist）需写路径
    std::unordered_map<std::string, ToolEntry> tools;
    std::unordered_map<std::string, CommandEntry> commands;
    /* 已加载插件（PluginManager::load_all 后填充，供 executor/agent 遍历） */
    std::vector<Plugin*> all_plugins;
    /* 事件出口：把事件推给客户端（TUI/gui 推送流）。M5 接入，首版可为空。 */
    std::function<void(const std::string& type, const std::string& payload)> emit_fn;
    /* 按名查已加载插件（loader 注入：插件 api 函数经此实现 get_dependency 依赖注入） */
    std::function<Plugin*(const std::string& name)> find_plugin;
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

/* plugin.json 的形状（入站）。与 PluginInfo 分开：dir/status/error 是 core 运行时
 * 填的，不在 plugin.json 里——混在一起会把它们误报成"缺键"。
 * 严格解构（strict_from）：缺任一键即该插件加载失败。plugin.json 漏个 type
 * 以前会静默变空串，插件从此在链路里神秘失踪且无处可查。 */
struct PluginManifest {
    std::string name;
    std::string description;
    std::string version;
    std::string type; // protocol / tool / permission / session
    int abi_version = 0;
    std::vector<std::string> deps;
};
BOOST_DESCRIBE_STRUCT(PluginManifest, (), (name, description, version, type, abi_version, deps))

/* 插件状态快照（GET /plugins 数据源；status: loaded / disabled / failed）。
 * 字段名与顺序即 PROTOCOL.md 的响应契约——describe 直接出站，不手写搬运。 */
struct PluginInfo {
    std::string name;
    std::string version;
    std::string type;
    std::string description;
    std::string dir;
    std::string status; // loaded / disabled / failed
    std::string error;  // 加载失败原因（status=failed 时填充）
    std::vector<std::string> deps;
};
BOOST_DESCRIBE_STRUCT(PluginInfo, (),
                      (name, version, type, description, dir, status, error, deps))

class PluginManager {
public:
    explicit PluginManager(CoreContext& ctx);
    ~PluginManager();

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    /* 扫描 + 加载 + ABI 校验 + init + 嵌套组装。
     * 遵守配置 plugins.disabled 禁用清单；返回失败数量。 */
    int load_all();

    /* 逆序 destroy + dlclose */
    void shutdown();

    /* 全部已发现插件（loaded/disabled/failed，含失败原因），按发现顺序 */
    std::vector<PluginInfo> list() const;

    /* 运行时启用：从 known 目录重载 + 移出禁用清单 + persist。未知/加载失败 → false */
    bool enable(const std::string& name);

    /* 运行时禁用：destroy + dlclose + 注销工具/命令 + 加入禁用清单 + persist。未知 → false */
    bool disable(const std::string& name);

    const std::vector<std::unique_ptr<Plugin>>& plugins() const { return plugins_; }
    Plugin* find(const std::string& name) const;

private:
    /* 发现：解析 plugin.json 并登记 known_（元数据/目录）。非插件目录返回 nullptr */
    PluginInfo* discover(const std::string& plugin_dir);
    /* 加载主体（load_all 与 enable 共用）：从已发现条目载入。
     * 成功 → plugins_ 追加 + known_ 状态 loaded；失败 → known_ failed + 日志，返回 false */
    bool load_plugin(PluginInfo* info);
    void assemble_nested();
    /* 注销插件注册的 tool/command（owner 匹配） */
    void unregister_entries(const Plugin* p);
    /* destroy + dlclose（shutdown / disable 共用） */
    void destroy(Plugin* p);

    PluginInfo* find_known(const std::string& name);
    /* 读配置禁用清单（合并树 plugins.disabled 数组，write_disabled 的反向） */
    std::vector<std::string> read_disabled() const;
    /* 写配置禁用清单（Config::set + persist） */
    bool write_disabled(const std::vector<std::string>& list);

    CoreContext& ctx_;
    std::vector<std::unique_ptr<Plugin>> plugins_; // 仅已加载（消费方遍历此表，语义不变）
    std::vector<PluginInfo> known_;                // 全部已发现（含 disabled/failed）
};

} // namespace realagent
