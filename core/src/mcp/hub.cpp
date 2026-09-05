/*
 * hub.cpp — 读配置、起连接、拼工具表
 *
 * 这个文件里没有一行协议。协议在 client.cpp，这里只回答三件事：
 * 有哪些 server、哪些已经连着、模型看见的那张表长什么样。
 */
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>

#include "config.hpp"
#include "mcp/mcp.hpp"

namespace realagent {

namespace fs = std::filesystem;

namespace {

/* 工具名里的非法字符换成 `_`（端点认的字符类是 `[a-zA-Z0-9_-]`，考据见 ADR-0023）。
 *
 * **只换字符，不截断。** 两件事的判据不同：长度那一半是用户写的配置键，他改得动，
 * 撞上就让请求爆、端点会指名道姓；字符是 server 起的名字，他一个字都改不了，
 * 而一个这样的 server 能让每一次请求都 400。
 *
 * 替换必须是确定性的：这个名字要永久写进会话记录，不能随算法或长度变。 */
std::string sanitize_tool_name(std::string s)
{
    std::replace_if(
        s.begin(), s.end(),
        [](char c) { return !(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'); },
        '_');
    return s;
}

/* 一个条目 → 一条归一过的启动规格（只剩 name / command / args / env 四个键）。
 * 未知键在这里落下：那份对象 dump 出来就是连接的键，留着一个被忽略的 `timeout`，
 * 同一个 server 会因为它起两个进程。
 *
 * **失败只有一个出口**：类型上的错由 json 库抛（`key 'command' not found`、
 * `type must be string, but is number`——自带键名与实际类型，比手写的准），
 * 业务上的错我们自己抛，同一个 catch 接住。err 空 = 用户明确关的，不是错。 */
std::optional<nlohmann::json> parse_entry(const std::string &name, const nlohmann::json &e,
                                          std::string &err)
{
    /* 每个进规格的字符串都过这一道。别家的模板变量（`${workspaceFolder}` /
     * `${CLAUDE_PROJECT_DIR}`）不展开——ADR-0010 明写「没有 env 那一层」。
     * 认出来点名跳过，比安静地传一个字面量强。 */
    const auto str = [](const nlohmann::json &v) {
        auto s = v.get<std::string>();
        if (s.find("${") != std::string::npos)
            throw std::runtime_error("`" + s +
                                     "` 里有 ${...}——那是别家客户端的变量，本项目不展开"
                                     "（连接是进程级的，不属于任何目录）。那个参数是这个 server "
                                     "被允许触碰的范围，请改成绝对路径：写死它就是明确授一次权");
        return s;
    };
    try
    {
        if (!e.value("enabled", true)) return std::nullopt; // `{"enabled": false}` = 关掉

        /* type 认它，是为了把「我不支持」和「你配错了」分开说——不认的话，一份 http
         * 配置会让 core 拿一个不存在的 command 去 fork，报的错跟真正的原因隔着十万八千里。 */
        if (const auto t = e.value("type", std::string("stdio")); t != "stdio")
            throw std::runtime_error("type=\"" + t + "\" 暂不支持（本版只有 stdio）");

        const auto command = str(e.at("command"));
        if (command.empty()) throw std::runtime_error("command 是空串");
        nlohmann::json c{{"name", name},
                         {"command", command},
                         {"args", nlohmann::json::array()},
                         {"env", nlohmann::json::object()}};
        for (const auto &one : e.value("args", nlohmann::json::array()))
            c["args"].push_back(str(one));
        for (const auto &[k, v] : e.value("env", nlohmann::json::object()).items())
            c["env"][k] = str(v);
        return c;
    } catch (const std::exception &ex)
    {
        // core 是常驻服务：这里不接住的话，一份手滑的配置能带走所有 agent
        err = name + ": " + ex.what();
        return std::nullopt;
    }
}

/* 读一份文件里的 mcpServers，合进 out。文件不存在 → 什么都不做（多数人一个都没配）。
 * 形状不对 → 记一条错，不拒绝启动（`models.json` 那条先例）。
 *
 * `value("mcpServers", {})` 一句办三件事：缺键当空的、类型不对就抛、拿到就是个对象。 */
void read_file(const fs::path &path, std::map<std::string, nlohmann::json> &out,
               std::vector<std::string> &errors)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return;
    std::ifstream f(path);
    if (!f)
    {
        errors.push_back(path.string() + " 打不开");
        return;
    }
    nlohmann::json servers;
    try
    {
        servers = nlohmann::json::parse(f).value("mcpServers", nlohmann::json::object());
    } catch (const nlohmann::json::exception &ex)
    {
        errors.push_back(path.string() + ": " + ex.what()); // 库的原话带行号列号
        return;
    }
    for (const auto &[name, entry] : servers.items())
    {
        std::string err;
        if (auto c = parse_entry(name, entry, err))
            out[name] = std::move(*c); // 同名整条覆盖：近的那份把远的整条换掉
        else
        {
            // 关掉的和坏掉的动作是同一个：抹掉。近的那份坏了，远的也不该顶上
            out.erase(name);
            if (!err.empty()) errors.push_back(path.string() + ": " + err);
        }
    }
}

} // namespace

McpConfig load_mcp_config(const std::string &workdir)
{
    McpConfig out;
    std::map<std::string, nlohmann::json> merged;
    // 远的先读，近的后读：workdir 里那份同名条目就此盖掉全局那份
    read_file(fs::path(getenv_or("HOME", ".")) / ".realagent" / "mcp.json", merged, out.errors);
    read_file(fs::path(workdir) / ".realagent" / "mcp.json", merged, out.errors);
    // merged 是 std::map，按 key 迭代出来就是按名字有序的：同一份配置两次运行给同一张表
    for (auto &[name, c] : merged) out.servers.push_back(std::move(c));
    return out;
}

McpHub::Lease McpHub::open(const std::string &workdir)
{
    McpConfig cfg = load_mcp_config(workdir);
    Lease lease;
    lease.errors = std::move(cfg.errors);

    /* 已经连着的直接复用。**键是那份配置本身**（dump 出来的那串），不只是名字：
     * 两个仓库各自定义了 fs 且参数不同，那是两个进程；参数一样，共用一个。
     * 用 find 不用 operator[]——后者会给每一个没命中的键插一个空 weak_ptr 进去，
     * 那些垃圾还得再擦一遍。 */
    std::vector<const nlohmann::json *> todo;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const auto &c : cfg.servers)
        {
            const auto it = conns_.find(c.dump());
            if (auto have = it == conns_.end() ? nullptr : it->second.lock())
                lease.conns.push_back(std::move(have));
            else
                todo.push_back(&c);
        }
    }

