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
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "agent/agent.hpp"
#include "agent/executor.hpp"
#include "agent/session.hpp"
#include "config.hpp"
#include "context.hpp"
#include "server/quic_server.hpp"
#include "tools/tools.hpp"

using namespace realagent;

static int run_tool_test(CoreContext& ctx) {
    ApprovalCoordinator approval; // 测试模式无客户端，ASK 会 30s 超时按 deny
    Executor exe(ctx, approval);

    // 上一次跑剩的文件先删掉：留着它 edit(create) 就走成了 append 分支，
    // 这条验证从第二次运行起就不再验证它声称验证的东西
    std::remove("/tmp/ra_edit_test.txt");

    const auto show = [](const char* tag, const json& r) {
        printf("%s: status=%lld output=%.80s\n", tag, r["status"].as_int64().value_or(-1),
               r["output"].as_string().value_or("").c_str());
    };
    show("read", exe.execute("t1", "read", "{\"file_path\":\"" __FILE__ "\"}"));
    show("edit(create)", exe.execute("t2", "edit", "{\"file_path\":\"/tmp/ra_edit_test.txt\",\"new_string\":\"hello realagent\\n\"}"));
    show("edit(append)", exe.execute("t2", "edit", "{\"file_path\":\"/tmp/ra_edit_test.txt\",\"old_string\":\"\",\"new_string\":\"line2\\n\"}"));
    show("read(verify)", exe.execute("t1", "read", "{\"file_path\":\"/tmp/ra_edit_test.txt\"}"));
    show("bash", exe.execute("t3", "bash", "{\"command\":\"echo tools-ok && ls /tmp/ra_edit_test.txt\"}"));
    return 0;
}

/* /model 响应：模型数据表里的清单，current 标出配置里当前那档（ADR-0009）。 */
static json models_payload(const CoreContext& ctx, const Agent& agent) {
    const std::string cur = ctx.config->model(ModelTier::Main);
    json arr = json::array();
    const json& list = agent.pricing().models();
    for (std::size_t i = 0; i < list.size(); ++i) {
        json m = list[i];
        m["current"] = (m["name"].as_string().value_or("") == cur);
        arr.push_back(std::move(m));
    }
    return arr;
}

/* 会话清单（GET /sessions、/new、/resume 共用）：盘上有哪些会话 + 现在在哪一个。
 * 当前会话可能一条消息都还没有（文件尚未落地），此时它不在扫描结果里——
 * 补一条空的进去，客户端的 current 才不会落空。 */
static json sessions_payload(const Agent& agent) {
    json arr = json::array();
    bool seen_current = false;
    for (const auto& s : Session::list()) {
        json e = to_json(s);
        const bool cur = (s.id == agent.session_id());
        e["current"] = cur;
        seen_current = seen_current || cur;
        arr.push_back(std::move(e));
    }
    if (!seen_current) {
        json e = to_json(SessionInfo{.id = agent.session_id()});
        e["current"] = true;
        arr.push_back(std::move(e)); // 新会话还没写过盘，排在最前（它最新）
        std::rotate(arr.as_array().begin(), arr.as_array().end() - 1, arr.as_array().end());
    }
    return arr;
}

/* 状态栏载荷：配的模型名 + 数据表里查到的元数据。
 * 查不到就只回名字——模型清单是参考资料，不是白名单（ADR-0009）。 */
static json statusline_payload(const CoreContext& ctx, const Agent& agent) {
    const std::string name = ctx.config->model(ModelTier::Main);
    json out;
    out["model"] = name;
    const json& list = agent.pricing().models();
    for (std::size_t i = 0; i < list.size(); ++i) {
        const json m = list[i];
        if (m["name"].as_string().value_or("") != name) continue;
        out["owned_by"] = m["owned_by"];
        out["context"] = m["context"];
        break;
    }
    return out;
}

