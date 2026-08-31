#include "agent/command.hpp"

#include <algorithm>

#include "agent/session.hpp"
#include "llm/llm.hpp"

namespace realagent {

std::string command_error(const std::string &msg)
{
    return nlohmann::json{{"ok", false}, {"error", msg}}.dump();
}

/* /provider 与 /model 共用的载荷：那一束端点配置，外加补好元数据的模型清单。
 *
 * **两条命令回同一份东西**，只是客户端拿它渲染两个不同的面板：/provider 渲染成
 * 一张表单，/model 渲染成清单加二级菜单。分成两份的话，/model 那头就看不见
 * provider 的其余字段，也就拼不出完整的目标状态发回来。
 *
 * `models` 的候选出自 provider 自己的 models（ADR-0023）——那回答的是
 * "这条路径上有哪些模型"。[[模型数据表]]只负责给挑中的名字补元数据，查不到就只剩名字：
 * 它是参考资料，不是白名单（ADR-0009）。current / small 标出两档各选中了谁。
 *
 * **api_key 原样在里面**：客户端要拼出完整的目标状态发回来，拿不到旧值就会把它清空。
 * 判据是同用户同机器——它本来就明文躺在 ~/.realagent/settings.json 里。
 * core 哪天跨机器服务，这条第一个作废。 */
static nlohmann::json config_payload(const CoreContext &ctx)
{
    const nlohmann::json prov = ctx.config->provider();
    const std::string cur = ctx.config->model(ModelTier::Main);
    const std::string small = ctx.config->model(ModelTier::Small);

    nlohmann::json models = nlohmann::json::array();
    for (const nlohmann::json &n : prov.value("models", nlohmann::json::array()))
    {
        if (!n.is_string()) continue;
        const std::string name = n.get<std::string>();
        nlohmann::json e{{"name", name}, {"current", name == cur}, {"small", name == small}};
        for (const nlohmann::json &m : ctx.pricing->models())
        {
            if (m["name"] != name) continue;
            e["owned_by"] = m["owned_by"];
            e["context"] = m["context"];
            break;
        }
        models.push_back(std::move(e));
    }
    // protocols 是给客户端渲染选项用的：名单在 core 的 kProtocols 里，客户端不留第二份
    return nlohmann::json{
        {"provider", prov}, {"models", std::move(models)}, {"protocols", protocol_names()}};
}

/* 打开着的会话可能一条消息都还没有（文件尚未落地），此时它不在扫描结果里——
 * 补一条空的进去，客户端才看得到自己在哪儿。 */
nlohmann::json sessions_payload(const Agents &pool, const Agent &agent)
{
    const std::map<std::string, int> opened = pool.openers();
    nlohmann::json arr = nlohmann::json::array();
    bool seen_self = false;
    for (const auto &s : Session::list(agent.session_dir()))
    {
        nlohmann::json e = s;
        const auto it = opened.find(s.id);
        e["opened_by"] = it != opened.end() ? nlohmann::json(it->second) : nlohmann::json();
        seen_self = seen_self || s.id == agent.session_id();
        arr.push_back(std::move(e));
    }
    if (!seen_self)
    {
        nlohmann::json e = SessionInfo{.id = agent.session_id()};
        e["opened_by"] = agent.id();
        arr.push_back(std::move(e)); // 新会话还没写过盘，排在最前（它最新）
        std::rotate(arr.begin(), arr.end() - 1, arr.end());
    }
    return arr;
}

/* 查不到就只回名字——模型清单是参考资料，不是白名单（ADR-0009）。 */
nlohmann::json statusline_payload(const CoreContext &ctx)
{
    const std::string name = ctx.config->model(ModelTier::Main);
    nlohmann::json out;
    out["model"] = name;
    for (const nlohmann::json &m : ctx.pricing->models())
    {
        if (m["name"] != name) continue;
        out["owned_by"] = m["owned_by"];
        out["context"] = m["context"];
        break;
    }
    return out;
}

namespace {

/* 一条命令跑起来要够到的三样东西 */
struct Env {
    CoreContext &ctx;
    Agents &pool;
    Agent &agent;
};

/* 成功载荷：{"ok":true,"command":name,"data":data} */
std::string ok_json(const char *name, nlohmann::json data)
{
    return nlohmann::json{{"ok", true}, {"command", name}, {"data", std::move(data)}}.dump();
}

/* 客户端发回完整的那一束，core 原样落盘——不合并、不重算、不校验一遍。
 *
 * 校验没了不是漏了：**这个项目里没有任何一处校验配置齐不齐**。缺了什么就让它
 * 以本来的方式失败——空 base_url 换回 libcurl 一句 URL 格式错，端点不认的模型名
 * 换回一个 400。多一道校验就多一份会跟真正的失败漂移的真相。
 *
 * 只认 provider 这一个键：客户端发的是那一束端点配置，不是整棵配置树。
 * 放开的话 /provider 就能顺手改掉 permission——那是安全默认，不归这条命令管。 */
std::string apply_config(Env &e, const nlohmann::json &data)
{
    const auto it = data.find("provider");
    if (it == data.end()) return command_error("载荷里没有 provider");
    if (!e.ctx.config->persist("provider", *it)) return command_error("写入 settings.json 失败");
    return {};
}

std::string cmd_new(Env &e, const std::string &, const nlohmann::json &)
{
    e.agent.reset(); // 新建会话：清空历史 + 换一个 JSONL 文件（旧的留在盘上）
    return ok_json("new", sessions_payload(e.pool, e.agent));
}

std::string cmd_resume(Env &e, const std::string &arg, const nlohmann::json &)
{
    // 无参 = 列会话（清单里 opened_by 标出自己在哪儿）；带 id = 恢复那一个。
    // 恢复失败保持原会话不动：宁可这条命令没生效，也不能把人扔进一段空白历史
    if (!arg.empty() && !e.agent.resume(arg)) return command_error("unknown session: " + arg);
    return ok_json("resume", sessions_payload(e.pool, e.agent));
}

/* /provider —— 两种形态：带 data = 覆盖那一束，不带 = 把它回给客户端渲染表单。
 *
 * 没有"新增 / 删除 / 切换"这些动作：**盘上就一份 provider**，换后端就是把这一束
 * 的字段改掉。少了那三个动作，也就少了三处分支和一层列表界面。 */
std::string cmd_provider(Env &e, const std::string &, const nlohmann::json &data)
{
    if (data.is_object())
        if (const std::string err = apply_config(e, data); !err.empty()) return err;
    return ok_json("provider", config_payload(e.ctx));
}

/* /model —— 与 /provider 同一份载荷、同一条写回路径，只是客户端拿它渲染模型面板。
 *
 * `/model <name>` 是本次改动之前就有的用法，保留：它只带一个模型名，
 * 拼不出完整的一束，所以这一条由 core 就地改 provider 里的 model。 */
std::string cmd_model(Env &e, const std::string &arg, const nlohmann::json &data)
{
    if (data.is_object())
    {
        if (const std::string err = apply_config(e, data); !err.empty()) return err;
    }
    else if (!arg.empty())
    {
        nlohmann::json prov = e.ctx.config->provider();
        if (prov.empty()) return command_error("还没配 provider——先用 /provider 配一个");
        // 这条路径 core 本来就在读那份 provider，顺手看一眼清单几乎不要钱；
        // 而它在本次改动之前就会拒绝清单外的模型名，别把已有的行为改掉
        bool known = false;
        for (const nlohmann::json &m : prov.value("models", nlohmann::json::array()))
            known = known || m == arg;
        if (!known) return command_error("unknown model: " + arg);
        prov["model"] = arg;
        if (const std::string err = apply_config(e, nlohmann::json{{"provider", prov}});
            !err.empty())
            return err;
    }
    return ok_json("model", config_payload(e.ctx));
}

struct CommandDef {
    const char *name; // 不带前导 '/'
    const char *description;
    std::string (*run)(Env &, const std::string &arg, const nlohmann::json &data);
};

/* 全部命令。清单与派发都读这张表，加一条命令就是加一行。 */
constexpr CommandDef kCommands[] = {
    {"new", "新建会话（清空当前对话，旧会话留在盘上）", cmd_new},
    {"resume", "查看会话列表（/resume <id> 恢复某个会话）", cmd_resume},
    {"model", "查看模型清单（/model <name> 切换主模型）", cmd_model},
    {"provider", "配置模型后端（端点、凭证）", cmd_provider},
};

} // namespace

nlohmann::json command_defs()
{
    nlohmann::json arr = nlohmann::json::array();
    for (const CommandDef &c : kCommands)
        arr.push_back(nlohmann::json{{"name", c.name}, {"description", c.description}});
    return arr;
}

std::string handle_command(CoreContext &ctx, Agents &pool, Agent &agent,
                           const std::string &input, const nlohmann::json &data)
{
    auto lk = agent.try_lock();
    if (!lk.owns_lock()) return command_error(AGENT_BUSY);

    // 首空白分词为命令名：/resume[ <id>]、/model[ <name>]
    const std::string cmd = input.substr(0, input.find(' '));
    // 命令参数：命令名之后去掉尾部空白的那一段（无参即空串）
    std::string arg = input.size() > cmd.size() ? input.substr(cmd.size() + 1) : std::string();
    while (!arg.empty() && arg.back() == ' ') arg.pop_back();

    Env env{ctx, pool, agent};
    for (const CommandDef &c : kCommands)
        if (cmd.compare(1, std::string::npos, c.name) == 0) return c.run(env, arg, data);
    return command_error("unknown command: " + cmd);
}

} // namespace realagent