    /* 没连的并行连——一个 server 起得慢（`npx` 第一次要下包）不该让别的跟着等。
     * 各线程只写自己那一格，写回池子在 join 之后，所以这里不需要锁。 */
    std::vector<std::shared_ptr<McpClient>> fresh(todo.size());
    std::vector<std::string> errs(todo.size());
    {
        std::vector<std::thread> ts;
        ts.reserve(todo.size());
        for (size_t i = 0; i < todo.size(); ++i)
            ts.emplace_back([&, i] { fresh[i] = McpClient::start(*todo[i], errs[i]); });
        for (auto &t : ts) t.join();
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (size_t i = 0; i < todo.size(); ++i)
        {
            if (!fresh[i])
            {
                // 坏 server 跳过，core 照常起（models.json 那条先例，不是 settings.json 那条）
                lease.errors.push_back(errs[i]);
                continue;
            }
            conns_[todo[i]->dump()] = fresh[i];
            lease.conns.push_back(fresh[i]);
        }
    }

    /* 拼表。core 从 server 手里只取三样，其余自己写——外部遵守协议，内部 core 知道
     * 该写什么。MCP 的 title / annotations / icons / outputSchema / execution 一律不收：
     * 端点的工具定义里没有它们的位置，annotations 更是规范自己说了不可信。 */
    for (const auto &c : lease.conns)
    {
        for (const auto &t : c->tools())
        {
            if (!t.is_object() || !t.contains("name") || !t["name"].is_string()) continue;
            const std::string remote = t["name"].get<std::string>();
            nlohmann::json one;
            // 名字 `<配置的键>__<原名>`，非法字符换 `_`、不截断（判据见 sanitize_tool_name）
            one["name"] = sanitize_tool_name(c->name() + "__" + remote);
            one["description"] = t.value("description", std::string());
            const auto schema = t.find("inputSchema");
            one["input_schema"] = schema != t.end() && schema->is_object()
                                      ? *schema
                                      : nlohmann::json{{"type", "object"}};
            one["_core"] = {{"label", remote},
                            {"dangerous", true}, // 一律。annotations 不可信（规范原话）
                            {"server", c->name()},
                            {"remote_name", remote}};
            lease.tools.push_back(std::move(one));
        }
    }
    return lease;
}

} // namespace realagent
