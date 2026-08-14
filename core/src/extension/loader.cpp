#include "extension/loader.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
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
    auto* owner = static_cast<Plugin*>(core->ctx);
    static std::string slot; // 单线程首版可接受；多线程需换 thread_local
    // 模型数据表路径（ADR-0009）：内容是插件自己的，"去哪儿找"这条规矩归 core——
    // 用户接管版存在就用它，否则用包内出厂版。两者不合并，插件只管读给到的路径。
    if (key && std::strcmp(key, "models_path") == 0) {
        const std::string runtime = ctx->config->models_path(owner->name);
        slot = std::filesystem::exists(runtime)
                   ? runtime
                   : (std::filesystem::path(owner->dir) / "models.json").string();
        return slot.c_str();
    }
    slot = ctx->config->get(key);
    return slot.c_str();
}

plugin_status_t api_depends_on(plugin_core_t* core, const char* plugin_name) {
    auto* p = static_cast<Plugin*>(core->ctx);
    p->deps.emplace_back(plugin_name);
    return PLUGIN_OK;
}

/* get_dependency：嵌套组装（ADR-0004）——外层插件取内层 api + 实例（须已加载） */
plugin_status_t api_get_dependency(plugin_core_t* core, const char* plugin_name,
                                   const plugin_api_t** out_api, plugin_t** out_instance) {
    auto* ctx = ctx_of(core);
    if (!plugin_name || !out_api || !out_instance || !ctx->find_plugin) return PLUGIN_ERR;
    auto* dep = ctx->find_plugin(plugin_name);
    if (!dep || !dep->api || !dep->instance) return PLUGIN_ERR;
    *out_api = dep->api;
    *out_instance = dep->instance;
    return PLUGIN_OK;
}

const plugin_core_api_t k_core_api = {
    .register_tool = api_register_tool,
    .register_command = api_register_command,
    .emit = api_emit,
    .log = api_log,
    .get_config = api_get_config,
    .depends_on = api_depends_on,
    .get_dependency = api_get_dependency,
};

/* plugin_type_t 的名字，下标即枚举值（0 位空着）。枚举是真相，字符串只是它的名字。 */
constexpr std::string_view kTypeNames[] = {"", "protocol", "tool", "permission", "session"};

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

    // 注入按名查已加载插件（api_get_dependency 依赖注入用）
    ctx_.find_plugin = [this](const std::string& name) -> Plugin* { return find(name); };

    // 第一遍：发现全部（登记 known_），按前置依赖序加载。
    // 注意：discover 返回 PluginInfo*，指向 known_ 内部元素；known_ 是 vector，
    // 后续 push_back 会扩容并令悬空指针。故先发现完所有条目（确定 known_ 不再增），
    // 再取稳定指针压入 pending。
    for (const auto& dir : ctx_.config->extension_dirs()) {
        if (!fs::is_directory(dir)) continue;
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_directory()) continue;
            discover(entry.path().string()); // 仅登记 known_，丢弃返回指针
        }
    }
    // known_ 现已稳定（discover 同名复用条目不再 push）——取稳定指针供加载使用。
    // 发现阶段已判失败（plugin.json 元数据不全）的不进 pending：否则 load_plugin
    // 会拿"找不到插件库"覆盖掉真正的失败原因。
    std::vector<PluginInfo*> pending;
    for (auto& k : known_)
        if (k.status != "failed") pending.push_back(&k);

    // 依赖先加载：外层插件 init 中 get_dependency 才能取到内层（嵌套链，ADR-0004）。
    std::vector<std::string> loaded;
    while (!pending.empty()) {
        bool progress = false;
        for (auto it = pending.begin(); it != pending.end();) {
            PluginInfo* info = *it;
            if (std::find(disabled.begin(), disabled.end(), info->name) != disabled.end()) {
                mark_disabled(info); // 禁用清单内：登记为 disabled，不加载
                fprintf(stderr, "[extension] 已跳过（禁用清单）: %s\n", info->name.c_str());
                it = pending.erase(it);
                progress = true;
                continue;
            }
            // 依赖就绪：已加载，或完全未发现（缺失——由 assemble_nested 报告，不阻塞）
            const auto dep_ready = [&](const std::string& d) {
                if (std::find(loaded.begin(), loaded.end(), d) != loaded.end()) return true;
                return find_known(d) == nullptr;
            };
            if (!std::all_of(info->deps.begin(), info->deps.end(), dep_ready)) { ++it; continue; }
            if (load_plugin(info)) loaded.push_back(info->name);
            it = pending.erase(it);
            progress = true;
        }
        if (!progress) { // 依赖环 / 缺失依赖：剩余强制加载，错误由 init/assemble_nested 暴露
            for (auto* info : pending)
                if (load_plugin(info)) loaded.push_back(info->name);
            break;
        }
    }

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
    // 严格解构：缺键即失败，不补空串
    const auto man = strict_from<PluginManifest>(*meta, meta_path.string());

    // 登记条目：同名多目录（项目级+全局级均发现）复用同一条，元数据以最后发现为准。
    // 名字本身可能就是缺的那个键——退回目录名当键，保证这条失败在 /plugins 里看得见。
    const std::string key =
        man ? man->name
            : (*meta)["name"].as_string().value_or(fs::path(plugin_dir).filename().string());
    PluginInfo* info = find_known(key);
    if (info == nullptr) {
        known_.push_back(PluginInfo{});
        info = &known_.back();
    }
    info->name = key;
    info->dir = plugin_dir;

    if (!man) {
        mark_failed(info, man.error());
        return nullptr; // 元数据都不全，加载无从谈起
    }
    info->description = man->description;
    info->version = man->version;
    info->type = man->type;
    info->deps = man->deps;
    info->status.clear(); // 另一目录曾判失败：本次发现合法元数据，清掉旧结论
    info->error.clear();
    return info;
}

