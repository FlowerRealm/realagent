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
    PLUGIN_CAP_REQUEST_BUILD, PLUGIN_CAP_REQUEST_REFINE, PLUGIN_CAP_RESPONSE_PARSE,
    PLUGIN_CAP_USAGE_METER,   PLUGIN_CAP_TOOL_LIST,      PLUGIN_CAP_TOOL_EXECUTE,
    PLUGIN_CAP_TOOL_INTERRUPT,
    PLUGIN_CAP_COMMAND_LIST,  PLUGIN_CAP_COMMAND_EXECUTE, PLUGIN_CAP_PERMISSION,
    PLUGIN_CAP_MODEL_LIST,    PLUGIN_CAP_EVENT_OBSERVE,
};

} // namespace

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

void CoreHost::on_reload(PluginManager& mgr) { resolve_slots(ctx_, mgr); }

/* —— 谁干哪段活：core 的词汇，realugin 不掺和 —— */

Plugin* current_provider(const PluginManager& mgr, const Config& cfg) {
    const auto& cands = mgr.providers_of(PLUGIN_CAP_REQUEST_REFINE);
    const std::string want = cfg.get("provider");
    if (!want.empty()) {
        if (std::find(cands.begin(), cands.end(), want) != cands.end()) return mgr.find(want);
        fprintf(stderr, "[extension] 配置 provider=%s 不在候选里（未加载 / 不提供 %s）\n",
                want.c_str(), PLUGIN_CAP_REQUEST_REFINE);
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
    auto list = cap_of<plugin_model_list_fn>(*prov, PLUGIN_CAP_MODEL_LIST);
    if (!list) return {};
    const char* text = list.fn(list.self); // 借阅：读完即用，不释放
    return text ? std::string(text) : std::string{};
}

void resolve_slots(CoreContext& ctx, const PluginManager& mgr) {
    ctx.slots = CapabilitySlots{};

    // —— 管线四段：由当前 provider 一次确定，零推导（ADR-0012）——
    if (Plugin* prov = current_provider(mgr, *ctx.config)) {
        ctx.slots.refine = cap_of<plugin_request_refine_fn>(*prov, PLUGIN_CAP_REQUEST_REFINE);
        ctx.slots.meter = cap_of<plugin_usage_meter_fn>(*prov, PLUGIN_CAP_USAGE_METER);
        const std::string proto_name = prov->meta("protocol");
        if (Plugin* proto = proto_name.empty() ? nullptr : mgr.find(proto_name)) {
            ctx.slots.build = cap_of<plugin_request_build_fn>(*proto, PLUGIN_CAP_REQUEST_BUILD);
            ctx.slots.parse = cap_of<plugin_response_parse_fn>(*proto, PLUGIN_CAP_RESPONSE_PARSE);
        } else if (!proto_name.empty()) {
            fprintf(stderr, "[extension] provider %s 的 protocol=%s 未加载，管线前后两段空置\n",
                    prov->name.c_str(), proto_name.c_str());
        }
    }
    // 无 provider（或它没绑协议）：协议容器唯一时直接用它——裸协议直连是合法用法，
    // 端点与凭证得用户自己在配置里写全
    if (!ctx.slots.build) {
        const auto& c = mgr.providers_of(PLUGIN_CAP_REQUEST_BUILD);
        if (c.size() == 1) {
            if (Plugin* p = mgr.find(c.front())) {
                ctx.slots.build = cap_of<plugin_request_build_fn>(*p, PLUGIN_CAP_REQUEST_BUILD);
                ctx.slots.parse = cap_of<plugin_response_parse_fn>(*p, PLUGIN_CAP_RESPONSE_PARSE);
            }
        } else if (c.size() > 1) {
            fprintf(stderr, "[extension] 多个容器提供 %s 且无 provider 指名，该段空置\n",
                    PLUGIN_CAP_REQUEST_BUILD);
        }
    }

    // —— 权限槽：独占。多个候选即空置并点名，不猜 ——
    if (const auto& c = mgr.providers_of(PLUGIN_CAP_PERMISSION); c.size() == 1) {
        if (Plugin* p = mgr.find(c.front()))
            ctx.slots.permission = cap_of<plugin_permission_fn>(*p, PLUGIN_CAP_PERMISSION);
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
