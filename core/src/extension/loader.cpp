#include "extension/loader.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "json.hpp"

namespace realagent {
namespace fs = std::filesystem;

namespace {

/* —— core → 插件 API 实现（plugin_core_api_t）。
 * core->ctx 指向 Plugin，CoreContext 经 Plugin::core_ctx 取得。 —— */

static CoreContext* ctx_of(plugin_core_t* core) {
    return static_cast<Plugin*>(core->ctx)->core_ctx;
}

plugin_status_t api_register_tool(plugin_core_t* core, const plugin_tool_t* tool) {
    auto* ctx = ctx_of(core);
    auto* owner = static_cast<Plugin*>(core->ctx);
    ToolEntry e{tool[0], owner};
    ctx->tools[tool->name] = std::move(e);
    return PLUGIN_OK;
}

plugin_status_t api_register_command(plugin_core_t* core, const plugin_command_t* cmd) {
    auto* ctx = ctx_of(core);
    auto* owner = static_cast<Plugin*>(core->ctx);
    CommandEntry e{cmd[0], owner};
    ctx->commands[cmd->name] = std::move(e);
    return PLUGIN_OK;
}

plugin_status_t api_emit(plugin_core_t* core, const char* type, const char* payload) {
    auto* ctx = ctx_of(core);
    if (ctx->emit_fn) ctx->emit_fn(type, payload);
    return PLUGIN_OK;
}

void api_log(plugin_core_t* core, int level, const char* msg) {
    (void)core;
    const char* lv = level <= 1 ? "debug" : level == 2 ? "info" : "warn/err";
    fprintf(stderr, "[plugin][%s] %s\n", lv, msg);
}

const char* api_get_config(plugin_core_t* core, const char* key) {
    auto* ctx = ctx_of(core);
    static std::string slot; // 单线程首版可接受；多线程需换 thread_local
    slot = ctx->config->get(key);
    return slot.c_str();
}

plugin_status_t api_depends_on(plugin_core_t* core, const char* plugin_name) {
    auto* p = static_cast<Plugin*>(core->ctx);
    p->deps.emplace_back(plugin_name);
    return PLUGIN_OK;
}

const plugin_core_api_t k_core_api = {
    .register_tool = api_register_tool,
    .register_command = api_register_command,
    .emit = api_emit,
    .log = api_log,
    .get_config = api_get_config,
    .depends_on = api_depends_on,
};

/* 标记 known_ 条目加载失败：打日志 + 记录原因（保留原有日志行为） */
void mark_failed(PluginInfo* info, const std::string& reason) {
    fprintf(stderr, "[extension] %s: %s\n", info->name.c_str(), reason.c_str());
    info->status = "failed";
    info->error = reason;
}

/* 标记 known_ 条目为禁用（不加载） */
void mark_disabled(PluginInfo* info) {
    info->status = "disabled";
    info->error.clear();
}

} // namespace

PluginManager::PluginManager(CoreContext& ctx) : ctx_(ctx) {}

PluginManager::~PluginManager() { shutdown(); }

int PluginManager::load_all() {
    int failures = 0;
    const auto disabled = read_disabled(); // 遵守配置 plugins.disabled 禁用清单
    for (const auto& dir : ctx_.config->extension_dirs()) load_one_dir(dir, disabled);
    assemble_nested();
    // 统计失败：known_ 中 status=failed 的数量
    for (const auto& k : known_) {
        if (k.status == "failed") {
            ++failures;
            fprintf(stderr, "[extension] %s: 加载失败: %s\n", k.name.c_str(), k.error.c_str());
        }
    }
    return failures;
}

void PluginManager::load_one_dir(const std::string& dir_path,
                                 const std::vector<std::string>& disabled) {
    if (!fs::is_directory(dir_path)) return;
    for (const auto& entry : fs::directory_iterator(dir_path)) {
        if (!entry.is_directory()) continue;
        PluginInfo* info = discover(entry.path().string());
        if (!info) continue; // 无 plugin.json / 解析失败（已打日志），不登记 known_
        if (std::find(disabled.begin(), disabled.end(), info->name) != disabled.end()) {
            mark_disabled(info); // 禁用清单内：登记为 disabled，不加载
            fprintf(stderr, "[extension] 已跳过（禁用清单）: %s\n", info->name.c_str());
            continue;
        }
        load_plugin(info);
    }
}

PluginInfo* PluginManager::discover(const std::string& plugin_dir) {
    // 解析 plugin.json
    const auto meta_path = fs::path(plugin_dir) / "plugin.json";
    std::ifstream f(meta_path);
    if (!f) return nullptr; // 无 plugin.json 的目录跳过
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto meta = json::parse(text);
    if (!meta) {
        fprintf(stderr, "[extension] 解析 plugin.json 失败: %s\n", plugin_dir.c_str());
        return nullptr;
    }
    const json& m = *meta; // 用链式 operator[]（读缺键返回 null，不抛异常）
    const auto name = m["name"].as_string();
    if (!name) return nullptr;

    // 同名多目录（项目级+全局级均发现）：复用条目，dir/元数据以最后发现为准
    for (auto& k : known_) {
        if (k.name == *name) {
            k.dir = plugin_dir;
            k.description = m["description"].as_string().value_or("");
            k.version = m["version"].as_string().value_or("0");
            k.type_name = m["type"].as_string().value_or("");
            k.deps.clear();
            if (const json d = m["deps"]; d.is_array()) {
                for (const auto& dd : d.as_array())
                    if (auto s = json(dd).as_string()) k.deps.push_back(*s);
            }
            return &k;
        }
    }

    PluginInfo info;
    info.dir = plugin_dir;
    info.name = *name;
    info.description = m["description"].as_string().value_or("");
    info.version = m["version"].as_string().value_or("0");
    info.type_name = m["type"].as_string().value_or("");
    if (const json d = m["deps"]; d.is_array()) {
        for (const auto& dd : d.as_array())
            if (auto s = json(dd).as_string()) info.deps.push_back(*s);
    }
    known_.push_back(std::move(info));
    return &known_.back();
}

bool PluginManager::load_plugin(PluginInfo* info) {
    // 构造插件实例；元数据取自已发现条目（load_all 与 enable 共用的加载主体）
    auto p = std::make_unique<Plugin>();
    p->dir = info->dir;
    p->name = info->name;
    p->description = info->description;
    p->version = info->version;
    p->type_name = info->type_name;
    p->deps = info->deps;

    // dlopen 插件库（.dylib 优先，次 .so）
    std::string lib_path;
    for (const auto& cand : {fs::path(info->dir) / (info->name + ".dylib"),
                             fs::path(info->dir) / (info->name + ".so")}) {
        if (fs::exists(cand)) { lib_path = cand.string(); break; }
    }
    if (lib_path.empty()) {
        mark_failed(info, "找不到插件库 .dylib/.so");
        return false;
    }
    p->dl_handle = dlopen(lib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!p->dl_handle) {
        mark_failed(info, std::string("dlopen 失败: ") + dlerror());
        return false;
    }
    auto create_fn = reinterpret_cast<plugin_create_fn>(dlsym(p->dl_handle, PLUGIN_CREATE_SYM));
    if (!create_fn) {
        mark_failed(info, std::string("缺少导出符号 ") + PLUGIN_CREATE_SYM);
        dlclose(p->dl_handle);
        return false;
    }
    p->instance = create_fn(&p->api);
    if (!p->instance || !p->api) {
        mark_failed(info, "plugin_create 失败");
        if (p->dl_handle) dlclose(p->dl_handle);
        return false;
    }
    // ABI 强校验
    if (p->api->abi_version != PLUGIN_ABI_VERSION) {
        char buf[128];
        snprintf(buf, sizeof buf, "ABI 版本不符 (插件 %d, core %d)，请重编插件",
                 p->api->abi_version, PLUGIN_ABI_VERSION);
        mark_failed(info, buf);
        dlclose(p->dl_handle);
        p->dl_handle = nullptr;
        return false;
    }
    // 初始化：core 句柄 ctx 指向 Plugin（api 函数经 Plugin::core_ctx 取 CoreContext）
    p->core_ctx = &ctx_;
    p->core_handle.api = &k_core_api;
    p->core_handle.ctx = p.get();
    if (p->api->init) {
        if (p->api->init(p->instance, &p->core_handle) != PLUGIN_OK) {
            fprintf(stderr, "[extension] %s: init 失败\n", p->name.c_str());
        }
    }
    fprintf(stderr, "[extension] 已加载: %s (%s) v%s\n", p->name.c_str(), p->type_name.c_str(),
            p->version.c_str());
    info->status = "loaded";
    info->error.clear();
    info->deps = p->deps; // init 中 depends_on 追加的依赖同步回 known_
    plugins_.push_back(std::move(p));
    return true;
}

void PluginManager::assemble_nested() {
    // 嵌套组装（ADR-0004）：插件声明的 deps 在 plugin.json/init 中已收集。
    // 首版不做跨插件对象注入——依赖关系由 core 记录，真正的
    // 包裹调用（vendor 调 protocol）在请求管线中按 deps 顺序解析。
    // TODO(M1): 若某插件声明依赖但目标缺失，标记加载失败。
    for (auto& p : plugins_) {
        for (const auto& d : p->deps) {
            if (!find(d)) {
                fprintf(stderr, "[extension] %s: 依赖插件 %s 未找到\n", p->name.c_str(), d.c_str());
            }
        }
    }
}

void PluginManager::unregister_entries(const Plugin* p) {
    // 注销该插件注册的 tool / command（owner 匹配），其余条目不动
    for (auto it = ctx_.tools.begin(); it != ctx_.tools.end();) {
        if (it->second.owner == p) it = ctx_.tools.erase(it);
        else ++it;
    }
    for (auto it = ctx_.commands.begin(); it != ctx_.commands.end();) {
        if (it->second.owner == p) it = ctx_.commands.erase(it);
        else ++it;
    }
}

void PluginManager::destroy(Plugin* p) {
    if (p->instance && p->api && p->api->destroy) p->api->destroy(p->instance);
    if (p->dl_handle) dlclose(p->dl_handle);
    p->instance = nullptr;
    p->dl_handle = nullptr;
}

void PluginManager::shutdown() {
    // 逆序销毁（依赖插件先销毁）+ dlclose
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) destroy(it->get());
    plugins_.clear();
}

