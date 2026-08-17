/*
 * slots.hpp — realagent 的宿主词汇（extension 模块）
 *
 * 容器怎么被找到、被打开、按什么顺序初始化，全在 realugin 里（ADR-0001 / ADR-0013）。
 * 这里是 realugin 不该知道的那一半：
 *
 *  - 配置从哪儿来、禁用清单存哪儿、哪些容器免前缀 —— CoreHost 实现 realugin::Host
 *  - 能力键叫什么、什么签名 —— core/sdk/realagent/agent_caps.h
 *  - 工具与命令视图（加前缀、撞名检查）—— 条目怎么命名是 core 的词汇
 *  - 谁是当前 provider、管线四段各由谁来干 —— resolve_slots（ADR-0012）
 *
 * core 不建注册表：工具、命令、模型都是现问现答，此处只保管四段管线的函数指针。
 */
#pragma once

#include <realagent/agent_caps.h>
#include <realugin/host.hpp>
#include <realugin/loader.hpp>

#include <functional>
#include <string>
#include <vector>

#include "../config.hpp"

namespace realagent {

/* 借用 realugin 的名字：调用点关心的是"容器/能力"，不是它住在哪个命名空间 */
using realugin::Cap;
using realugin::cap_of;
using realugin::Plugin;
using realugin::PluginManager;

/* 具名条目的对外视图：现问现答，不存副本。工具与命令只差条目类型，
 * 视图与取法都是同一份。 */
template <class Def>
struct EntryView {
    std::string name;  // 对外名字（带命名空间前缀或短名）
    const Def* def;    // 借阅自插件
    Plugin* owner;
};
/* 前缀怎么拼是 core 的词汇——工具用 '_'（LLM 那边的函数名不许有 ':'），
 * 命令用 ':'（斜杠命令的习惯写法）。realugin 只算出 Plugin::prefix 摆在那儿。 */
using ToolView = EntryView<realagent_tool_t>;
using CommandView = EntryView<realagent_command_t>;

std::vector<ToolView> tools_of(const PluginManager& mgr);
std::vector<CommandView> commands_of(const PluginManager& mgr);

/* 管线四段 + 权限（ADR-0012）。每段一个函数，各自独占。
 * 空 = 没人干这一段：build/parse 空则无法调用 LLM，refine/meter 空则请求原样发出、不计价。 */
struct CapabilitySlots {
    Cap<realagent_request_build_fn> build;
    Cap<realagent_request_refine_fn> refine;
    Cap<realagent_response_parse_fn> parse;
    Cap<realagent_usage_meter_fn> meter;
    Cap<realagent_permission_fn> permission;
};

/* core 运行上下文：配置 + 能力槽 + 事件出口 */
struct CoreContext {
    Config* config = nullptr; // 非 const：禁用清单 persist 需写路径
    CapabilitySlots slots;
    /* 事件的唯一出口。挂上 PluginManager::emit——一份送客户端，一份扇出给订阅插件 */
    std::function<void(const std::string& type, const std::string& payload)> emit_fn;
};

/* realagent 的宿主实现：realugin 每次需要"这个宿主怎么想"都会走到这里 */
class CoreHost : public realugin::Host {
public:
    explicit CoreHost(CoreContext& ctx) : ctx_(ctx) {}

    /* 送客户端的那一路（推送流队列）。不挂就只有插件订阅者收得到事件。 */
    void set_sink(std::function<void(const std::string&, const std::string&)> sink) {
        sink_ = std::move(sink);
    }

    std::vector<std::string> extension_dirs() const override;
    std::string get_config(const realugin::Plugin& p, const char* key) const override;
    void emit(const std::string& type, const std::string& payload) override;
    void log(int level, const char* msg) override;
    std::vector<std::string> disabled() const override;
    bool set_disabled(const std::vector<std::string>& list) override;
    bool unprefixed(const std::string& plugin_name) const override;
    std::vector<std::string> extra_deps(const realugin::Plugin& p) const override;
    bool knows_capability(const char* cap) const override;
    const char* broadcast_capability() const override { return REALAGENT_CAP_EVENT_OBSERVE; }
    std::string validate(const Plugin& p, PluginManager& mgr) const override;
    void on_reload(PluginManager& mgr) override;

private:
    CoreContext& ctx_;
    std::function<void(const std::string&, const std::string&)> sink_;
};

/* 当前 provider 容器（配置 provider 指名者；未配且恰好一个候选则用它） */
Plugin* current_provider(const PluginManager& mgr, const Config& cfg);

/* 当前 provider 的模型清单 JSON（借阅插件内存）；无则空 */
std::string models_json(const PluginManager& mgr, const Config& cfg);

/* 重解析能力槽：管线四段由当前 provider（及其 protocol）一次确定；权限槽取唯一候选。
 * 冲突/缺失只空置该段并点名，不卸载插件——那是配置问题，不是插件坏了。 */
void resolve_slots(CoreContext& ctx, const PluginManager& mgr);

/* 容器清单 → JSON（GET /plugins 的响应契约：字段名与顺序都在这儿定） */
json plugins_json(const std::vector<const Plugin*>& list);

} // namespace realagent
