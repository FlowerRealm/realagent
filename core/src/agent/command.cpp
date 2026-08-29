#include "agent/command.hpp"

#include <algorithm>

#include "agent/session.hpp"
#include "llm/llm.hpp"

namespace realagent {

std::string command_error(const std::string &msg)
{
    return nlohmann::json{{"ok", false}, {"error", msg}}.dump();
}

/* /model 响应：模型数据表里的清单，current 标出配置里当前那档（ADR-0009）。 */
static nlohmann::json models_payload(const CoreContext &ctx)
{
    const std::string cur = ctx.config->model(ModelTier::Main);
    nlohmann::json arr = nlohmann::json::array();
    for (nlohmann::json m : ctx.pricing->models())
    {
        m["current"] = (m["name"] == cur);
        arr.push_back(std::move(m));
    }
    return arr;
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

std::string cmd_new(Env &e, const std::string &)
{
    e.agent.reset(); // 新建会话：清空历史 + 换一个 JSONL 文件（旧的留在盘上）
    return ok_json("new", sessions_payload(e.pool, e.agent));
}

std::string cmd_resume(Env &e, const std::string &arg)
{
    // 无参 = 列会话（清单里 opened_by 标出自己在哪儿）；带 id = 恢复那一个。
    // 恢复失败保持原会话不动：宁可这条命令没生效，也不能把人扔进一段空白历史
    if (!arg.empty() && !e.agent.resume(arg)) return command_error("unknown session: " + arg);
    return ok_json("resume", sessions_payload(e.pool, e.agent));
}

std::string cmd_model(Env &e, const std::string &arg)
{
    // 无参 = 列清单；带名 = 切主模型（写回 settings.json，下一次调用即生效）。
    // 只认数据表里的模型：交互式选择就该从已知的里挑，打字选中不存在的
    // 只会得到一个端点 400。启动时不校验配置是另一回事（ADR-0009）。
    if (!arg.empty())
    {
        bool known = false;
        for (const nlohmann::json &m : models_payload(e.ctx))
            if (m["name"] == arg) known = true;
        if (!known) return command_error("unknown model: " + arg);
        // 点对点写：只改文件里的 model 这一个键。statusline 帧不在这里推——
        // 事件循环发现载荷变了自己会推（见 main.cpp 的 on_tick）
        if (!e.ctx.config->persist("model", nlohmann::json(arg)))
            return command_error("写入 settings.json 失败");
    }
    return ok_json("model", models_payload(e.ctx));
}

struct CommandDef {
    const char *name; // 不带前导 '/'
    const char *description;
    std::string (*run)(Env &, const std::string &arg);
};

/* 全部命令。清单与派发都读这张表，加一条命令就是加一行。 */
constexpr CommandDef kCommands[] = {
    {"new", "新建会话（清空当前对话，旧会话留在盘上）", cmd_new},
    {"resume", "查看会话列表（/resume <id> 恢复某个会话）", cmd_resume},
    {"model", "查看模型清单（/model <name> 切换主模型）", cmd_model},
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
                           const std::string &input)
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
        if (cmd.compare(1, std::string::npos, c.name) == 0) return c.run(env, arg);
    return command_error("unknown command: " + cmd);
}

} // namespace realagent
