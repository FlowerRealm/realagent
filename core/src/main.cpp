/*
 * main.cpp — realagent core 入口（常驻服务）
 *
 * 验证模式：
 *   realagent-core test-tools → 工具执行链路验证（read/edit/bash + 权限）
 * 常驻模式（默认）：
 *   realagent-core → 启动 QUIC/HTTP3 服务（PROTOCOL.md 端点）
 *   POST /message → 启动 agent loop
 */
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "agent/agent.hpp"
#include "agent/agents.hpp"
#include "agent/context.hpp"
#include "agent/executor.hpp"
#include "agent/history.hpp"
#include "agent/session.hpp"
#include "config.hpp"
#include "server/quic_server.hpp"
#include "tools/tools.hpp"

using namespace realagent;

static int run_tool_test(CoreContext &ctx)
{
    ApprovalCoordinator approval; // 测试模式无客户端，ASK 会 30s 超时按 deny
    Executor exe(ctx, approval, std::filesystem::current_path().string());

    // 上一次跑剩的文件先删掉：留着它 edit(create) 就走成了 append 分支，
    // 这条验证从第二次运行起就不再验证它声称验证的东西
    std::remove("/tmp/ra_edit_test.txt");

    const auto show = [](const char *tag, const nlohmann::json &r) {
        printf("%s: status=%d output=%.80s\n", tag, r["status"].get<int>(),
               r["output"].get<std::string>().c_str());
    };
    show("read", exe.execute("t1", "read", "{\"file_path\":\"" __FILE__ "\"}"));
    show("create", exe.execute("t2", "edit", R"({"edits":[{"file_path":"/tmp/ra_edit_test.txt","new_text":"hello realagent\nline2"}]})"));
    // 行号与 hash 就抄 read 印出来的那两个值——跟模型走的是同一条路
    const nlohmann::json r = exe.execute("t1", "read", "{\"file_path\":\"/tmp/ra_edit_test.txt\"}");
    show("read", r);
    const std::string h1 = r["output"].get<std::string>().substr(2, 3);
    show("replace", exe.execute("t2", "edit", nlohmann::json{{"edits", {{{"file_path", "/tmp/ra_edit_test.txt"}, {"line", 1}, {"hash", h1}, {"new_text", "HELLO"}}}}}.dump()));
    show("verify", exe.execute("t1", "read", "{\"file_path\":\"/tmp/ra_edit_test.txt\"}"));
    show("bash", exe.execute("t3", "bash", "{\"command\":\"echo tools-ok && ls /tmp/ra_edit_test.txt\"}"));
    return 0;
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

/* 会话清单（GET /sessions、/new、/resume 共用）：盘上有哪些会话，以及每一个被谁打开着。
 *
 * **`current: bool` 换成 `opened_by`（ADR-0019 §10）**：多 agent 之后「当前」没有主语了，
 * 同一个目录下可以有 N 个 agent 各自打开着一个会话。一个会话要么被某个 agent 打开着，
 * 要么躺在盘上——`opened_by` 直接说的就是这句话，不需要客户端再去问「谁的当前」。
 *
 * 打开着的会话可能一条消息都还没有（文件尚未落地），此时它不在扫描结果里——
 * 补一条空的进去，客户端才看得到自己在哪儿。 */
static nlohmann::json sessions_payload(const Agents &pool, const Agent &agent)
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

/* 状态栏载荷：配的模型名 + 数据表里查到的元数据。
 * 查不到就只回名字——模型清单是参考资料，不是白名单（ADR-0009）。 */
static nlohmann::json statusline_payload(const CoreContext &ctx)
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

int main(int argc, char **argv)
{
    // 文件坏了就地退出：读不懂用户写了什么，就别带着半份配置往下跑
    auto loaded = Config::load();
    if (!loaded)
    {
        fprintf(stderr, "[config] %s\n", loaded.error().c_str());
        return 1;
    }
    auto cfg = std::move(*loaded); // 非 const：persist 需写路径
    CoreContext ctx;
    ctx.config = &cfg;

    // 模型数据表读一次，进程级只读（ADR-0010 不热重载；ADR-0019 不进 Agent）。
    // 读不动就报出来，之后照常跑——没有表只是不算钱，不是"不能对话"，
    // 为它拒绝启动是把一个次要功能提成了必需品
    std::string price_err;
    const Pricing pricing = Pricing::load(cfg, &price_err);
    if (!price_err.empty()) fprintf(stderr, "[llm] %s（本次运行不计价）\n", price_err.c_str());
    ctx.pricing = &pricing;

    /* 端点那一束（protocol / base_url / model）没有默认值，缺了就跑不了（ADR-0017）。
     *
     * 但**不在这里退出**：core 是常驻服务，TUI 是另一个进程。core 一退，
     * 用户在 TUI 里看见的是一句"连不上"——比配错了还难诊断，
     * 恰好犯了"报错了用户啥都不知道"这条本身要治的病。
     * 所以照常起、启动日志喊一遍，同时把这段话原样交给任何一次 POST /message，
     * 让它出现在用户正盯着的那块屏幕上。 */
    const std::string cfg_error = endpoint_config_error(cfg);
    if (!cfg_error.empty()) fprintf(stderr, "[config] %s\n", cfg_error.c_str());

    // 线程安全事件队列：agent 线程 emit → 事件循环 on_tick flush 到推送流
    // （ADR-0002 线程模型：quiche 非线程安全，推送必须在事件循环线程）
    std::mutex ev_mtx;
    std::deque<std::pair<std::string, std::string>> ev_queue;
    ctx.emit_fn = [&ev_mtx, &ev_queue](const std::string &type, const std::string &payload) {
        std::lock_guard<std::mutex> lk(ev_mtx);
        ev_queue.emplace_back(type, payload);
    };

    for (const auto &t : tool_defs())
        fprintf(stderr, "  tool: %s (dangerous=%d)\n", t.name.c_str(), (int)t.dangerous);

    ApprovalCoordinator approval;
    approval.set_emit(ctx.emit_fn); // permission_request 也走队列

    if (argc > 1 && std::string(argv[1]) == "test-tools")
    {
        return run_tool_test(ctx);
    }

    // —— 常驻服务模式 ——
    // core 启动时 agent 数为 0，不自动创建任何 agent（ADR-0019）：自动建就得替用户
    // 猜 workdir，而 ADR-0017 刚把猜赶出去。客户端连上来自己建第一个。
    Agents pool(ctx, approval);

    QuicServerConfig scfg;
    // 证书用全局绝对路径（不依赖 cwd）
    {
        const std::string home = getenv_or("HOME", ".");
        scfg.cert_file = home + "/.realagent/cert.pem";
        scfg.key_file = home + "/.realagent/key.pem";
    }
    QuicServer server(scfg);
    // 没有客户端订阅推送流 = 没人能裁决危险工具，当场拒绝（ADR-0019 §8）
    approval.set_online([&server] { return server.has_client(); });

    QuicCallbacks cbs;
    // 斜杠命令列表（GET /commands，TUI 菜单数据源）。core 是唯一真相源。
    // 与下方 /message 的斜杠命令分支共用同一命令集——新增命令两处同步（v1 义务）。
    cbs.on_commands = []() {
        nlohmann::json arr = nlohmann::json::array();
        const auto add = [&arr](const char *name, const char *desc) {
            arr.push_back(nlohmann::json{{"name", name}, {"description", desc}});
        };
        add("new", "新建会话（清空当前对话，旧会话留在盘上）");
        add("resume", "查看会话列表（/resume <id> 恢复某个会话）");
        add("model", "查看模型清单（/model <name> 切换主模型）");
        return arr.dump();
    };

    const auto err_json = [](const std::string &msg) {
        return nlohmann::json{{"ok", false}, {"error", msg}}.dump();
    };

    /* 事件循环线程碰 agent 的那把锁一律用 try_lock（ADR-0017）。
     *
     * 这些回调跑在事件循环那唯一一条线程上（quic_server.cpp 的 poll 循环）。
     * 在这儿阻塞等锁，等的是"一次 run 跑完"——期间收不了任何请求，
     * 包括那个唯一能把 run 停下来的 POST /interrupt，也推不出任何事件帧。
     * 用户打一条 /new 想放弃当前任务，换来的是整个客户端假死到 run 自己结束。
     *
     * 所以宁可当场说"忙着呢"：拿不到锁就回一句人话，让他先中断。
     * 一次拒绝是一句话，一次假死是没有话。
     *
     * 锁归每个 Agent 自己（Agent::try_lock）——共享一把会让「A 在跑」挡住「动 B 的历史」。 */
    const auto agent_busy = "agent 正在运行——先中断（Esc / POST /interrupt）再执行这条命令";

    // 斜杠命令的唯一实现。两个门进来（`POST /message` 的 `/` 前缀、`POST /command`），
    // 一份代码——两份实现迟早只改一边，那时同一条命令在两个端点上行为不同，
    // 谁都查不出为什么。命令不投收件箱，直接返回结果。
    auto handle_command = [&](Agent &agent, const std::string &user_input) -> std::string {
        auto lk = agent.try_lock();
        if (!lk.owns_lock()) return err_json(agent_busy);
        if (user_input == "/new")
        {
            agent.reset(); // 新建会话：清空历史 + 换一个 JSONL 文件（旧的留在盘上）
            return nlohmann::json{{"ok", true}, {"command", "new"}, {"data", sessions_payload(pool, agent)}}
                .dump();
        }
        // 首空白分词为命令名：/resume[ <id>]、/model[ <name>]
        const std::string cmd = user_input.substr(0, user_input.find(' '));
        // 命令参数：命令名之后去掉首尾空白的那一段（无参即空串）
        std::string arg =
            user_input.size() > cmd.size() ? user_input.substr(cmd.size() + 1) : std::string();
        while (!arg.empty() && arg.back() == ' ') arg.pop_back();

        if (cmd == "/resume")
        {
            // 无参 = 列会话（清单里 current 标出自己在哪儿）；带 id = 恢复那一个。
            // 恢复失败保持原会话不动：宁可这条命令没生效，也不能把人扔进一段空白历史
            if (!arg.empty() && !agent.resume(arg)) return err_json("unknown session: " + arg);
            return nlohmann::json{{"ok", true}, {"command", "resume"}, {"data", sessions_payload(pool, agent)}}
                .dump();
        }
        if (cmd == "/model")
        {
            // 无参 = 列清单；带名 = 切主模型（写回 settings.json，下一次调用即生效）。
            // 只认数据表里的模型：交互式选择就该从已知的里挑，打字选中不存在的
            // 只会得到一个端点 400。启动时不校验配置是另一回事（ADR-0009）。
            if (!arg.empty())
            {
                bool known = false;
                for (const nlohmann::json &m : models_payload(ctx))
                    if (m["name"] == arg) known = true;
                if (!known) return err_json("unknown model: " + arg);
                // 点对点写：只改文件里的 model 这一个键。statusline 帧不在这里推——
                // 事件循环发现载荷变了自己会推（见 on_tick）
                if (!ctx.config->persist("model", nlohmann::json(arg)))
                    return err_json("写入 settings.json 失败");
            }
            return nlohmann::json{{"ok", true}, {"command", "model"}, {"data", models_payload(ctx)}}
                .dump();
        }
        return err_json("unknown command: " + cmd);
    };

    // POST /agent：建一个 agent。workdir 必传，core 不猜
    cbs.on_agent = [&](const std::string &body) {
        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        std::string err;
        const int id = pool.create(j.value("workdir", ""), 0, {}, {}, err);
        if (id <= 0) return err_json(err);
        return nlohmann::json{{"ok", true}, {"agent_id", id}}.dump();
    };
    // GET /agents：列出所有 agent
    cbs.on_agents = [&pool](const std::string &) {
        return pool.list().dump();
    };

    /* GET /history：一个 agent 的历史，回放成事件帧（ADR-0020）。 */
    cbs.on_history = [&](const std::string &body) {
        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        const int id = j.value("agent_id", 0);
        Agent *a = pool.find(id);
        if (!a) return err_json("无此 agent: " + std::to_string(id));
        nlohmann::json msgs;
        if (!Session::read(a->session_dir(), a->session_id(), msgs))
            msgs = nlohmann::json::array();
        return history_frames(msgs).dump();
    };

    cbs.on_message = [&](const std::string &body) {
        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        const std::string user_input = j.value("message", "");
        if (user_input.empty()) return err_json("empty message");
        const int id = j.value("agent_id", 0);
        Agent *a = pool.find(id);
        if (!a) return err_json("无此 agent: " + std::to_string(id));
        if (user_input[0] == '/') return handle_command(*a, user_input);
        if (!cfg_error.empty()) return err_json(cfg_error);
        a->post(user_input);
        return std::string("{\"status\":\"processing\"}");
    };
    cbs.on_command = [&](const std::string &body) {
        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        std::string cmd = j.value("command", "");
        if (cmd.empty()) return err_json("empty command");
        if (cmd[0] != '/') cmd.insert(cmd.begin(), '/');
        const int id = j.value("agent_id", 0);
        Agent *a = pool.find(id);
        if (!a) return err_json("无此 agent: " + std::to_string(id));
        return handle_command(*a, cmd);
    };
    // GET /sessions 与 POST /session 都要指名道姓：会话目录跟着 agent 的 workdir 走
    cbs.on_sessions = [&](const std::string &body) {
        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        const int id = j.value("agent_id", 0);
        Agent *a = pool.find(id);
        if (!a) return err_json("无此 agent: " + std::to_string(id));
        auto lk = a->try_lock();
        if (!lk.owns_lock()) return err_json(agent_busy);
        return sessions_payload(pool, *a).dump();
    };
    cbs.on_session = [&](const std::string &body) {
        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        const int id = j.value("agent_id", 0);
        Agent *a = pool.find(id);
        if (!a) return err_json("无此 agent: " + std::to_string(id));
        auto lk = a->try_lock();
        if (!lk.owns_lock()) return err_json(agent_busy);
        const std::string sid = j.value("id", "");
        if (sid.empty())
            a->reset();
        else if (!a->resume(sid))
            return err_json("unknown session: " + sid);
        return nlohmann::json{{"ok", true},
                              {"data", sessions_payload(pool, *a)}}
            .dump();
    };
    cbs.on_interrupt = [&](const std::string &body) {
        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        Agent *a = pool.find(j.value("agent_id", 0));
        if (!a) return;
        a->interrupt();
        approval.cancel(a->id());
    };

    // 客户端正常退出
    cbs.on_group_close = [](const std::string &) {
        return std::string("{\"ok\":true}");
    };

    cbs.on_approval_response = [&approval](const std::string &id, bool allow) {
        approval.respond(id, allow);
    };
    // 状态栏数据（GET /statusline）：客户端启动时拉一次，之后由 statusline 帧推更新
    cbs.on_statusline = [&ctx]() { return statusline_payload(ctx).dump(); };
    // 启动值取一次，避免首轮推一帧与客户端 GET /statusline 重复的内容
    std::string last_statusline = statusline_payload(ctx).dump();
    cbs.on_tick = [&]() {
        if (std::string cur = statusline_payload(ctx).dump(); cur != last_statusline)
        {
            last_statusline = std::move(cur);
            server.push_event("statusline", last_statusline);
        }

        std::deque<std::pair<std::string, std::string>> batch;
        {
            std::lock_guard<std::mutex> lk(ev_mtx);
            batch.swap(ev_queue);
        }
        for (auto &[t, p] : batch) server.push_event(t, p);
    };

    server.set_callbacks(cbs);
    server.run(); // 阻塞事件循环

    return 0;
}