Plugin* PluginManager::find(const std::string& name) const {
    for (const auto& p : plugins_)
        if (p->name == name) return p.get();
    return nullptr;
}

std::vector<PluginInfo> PluginManager::list() const { return known_; }

PluginInfo* PluginManager::find_known(const std::string& name) {
    for (auto& k : known_)
        if (k.name == name) return &k;
    return nullptr;
}

std::vector<std::string> PluginManager::read_disabled() const {
    std::vector<std::string> out;
    // 禁用清单存于合并树 plugins.disabled 数组（write_disabled 写入的结构，读取为其反向）
    const json d = ctx_.config->to_json()["plugins"]["disabled"];
    if (d.is_array()) {
        for (const auto& v : d.as_array())
            if (auto s = json(v).as_string()) out.push_back(*s);
    }
    return out;
}

bool PluginManager::write_disabled(const std::vector<std::string>& list) {
    // 写入合并树 plugins.disabled 并持久化（契约：Config::set + persist）
    json plugins;
    json arr = json::array();
    for (const auto& n : list) arr.push_back(n);
    plugins["disabled"] = std::move(arr);
    ctx_.config->set("plugins", plugins);
    return ctx_.config->persist();
}

bool PluginManager::enable(const std::string& name) {
    PluginInfo* info = find_known(name);
    if (!info) {
        fprintf(stderr, "[extension] enable: 未知插件 %s（未发现，无法启用）\n", name.c_str());
        return false;
    }
    if (info->status == "loaded") return true; // 幂等

    // 从 known 目录重载（load_plugin 内部同步 known_ 状态）
    if (!load_plugin(info)) return false;

    // 移出禁用清单 + persist
    auto disabled = read_disabled();
    auto it = std::find(disabled.begin(), disabled.end(), name);
    if (it != disabled.end()) {
        disabled.erase(it);
        if (!write_disabled(disabled)) {
            // persist 失败：本会话已加载，重启后仍按禁用清单跳过
            fprintf(stderr, "[extension] enable: %s 移出禁用清单失败（配置未持久化）\n",
                    name.c_str());
        }
    }
    return true;
}

bool PluginManager::disable(const std::string& name) {
    PluginInfo* info = find_known(name);
    if (!info) {
        fprintf(stderr, "[extension] disable: 未知插件 %s\n", name.c_str());
        return false;
    }
    if (info->status == "disabled") return true; // 幂等

    // destroy + dlclose + 注销工具/命令（同名多目录 → 全部处理）
    for (auto it = plugins_.begin(); it != plugins_.end();) {
        if ((*it)->name != name) {
            ++it;
            continue;
        }
        unregister_entries(it->get());
        destroy(it->get());
        it = plugins_.erase(it);
    }

    // 加入禁用清单 + persist（failed 插件无实例可销毁，仅登记禁用）
    auto disabled = read_disabled();
    if (std::find(disabled.begin(), disabled.end(), name) == disabled.end()) {
        disabled.push_back(name);
        if (!write_disabled(disabled)) {
            fprintf(stderr, "[extension] disable: %s 加入禁用清单失败（配置未持久化）\n",
                    name.c_str());
            return false;
        }
    }
    mark_disabled(info);
    fprintf(stderr, "[extension] 已禁用: %s\n", name.c_str());
    return true;
}

} // namespace realagent