int main(int argc, char** argv) {
    // 文件坏了就地退出：读不懂用户写了什么，就别带着半份配置往下跑
    auto loaded = Config::load();
    if (!loaded) {
        fprintf(stderr, "[config] %s\n", loaded.error().c_str());
        return 1;
    }
    auto cfg = std::move(*loaded); // 非 const：persist 需写路径
    CoreContext ctx;
    ctx.config = &cfg;

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
    ctx.emit_fn = [&ev_mtx, &ev_queue](const std::string& type, const std::string& payload) {
        std::lock_guard<std::mutex> lk(ev_mtx);
        ev_queue.emplace_back(type, payload);
    };

    for (const auto& t : tool_defs())
        fprintf(stderr, "  tool: %s (dangerous=%d)\n", t.name.c_str(), (int)t.dangerous);

    ApprovalCoordinator approval;
    approval.set_emit(ctx.emit_fn); // permission_request 也走队列
    Executor exe(ctx, approval);

    if (argc > 1 && std::string(argv[1]) == "test-tools") {
        return run_tool_test(ctx);
    }

    // —— 常驻服务模式 ——
    Agent agent(ctx, exe);
    std::mutex agent_mtx; // 串行化 agent 运行（一次一个任务）

    QuicServerConfig scfg;
    // 证书用全局绝对路径（不依赖 cwd）
    {
        const std::string home = getenv_or("HOME", ".");
        scfg.cert_file = home + "/.realagent/cert.pem";
        scfg.key_file = home + "/.realagent/key.pem";
    }
    QuicServer server(scfg);

    QuicCallbacks cbs;
    // 斜杠命令列表（GET /commands，TUI 菜单数据源）。core 是唯一真相源。
    // 与下方 /message 的斜杠命令分支共用同一命令集——新增命令两处同步（v1 义务）。
    cbs.on_commands = []() {
        json arr = json::array();
        const auto add = [&arr](const char* name, const char* desc) {
            json cmd;
            cmd["name"] = name;
            cmd["description"] = desc;
            arr.push_back(std::move(cmd));
        };
        add("new", "新建会话（清空当前对话，旧会话留在盘上）");
        add("resume", "查看会话列表（/resume <id> 恢复某个会话）");
        add("model", "查看模型清单（/model <name> 切换主模型）");
        return arr.dump();
    };
    /* 事件循环线程碰 agent_mtx 一律用 try_lock（ADR-0017）。
     *
     * 这些回调跑在事件循环那唯一一条线程上（quic_server.cpp 的 poll 循环）。
     * 在这儿阻塞等锁，等的是"一次 run 跑完"——期间收不了任何请求，
     * 包括那个唯一能把 run 停下来的 POST /interrupt，也推不出任何事件帧。
     * 用户打一条 /new 想放弃当前任务，换来的是整个客户端假死到 run 自己结束。
     *
     * 所以宁可当场说"忙着呢"：拿不到锁就回一句人话，让他先中断。
     * 一次拒绝是一句话，一次假死是没有话。 */
    const auto agent_busy = []() {
        json out;
        out["ok"] = false;
        out["error"] = "agent 正在运行——先中断（Esc / POST /interrupt）再执行这条命令";
        return out.dump();
    };

    // 斜杠命令的唯一实现。两个门进来（`POST /message` 的 `/` 前缀、`POST /command`），
    // 一份代码——两份实现迟早只改一边，那时同一条命令在两个端点上行为不同，
    // 谁都查不出为什么。命令不启动 agent，直接返回结果。
    // 与 agent 互斥：/new /resume 会换掉对话历史，执行中换等于把地板抽走。
    auto handle_command = [&](const std::string& user_input) -> std::string {
        std::unique_lock<std::mutex> lk(agent_mtx, std::try_to_lock);
        if (!lk.owns_lock()) return agent_busy();
        if (user_input == "/new") {
            agent.reset(); // 新建会话：清空历史 + 换一个 JSONL 文件（旧的留在盘上）
            json out;
            out["ok"] = true;
            out["command"] = "new";
            out["data"] = sessions_payload(agent);
            return out.dump();
        }
        // 首空白分词为命令名：/resume[ <id>]、/model[ <name>]
        const std::string cmd = user_input.substr(0, user_input.find(' '));
        // 命令参数：命令名之后去掉首尾空白的那一段（无参即空串）
        const auto arg_of = [&user_input, &cmd]() {
            std::string a =
                user_input.size() > cmd.size() ? user_input.substr(cmd.size() + 1) : std::string();
            while (!a.empty() && a.back() == ' ') a.pop_back();
            return a;
        };
        if (cmd == "/resume") {
            // 无参 = 列会话（清单里 current 标出自己在哪儿）；带 id = 恢复那一个。
            // 恢复失败保持原会话不动：宁可这条命令没生效，也不能把人扔进一段空白历史
            const std::string id = arg_of();
            json out;
            out["command"] = "resume";
            if (!id.empty() && !agent.resume(id)) {
                out["ok"] = false;
                out["error"] = "unknown session: " + id;
                return out.dump();
            }
            out["ok"] = true;
            out["data"] = sessions_payload(agent);
            return out.dump();
        }
        if (cmd == "/model") {
            // 无参 = 列清单；带名 = 切主模型（写回 settings.json，下一次调用即生效）。
            // 只认数据表里的模型：交互式选择就该从已知的里挑，打字选中不存在的
            // 只会得到一个端点 400。启动时不校验配置是另一回事（ADR-0009）。
            const std::string name = arg_of();
            json out;
            out["command"] = "model";
            if (!name.empty()) {
                const json avail = models_payload(ctx, agent);
                bool known = false;
                for (std::size_t i = 0; i < avail.size(); ++i)
                    if (avail[i]["name"].as_string().value_or("") == name) known = true;
                if (!known) {
                    out["ok"] = false;
                    out["error"] = "unknown model: " + name;
                    return out.dump();
                }
                // 点对点写：只改文件里的 model 这一个键。statusline 帧不在这里推——
                // 事件循环发现载荷变了自己会推（见 on_tick）
                if (!ctx.config->persist("model", json(name))) {
                    out["ok"] = false;
                    out["error"] = "写入 settings.json 失败";
                    return out.dump();
                }
            }
            out["ok"] = true;
            out["data"] = models_payload(ctx, agent);
            return out.dump();
        }
        return std::string("{\"error\":\"unknown command\"}");
    };
    cbs.on_message = [&](const std::string& body) {
        // POST /message：body 为 {"message":"..."}。agent 在独立线程运行
        // （ADR-0002 线程模型）：不阻塞事件循环，审批等待期间仍能收裁决。
        auto msg = json::parse(body).value_or(json{});
        const std::string user_input = msg["message"].as_string().value_or("");
        if (user_input.empty()) return std::string("{\"error\":\"empty message\"}");
        if (user_input[0] == '/') return handle_command(user_input);
        // 端点没配齐就别起 agent：起了也是当场失败，而这段话比那句失败清楚得多
        if (!cfg_error.empty()) {
            json out;
            out["error"] = cfg_error;
            return out.dump();
        }

        std::thread([&agent, &agent_mtx, user_input]() {
            std::lock_guard<std::mutex> lk(agent_mtx);
            agent.run(user_input);
        }).detach();
        return std::string("{\"status\":\"processing\"}");
    };
    // POST /command：体 {"command":"/new"}。与上面 `/` 前缀那条走同一份实现——
    // 存在两个端点是历史形态（PROTOCOL.md），不是两套行为。命令名可以不带 '/'。
    cbs.on_command = [&handle_command](const std::string& body) {
        auto b = json::parse(body).value_or(json{});
        std::string cmd = std::string(b["command"].as_string().value_or(""));
        if (cmd.empty()) return std::string("{\"error\":\"empty command\"}");
        if (cmd[0] != '/') cmd.insert(cmd.begin(), '/');
        return handle_command(cmd);
    };
    // GET /sessions：盘上的会话清单 + current 标当前
    cbs.on_sessions = [&agent, &agent_mtx, &agent_busy]() {
        std::unique_lock<std::mutex> lk(agent_mtx, std::try_to_lock);
        if (!lk.owns_lock()) return agent_busy();
        return sessions_payload(agent).dump();
    };
    // POST /session：体带 id = 恢复，体空 = 新建。与 /resume /new 同一套动作，
    // 只是给不走斜杠命令的客户端留的门。
    cbs.on_session = [&agent, &agent_mtx, &agent_busy](const std::string& body) {
        auto b = json::parse(body).value_or(json{});
        const std::string id = std::string(b["id"].as_string().value_or(""));
        std::unique_lock<std::mutex> lk(agent_mtx, std::try_to_lock);
        if (!lk.owns_lock()) return agent_busy();
        json out;
        if (id.empty()) {
            agent.reset();
        } else if (!agent.resume(id)) {
            out["ok"] = false;
            out["error"] = "unknown session: " + id;
            return out.dump();
        }
        out["ok"] = true;
        out["data"] = sessions_payload(agent);
        return out.dump();
    };
    cbs.on_interrupt = [&agent, &approval]() {
        agent.interrupt();
        approval.cancel_all();
    };
    cbs.on_approval_response = [&approval](const std::string& id, bool allow) {
        approval.respond(id, allow);
    };
    // 状态栏数据（GET /statusline）：客户端启动时拉一次，之后由 statusline 帧推更新
    cbs.on_statusline = [&ctx, &agent]() { return statusline_payload(ctx, agent).dump(); };
    // 事件循环每轮：状态栏盯一眼，把 agent 线程入队的事件 flush 到推送流
    //
    // 状态栏载荷变了就推一帧。载荷本身就是信号——谁改的配置、怎么改的，这里不关心，
    // 改配置的代码路径也就不需要记得通知谁（漏不掉，也不会为无关变更白推）。
    // 不为此单开线程：推帧必须在事件循环线程（ADR-0002），线程只能把活儿再传回来。
    // 启动值取一次，避免首轮推一帧与客户端 GET /statusline 重复的内容。
    std::string last_statusline = statusline_payload(ctx, agent).dump();
    cbs.on_tick = [&]() {
        if (std::string cur = statusline_payload(ctx, agent).dump(); cur != last_statusline) {
            last_statusline = std::move(cur);
            server.push_event("statusline", last_statusline);
        }

        std::deque<std::pair<std::string, std::string>> batch;
        {
            std::lock_guard<std::mutex> lk(ev_mtx);
            batch.swap(ev_queue);
        }
        for (auto& [t, p] : batch) server.push_event(t, p);
    };

    server.set_callbacks(cbs);
    server.run(); // 阻塞事件循环

    return 0;
}
