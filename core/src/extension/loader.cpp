#include "extension/loader.hpp"

#include <dlfcn.h>

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

} // namespace

PluginManager::PluginManager(CoreContext& ctx) : ctx_(ctx) {}

PluginManager::~PluginManager() { shutdown(); }

int PluginManager::load_all() {
    int failures = 0;
    for (const auto& dir : ctx_.config->extension_dirs()) load_one_dir(dir);
    assemble_nested();
    // 统计失败：加载数 vs 期望数（首版不做严格计数，失败已打日志）
    return failures;
}

void PluginManager::load_one_dir(const std::string& dir_path) {
    if (!fs::is_directory(dir_path)) return;
    for (const auto& entry : fs::directory_iterator(dir_path)) {
        if (!entry.is_directory()) continue;
        load_plugin(entry.path().string());
    }
}

void PluginManager::load_plugin(const std::string& plugin_dir) {
    // 解析 plugin.json
    const auto meta_path = fs::path(plugin_dir) / "plugin.json";
    std::ifstream f(meta_path);
    if (!f) return; // 无 plugin.json 的目录跳过
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto meta = json::parse(text);
    if (!meta) {
        fprintf(stderr, "[extension] 解析 plugin.json 失败: %s\n", plugin_dir.c_str());
        return;
    }
    const json& m = *meta; // 用链式 operator[]（读缺键返回 null，不抛异常）
    const auto name = m["name"].as_string();
    if (!name) return;

    auto p = std::make_unique<Plugin>();
    p->dir = plugin_dir;
    p->name = *name;
    p->description = m["description"].as_string().value_or("");
    p->version = m["version"].as_string().value_or("0");
    p->type_name = m["type"].as_string().value_or("");
    if (const json deps = m["deps"]; deps.is_array()) {
        for (const auto& d : deps.as_array()) {
            if (auto s = json(d).as_string()) p->deps.push_back(*s);
        }
    }

    // dlopen 插件库（.dylib 优先，次 .so）
    std::string lib_path;
    for (const auto& cand : {fs::path(plugin_dir) / (name->c_str() + std::string(".dylib")),
                             fs::path(plugin_dir) / (name->c_str() + std::string(".so"))}) {
        if (fs::exists(cand)) { lib_path = cand.string(); break; }
    }
    if (lib_path.empty()) {
        fprintf(stderr, "[extension] %s: 找不到插件库 .dylib/.so\n", name->c_str());
        return;
    }
    p->dl_handle = dlopen(lib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!p->dl_handle) {
        fprintf(stderr, "[extension] %s: dlopen 失败: %s\n", name->c_str(), dlerror());
        return;
    }
    auto create_fn = reinterpret_cast<plugin_create_fn>(dlsym(p->dl_handle, PLUGIN_CREATE_SYM));
    if (!create_fn) {
        fprintf(stderr, "[extension] %s: 缺少导出符号 %s\n", name->c_str(), PLUGIN_CREATE_SYM);
        dlclose(p->dl_handle);
        return;
    }
    p->instance = create_fn(&p->api);
    if (!p->instance || !p->api) {
        fprintf(stderr, "[extension] %s: plugin_create 失败\n", name->c_str());
        if (p->dl_handle) dlclose(p->dl_handle);
        return;
    }
    // ABI 强校验
    if (p->api->abi_version != PLUGIN_ABI_VERSION) {
        fprintf(stderr, "[extension] %s: ABI 版本不符 (插件 %d, core %d)，请重编插件\n",
                name->c_str(), p->api->abi_version, PLUGIN_ABI_VERSION);
        dlclose(p->dl_handle);
        p->dl_handle = nullptr;
        return;
    }
    // 初始化：core 句柄 ctx 指向 Plugin（api 函数经 Plugin::core_ctx 取 CoreContext）
    p->core_ctx = &ctx_;
    p->core_handle.api = &k_core_api;
    p->core_handle.ctx = p.get();
    if (p->api->init) {
        if (p->api->init(p->instance, &p->core_handle) != PLUGIN_OK) {
            fprintf(stderr, "[extension] %s: init 失败\n", name->c_str());
        }
    }
    fprintf(stderr, "[extension] 已加载: %s (%s) v%s\n", p->name.c_str(), p->type_name.c_str(),
            p->version.c_str());
    plugins_.push_back(std::move(p));
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

void PluginManager::shutdown() {
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
        auto& p = *it;
        if (p->instance && p->api && p->api->destroy) p->api->destroy(p->instance);
        if (p->dl_handle) dlclose(p->dl_handle);
        p->instance = nullptr;
        p->dl_handle = nullptr;
    }
    plugins_.clear();
}

Plugin* PluginManager::find(const std::string& name) const {
    for (const auto& p : plugins_)
        if (p->name == name) return p.get();
    return nullptr;
}

} // namespace realagent
