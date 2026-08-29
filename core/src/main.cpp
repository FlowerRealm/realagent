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
#include "agent/executor.hpp"
#include "agent/history.hpp"
#include "agent/session.hpp"
#include "config.hpp"
#include "context.hpp"
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
static nlohmann::json sessions_payload(const Agents &pool, const std::string &client,
                                       const Agent &agent)
{
    const std::map<std::string, std::string> opened = pool.openers(client);
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

    /* 体是客户端给的，形状不由 core 说了算：不是对象、没这个键、值不是字符串——
     * 都按"没说话"处理，绝不为此崩在事件循环线程里。
     * （const operator[] 撞上缺键是未定义行为，所以按迭代器查；find 在非对象上恒返回 end） */
    const auto field = [](const std::string &body, const char *key) {
        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        const auto it = j.find(key);
        return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
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
    auto handle_command = [&](const std::string &client, Agent &agent,
                              const std::string &user_input) -> std::string {
        auto lk = agent.try_lock();
        if (!lk.owns_lock()) return err_json(agent_busy);
        if (user_input == "/new")
        {
            agent.reset(); // 新建会话：清空历史 + 换一个 JSONL 文件（旧的留在盘上）
            return nlohmann::json{{"ok", true}, {"command", "new"}, {"data", sessions_payload(pool, client, agent)}}
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
            return nlohmann::json{{"ok", true}, {"command", "resume"}, {"data", sessions_payload(pool, client, agent)}}
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

    /* 每个要动某个 agent 的端点都从这里取它。**agent_id 与 client_id 都必填、无默认**：
     * agent_id 猜"就那一个吧"，在第二个 agent 出现的当天就会变成"刚才那条消息发给谁了"
     * （ADR-0019）；client_id 决定这个 agent 在不在你那一组，跨组一律当不存在（ADR-0021）。 */
    const auto pick = [&](const std::string &body) {
        return pool.find(field(body, "client_id"), field(body, "agent_id"));
    };

    // POST /agent：建一个 agent。workdir 必传，core 不猜——它是全机单实例，
    // 自己的 cwd 是"启动它那个 shell 当时在哪"，跟任何 agent 都无关（ADR-0019）。
    // 客户端可以替用户填（它知道用户站在哪），那是客户端的默认值，不是 core 的。
    // 人不是图上的节点，所以这道门建出来的 agent 没有边。
    cbs.on_agent = [&](const std::string &body) {
        std::string err;
        const std::string id =
            pool.create(field(body, "client_id"), field(body, "workdir"), "", {}, {}, err);
        if (id.empty()) return err_json(err);
        return nlohmann::json{{"ok", true}, {"agent_id", id}}.dump();
    };
    // GET /agents：人看得见全部——TUI 不是图上的节点，不受边的约束
    cbs.on_agents = [&pool](const std::string &body) {
        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        const auto it = j.find("client_id");
        return pool.list(it != j.end() && it->is_string() ? it->get<std::string>() : "").dump();
    };

    /* GET /history：一个 agent 的历史，回放成事件帧（ADR-0020）。
     *
     * 读的是**盘上那份**，不是内存里的 messages_：Session::append 每条消息完成时即时
     * 追加，所以文件永远是「最后一条已落盘的消息」为止那一段——正好就是接缝的位置。
     * 客户端的视图 = 这段回放 + 推送流喂进来的活尾巴。
     *
     * 顺带把内存那份从这条路上摘干净：idle 的 agent 早晚要丢掉 messages_（ADR-0019 §7），
     * 到那时这个端点一个字都不用改。
     *
     * 新会话还没写过盘 → 空数组，不是错。 */
    cbs.on_history = [&](const std::string &body) {
        Agent *a = pick(body);
        if (!a) return err_json("无此 agent（GET /history 要带 agent_id）");
        nlohmann::json msgs;
        if (!Session::read(a->session_dir(), a->session_id(), msgs))
            msgs = nlohmann::json::array();
        return history_frames(msgs).dump();
    };

    cbs.on_message = [&](const std::string &body) {
        const std::string user_input = field(body, "message");
        if (user_input.empty()) return err_json("empty message");
        Agent *a = pick(body);
        if (!a) return err_json("无此 agent（POST /message 要带 agent_id）");
        if (user_input[0] == '/') return handle_command(field(body, "client_id"), *a, user_input);
        // 端点没配齐就别投：投了也是当场失败，而这段话比那句失败清楚得多
        if (!cfg_error.empty()) return err_json(cfg_error);
        // 投进收件箱就返回。agent 自己有一条线程在等（ADR-0019）
        a->post(user_input);
        return std::string("{\"status\":\"processing\"}");
    };
    // POST /command：体 {"agent_id","command"}。与上面 `/` 前缀那条走同一份实现——
    // 存在两个端点是历史形态（PROTOCOL.md），不是两套行为。命令名可以不带 '/'。
    cbs.on_command = [&](const std::string &body) {
        std::string cmd = field(body, "command");
        if (cmd.empty()) return err_json("empty command");
        if (cmd[0] != '/') cmd.insert(cmd.begin(), '/');
        Agent *a = pick(body);
        if (!a) return err_json("无此 agent（POST /command 要带 agent_id）");
        return handle_command(field(body, "client_id"), *a, cmd);
    };
    // GET /sessions 与 POST /session 都要指名道姓：会话目录跟着 agent 的 workdir 走
    cbs.on_sessions = [&](const std::string &body) {
        Agent *a = pick(body);
        if (!a) return err_json("无此 agent（GET /sessions 要带 agent_id）");
        auto lk = a->try_lock();
        if (!lk.owns_lock()) return err_json(agent_busy);
        return sessions_payload(pool, field(body, "client_id"), *a).dump();
    };
    cbs.on_session = [&](const std::string &body) {
        Agent *a = pick(body);
        if (!a) return err_json("无此 agent（POST /session 要带 agent_id）");
        auto lk = a->try_lock();
        if (!lk.owns_lock()) return err_json(agent_busy);
        const std::string id = field(body, "id");
        if (id.empty())
            a->reset();
        else if (!a->resume(id))
            return err_json("unknown session: " + id);
        return nlohmann::json{{"ok", true},
                              {"data", sessions_payload(pool, field(body, "client_id"), *a)}}
            .dump();
    };
    cbs.on_interrupt = [&](const std::string &body) {
        Agent *a = pick(body);
        if (!a) return;
        a->interrupt();
        // 按 agent 取消，不是一刀切：中断 A 掐掉 B 正等着的审批，
        // 是把「停下这一个」办成了「全场停摆」（ADR-0019 §8）
        approval.cancel(a->id());
    };

    // 客户端正常退出：显式关组。断线那条路是兜底，不是主路
    cbs.on_group_close = [&pool](const std::string &body) {
        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        const auto it = j.find("client_id");
        if (it != j.end() && it->is_string()) pool.close_group(it->get<std::string>());
        return std::string("{\"ok\":true}");
    };

    /* 组的生命周期（ADR-0021）：客户端主动建，退出前显式关，**断线满 60 秒即关**。
     *
     * 测活不自己造：QUIC 有原生的 max_idle_timeout，连接死了 quic_server 立刻知道，
     * 这里只记一张表。断线不等于关组——网抖一下就杀掉一组正在干活的 agent，
     * 那是把丢包变成了 kill。 */
    std::map<std::string, std::chrono::steady_clock::time_point> gone_since;
    cbs.on_client_here = [&gone_since](const std::string &c) { gone_since.erase(c); };
    cbs.on_client_gone = [&gone_since](const std::string &c) {
        gone_since[c] = std::chrono::steady_clock::now();
        fprintf(stderr, "[group] 客户端 %s 断开，60 秒内不回来就关掉它那一组\n", c.c_str());
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

        // 断线满 60 秒的组关掉。60 是个选定的数不是推导出来的：要容得下网线抖一下，
        // 又短到不至于让忘关的窗口一直占着内存。不合适就改这一个常量
        const auto now = std::chrono::steady_clock::now();
        for (auto it = gone_since.begin(); it != gone_since.end();)
        {
            if (now - it->second < std::chrono::seconds(60))
            {
                ++it;
                continue;
            }
            fprintf(stderr, "[group] 客户端 %s 超过 60 秒没回来，关组\n", it->first.c_str());
            pool.close_group(it->first);
            it = gone_since.erase(it);
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
