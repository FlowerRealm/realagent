#include "extension/slots.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "json.hpp"

namespace realagent {
namespace fs = std::filesystem;

namespace {

/* core 认识的能力键。不在表里的键 core 不解释（插件之间的私约），
 * 但加载期会点名——写错一个字母就等于能力静默消失，这是能力表方案唯一的软肋。 */
constexpr const char* kKnownCaps[] = {
    REALAGENT_CAP_REQUEST_BUILD, REALAGENT_CAP_REQUEST_REFINE,  REALAGENT_CAP_RESPONSE_PARSE,
    REALAGENT_CAP_USAGE_METER,   REALAGENT_CAP_TOOL_LIST,       REALAGENT_CAP_TOOL_EXECUTE,
    REALAGENT_CAP_TOOL_INTERRUPT, REALAGENT_CAP_COMMAND_LIST,   REALAGENT_CAP_COMMAND_EXECUTE,
    REALAGENT_CAP_PERMISSION,    REALAGENT_CAP_MODEL_LIST,      REALAGENT_CAP_EVENT_OBSERVE,
};

/* GET /plugins 的响应契约：字段名与顺序就是这张结构体声明。
 * 它住在 core 这边——realugin 的 PluginInfo 是加载器的内部快照，不该替宿主定响应格式。 */
struct PluginWire {
    std::string name;
    std::string version;
    std::vector<std::string> capabilities;
    std::string description;
    std::string dir;
    std::string status;
    std::string error;
    std::vector<std::string> deps;
};
BOOST_DESCRIBE_STRUCT(PluginWire, (),
                      (name, version, capabilities, description, dir, status, error, deps))

} // namespace

/* —— 具名条目的对外视图：现问现答，不留副本 —— */

std::vector<ToolView> tools_of(const PluginManager& mgr) {
    std::vector<ToolView> out;
    for (const auto& p : mgr.plugins()) {
        auto list = cap_of<realagent_tool_list_fn>(*p, REALAGENT_CAP_TOOL_LIST);
        if (!list) continue;
        const realagent_tool_t* defs = nullptr;
        const size_t n = list.fn(list.self, &defs);
        for (size_t i = 0; i < n && defs; ++i) {
            const std::string base = defs[i].name ? defs[i].name : "";
            // 工具名要能进 LLM 的函数名位，分隔符只能用 '_'
            out.push_back(
                ToolView{p->prefix.empty() ? base : p->prefix + "_" + base, &defs[i], p.get()});
        }
    }
    return out;
}

std::vector<CommandView> commands_of(const PluginManager& mgr) {
    std::vector<CommandView> out;
    for (const auto& p : mgr.plugins()) {
        auto list = cap_of<realagent_command_list_fn>(*p, REALAGENT_CAP_COMMAND_LIST);
        if (!list) continue;
        const realagent_command_t* defs = nullptr;
        const size_t n = list.fn(list.self, &defs);
        for (size_t i = 0; i < n && defs; ++i) {
            const std::string base = defs[i].name ? defs[i].name : "";
            // 斜杠命令的习惯写法是 <容器>:<命令>
            out.push_back(
                CommandView{p->prefix.empty() ? base : p->prefix + ":" + base, &defs[i], p.get()});
        }
    }
    return out;
}

json plugins_json(const std::vector<PluginInfo>& list) {
    std::vector<PluginWire> wire;
    wire.reserve(list.size());
    for (const auto& i : list)
        wire.push_back(PluginWire{i.name, i.version, i.capabilities, i.description, i.dir,
                                  realugin::to_string(i.status), i.error, i.deps});
    return json(to_json(wire));
}

/* —— CoreHost：realugin 问，core 答 —— */

std::vector<std::string> CoreHost::extension_dirs() const {
    return ctx_.config->extension_dirs();
}

std::string CoreHost::get_config(const realugin::Plugin& p, const char* key) const {
    // 模型数据表路径（ADR-0009）：内容是插件自己的，"去哪儿找"这条规矩归 core——
    // 用户接管版存在就用它，否则用包内出厂版。两者不合并。
    if (key && std::string_view(key) == "models_path") {
        const std::string runtime = ctx_.config->models_path(p.name);
        return fs::exists(runtime) ? runtime : (fs::path(p.dir) / "models.json").string();
    }
    return ctx_.config->get(key ? key : "");
}

void CoreHost::emit(const std::string& type, const std::string& payload) {
    if (sink_) sink_(type, payload);
}

void CoreHost::log(int level, const char* msg) {
    const char* lv = level <= REALUGIN_LOG_DEBUG ? "debug"
                     : level == REALUGIN_LOG_INFO ? "info"
                     : level == REALUGIN_LOG_WARN ? "warn"
                                                  : "error";
    fprintf(stderr, "[extension][%s] %s\n", lv, msg ? msg : "");
}

std::vector<std::string> CoreHost::disabled() const {
    std::vector<std::string> out;
    const json d = ctx_.config->to_json()["plugins"]["disabled"];
    if (d.is_array())
        for (const auto& v : d.as_array())
            if (auto s = json(v).as_string()) out.push_back(*s);
    return out;
}

bool CoreHost::set_disabled(const std::vector<std::string>& list) {
    // 点分路径：只写 plugins.disabled 这一个叶子，兄弟键 plugins.unprefixed 原样保留
    json arr = json::array();
    for (const auto& n : list) arr.push_back(n);
    return ctx_.config->persist("plugins.disabled", arr);
}

bool CoreHost::unprefixed(const std::string& plugin_name) const {
    const json list = ctx_.config->to_json()["plugins"]["unprefixed"];
    if (!list.is_array()) return false;
    for (const auto& v : list.as_array())
        if (json(v).as_string().value_or("") == plugin_name) return true;
    return false;
}

std::vector<std::string> CoreHost::extra_deps(const realugin::Manifest& m) const {
    // protocol 是 Provider 壳专有的可选键：它绑定哪个协议容器，自带一条依赖边。
    // 交给 realugin 当普通 deps 用，图上就不留暗边。
    const auto it = m.extra.find("protocol");
    if (it == m.extra.end() || it->second.empty()) return {};
    return {it->second};
}

bool CoreHost::knows_capability(const char* cap) const {
    return std::any_of(std::begin(kKnownCaps), std::end(kKnownCaps),
                       [&](const char* k) { return std::string_view(k) == cap; });
}

/* 撞名检查：条目叫什么、怎么算撞，是 core 的词汇——realugin 不掺和。
 * 每 init 完一个容器查一次，拉一次清单、只校验、结果丢弃（校验不是副本）。 */
std::string CoreHost::validate(const Plugin& p, PluginManager& mgr) const {
    (void)p;
    std::vector<std::string> seen;
    for (const auto& t : tools_of(mgr)) {
        if (std::find(seen.begin(), seen.end(), t.name) != seen.end())
            return "工具名撞车: " + t.name;
        seen.push_back(t.name);
    }
    seen.clear();
    for (const auto& c : commands_of(mgr)) {
        if (std::find(seen.begin(), seen.end(), c.name) != seen.end())
            return "命令名撞车: " + c.name;
        seen.push_back(c.name);
    }
    return {};
}

void CoreHost::on_reload(PluginManager& mgr) { resolve_slots(ctx_, mgr); }

/* —— 谁干哪段活：core 的词汇，realugin 不掺和 —— */

Plugin* current_provider(const PluginManager& mgr, const Config& cfg) {
    const auto& cands = mgr.providers_of(REALAGENT_CAP_REQUEST_REFINE);
    const std::string want = cfg.get("provider");
    if (!want.empty()) {
        if (std::find(cands.begin(), cands.end(), want) != cands.end()) return mgr.find(want);
        fprintf(stderr, "[extension] 配置 provider=%s 不在候选里（未加载 / 不提供 %s）\n",
                want.c_str(), REALAGENT_CAP_REQUEST_REFINE);
        return nullptr;
    }
    if (cands.size() == 1) return mgr.find(cands.front());
    if (cands.size() > 1) {
        std::string names;
        for (const auto& n : cands) names += (names.empty() ? "" : " / ") + n;
        fprintf(stderr, "[extension] 多个 provider 候选（%s）且未配置 provider，请指名\n",
                names.c_str());
    }
    return nullptr;
}

std::string models_json(const PluginManager& mgr, const Config& cfg) {
    Plugin* prov = current_provider(mgr, cfg);
    if (!prov) return {};
    auto list = cap_of<realagent_model_list_fn>(*prov, REALAGENT_CAP_MODEL_LIST);
    if (!list) return {};
    const char* text = list.fn(list.self); // 借阅：读完即用，不释放
    return text ? std::string(text) : std::string{};
}

void resolve_slots(CoreContext& ctx, const PluginManager& mgr) {
    ctx.slots = CapabilitySlots{};

    // —— 管线四段：由当前 provider 一次确定，零推导（ADR-0012）——
    if (Plugin* prov = current_provider(mgr, *ctx.config)) {
        ctx.slots.refine = cap_of<realagent_request_refine_fn>(*prov, REALAGENT_CAP_REQUEST_REFINE);
        ctx.slots.meter = cap_of<realagent_usage_meter_fn>(*prov, REALAGENT_CAP_USAGE_METER);
        const std::string proto_name = prov->meta("protocol");
        if (Plugin* proto = proto_name.empty() ? nullptr : mgr.find(proto_name)) {
            ctx.slots.build =
                cap_of<realagent_request_build_fn>(*proto, REALAGENT_CAP_REQUEST_BUILD);
            ctx.slots.parse =
                cap_of<realagent_response_parse_fn>(*proto, REALAGENT_CAP_RESPONSE_PARSE);
        } else if (!proto_name.empty()) {
            fprintf(stderr, "[extension] provider %s 的 protocol=%s 未加载，管线前后两段空置\n",
                    prov->name.c_str(), proto_name.c_str());
        }
    }
    // 无 provider（或它没绑协议）：协议容器唯一时直接用它——裸协议直连是合法用法，
    // 端点与凭证得用户自己在配置里写全
    if (!ctx.slots.build) {
        const auto& c = mgr.providers_of(REALAGENT_CAP_REQUEST_BUILD);
        if (c.size() == 1) {
            if (Plugin* p = mgr.find(c.front())) {
                ctx.slots.build = cap_of<realagent_request_build_fn>(*p, REALAGENT_CAP_REQUEST_BUILD);
                ctx.slots.parse =
                    cap_of<realagent_response_parse_fn>(*p, REALAGENT_CAP_RESPONSE_PARSE);
            }
        } else if (c.size() > 1) {
            fprintf(stderr, "[extension] 多个容器提供 %s 且无 provider 指名，该段空置\n",
                    REALAGENT_CAP_REQUEST_BUILD);
        }
    }

    // —— 权限槽：独占。多个候选即空置并点名，不猜 ——
    if (const auto& c = mgr.providers_of(REALAGENT_CAP_PERMISSION); c.size() == 1) {
        if (Plugin* p = mgr.find(c.front()))
            ctx.slots.permission = cap_of<realagent_permission_fn>(*p, REALAGENT_CAP_PERMISSION);
    } else if (c.size() > 1) {
        std::string names;
        for (const auto& n : c) names += (names.empty() ? "" : " / ") + n;
        fprintf(stderr, "[extension] 权限槽冲突: %s 同时提供裁决，该槽空置"
                        "（请用 plugins.disabled 禁掉其中之一）\n", names.c_str());
    }

    const auto who = [](const auto& cap) { return cap ? cap.owner->name.c_str() : "-"; };
    fprintf(stderr, "[extension] 管线: build=%s refine=%s parse=%s meter=%s | permission=%s\n",
            who(ctx.slots.build), who(ctx.slots.refine), who(ctx.slots.parse),
            who(ctx.slots.meter), who(ctx.slots.permission));
}

} // namespace realagent