bool PluginManager::load_plugin(PluginInfo* info) {
    // 构造插件实例；元数据取自已发现条目（load_all 与 enable 共用的加载主体）
    auto p = std::make_unique<Plugin>();
    p->dir = info->dir;
    p->name = info->name;
    p->description = info->description;
    p->version = info->version;
    p->type_name = info->type;
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
    // 声明 ⇄ 实现：plugin.json 的 type 只用来给人看，调度全看 api->type。两者不符时
    // GET /plugins 会撒谎，而它是排查任何插件问题的起点。越界枚举取空串，同样不相等。
    const std::string_view impl = (unsigned)p->api->type < std::size(kTypeNames)
                                      ? kTypeNames[p->api->type] : std::string_view{};
    if (info->type != impl) {
        mark_failed(info, "plugin.json 声明 type=" + info->type + "，实现是 " + std::string(impl));
        dlclose(p->dl_handle);
        p->dl_handle = nullptr;
        return false;
    }
    // 初始化：core 句柄 ctx 指向 Plugin（api 函数经 Plugin::core_ctx 取 CoreContext）
    p->core_ctx = &ctx_;
    p->core_handle.api = &k_core_api;
    p->core_handle.ctx = p.get();
    // init 失败即加载失败：半死的插件留在链路里，只会让后面每一个症状都对不上号
    if (p->api->init && p->api->init(p->instance, &p->core_handle) != PLUGIN_OK) {
        mark_failed(info, "init 失败（见插件日志）");
        unregister_entries(p.get());
        destroy(p.get());
        return false;
    }
    // 模型清单入册（init 之后：插件此时已读完自己的模型数据表）。
    // 清单坏了就是插件坏了——卸载，不留半个能用的插件在链路里
    if (const auto err = register_models(p.get()); !err.empty()) {
        mark_failed(info, err);
        unregister_entries(p.get());
        destroy(p.get());
        return false;
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
    // 嵌套组装（ADR-0004）：插件声明的 deps 在 plugin.json/init 中已收集，
    // 加载已按拓扑序（内层先）完成。包裹调用由插件自身经 get_dependency 取内层
    // 接口表实现（壳调协议层）。此处仅校验：声明了依赖但目标未加载 → 报告。
    // get_dependency 在外层 init 中已对缺失内层返回 PLUGIN_ERR 并 fail，
    // 这里是兜底日志（针对 init 未自行检查的插件）。
    for (auto& p : plugins_) {
        for (const auto& d : p->deps) {
            if (!find(d)) {
                fprintf(stderr, "[extension] %s: 依赖插件 %s 未找到\n", p->name.c_str(), d.c_str());
            }
        }
    }
}

std::string PluginManager::register_models(Plugin* p) {
    // 模型清单入册（ADR-0009）：数据是插件自己的，此处只收它报上来的公共字段。
    // 不报清单（协议层供应商中立、非协议插件）是正常状态，不是错误。
    if (!p->api->list_models) return {};
    const char* text = p->api->list_models(p->instance); // 内存归插件，core 不释放
    if (!text) return {};
    const auto arr = json::parse(text);
    if (!arr || !arr->is_array())
        return p->name + ": list_models 不是 JSON 数组";
    for (std::size_t i = 0; i < arr->size(); ++i) {
        const auto m = strict_from<Model>((*arr)[i], p->name + ": list_models[" + std::to_string(i) + "]");
        // 一条写坏就整个插件加载失败：模型表是人手写的，打错字当场死好过算错账
        if (!m) return m.error();
        // 重名后写覆盖，不检测——用户嫌烦就自己写一份模型数据表接管
        ctx_.models[m->name] = ModelEntry{*m, p};
    }
    return {};
}

void PluginManager::unregister_entries(const Plugin* p) {
    // 注销该插件注册的 tool / command / model（owner 匹配），其余条目不动
    for (auto it = ctx_.tools.begin(); it != ctx_.tools.end();) {
        if (it->second.owner == p) it = ctx_.tools.erase(it);
        else ++it;
    }
    for (auto it = ctx_.commands.begin(); it != ctx_.commands.end();) {
        if (it->second.owner == p) it = ctx_.commands.erase(it);
        else ++it;
    }
    for (auto it = ctx_.models.begin(); it != ctx_.models.end();) {
        if (it->second.owner == p) it = ctx_.models.erase(it);
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
