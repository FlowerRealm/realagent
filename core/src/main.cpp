/*
 * main.cpp — realagent core 入口（常驻服务）
 *
 * M0-M2 验证模式：
 *   realagent-core test-tools → 工具执行链路验证（read/edit/bash + 权限）
 * M5+ 常驻模式（默认）：
 *   realagent-core → 加载插件 + 启动 QUIC/HTTP3 服务（PROTOCOL.md 端点）
 *   POST /message → 启动 agent loop
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <deque>
#include <mutex>

#include "config.hpp"
#include "extension/slots.hpp"
#include "agent/executor.hpp"
#include "agent/agent.hpp"
#include "agent/session.hpp"
#include "server/quic_server.hpp"

using namespace realagent;

static int run_tool_test(CoreContext& ctx, PluginManager& mgr) {
    ApprovalCoordinator approval; // 测试模式无客户端，ASK 会 30s 超时按 deny
    Executor exe(ctx, mgr, approval);

    {
        const auto r = exe.execute("t1", "read", "{\"file_path\":\".realagent/extensions/core-tools/plugin.json\"}");
        printf("read: status=%d messages=%s\n\n", r.status, r.messages.c_str());
    }
    {
        const auto r = exe.execute("t2", "edit", "{\"file_path\":\"/tmp/ra_edit_test.txt\",\"new_string\":\"hello realagent\\n\"}");
        printf("edit(create): status=%d messages=%s\n", r.status, r.messages.c_str());
    }
    {
        const auto r = exe.execute("t2", "edit", "{\"file_path\":\"/tmp/ra_edit_test.txt\",\"old_string\":\"\",\"new_string\":\"line2\\n\"}");
        printf("edit(append): status=%d messages=%s\n", r.status, r.messages.c_str());
    }
    {
        const auto r = exe.execute("t1", "read", "{\"file_path\":\"/tmp/ra_edit_test.txt\"}");
        printf("read(verify): status=%d messages=%s\n", r.status, r.messages.c_str());
    }
    {
        const auto r = exe.execute("t3", "bash", "{\"command\":\"echo core-tools-ok && ls /tmp/ra_edit_test.txt\"}");
        printf("bash: status=%d messages=%s\n", r.status, r.messages.c_str());
    }
    return 0;
}

/* GET /plugins 响应：PluginInfo → JSON。字段名与顺序由 BOOST_DESCRIBE_STRUCT
 * 的字段表生成，即结构体声明本身——契约与结构体不可能再走散。 */
static json plugins_payload(const std::vector<PluginInfo>& list) { return to_json(list); }

/* /model 响应：当前 provider 的模型清单（现问现答，core 不存表——ADR-0012）。
 * current 标出配置里当前那档。没有 provider 就没有清单可谈。 */
static json models_payload(const CoreContext& ctx, const PluginManager& mgr) {
    const std::string cur = ctx.config->model(ModelTier::Main);
    json arr = json::array();
    const auto text = models_json(mgr, *ctx.config);
    const auto parsed = text.empty() ? std::nullopt : json::parse(text);
    if (!parsed || !parsed->is_array()) return arr;
    for (std::size_t i = 0; i < parsed->size(); ++i) {
        json m = (*parsed)[i];
        m["current"] = (m["name"].as_string().value_or("") == cur);
        arr.push_back(std::move(m));
    }
    return arr;
}

/* /provider 响应：能进"改请求"那一段的候选（提供 request.refine 的容器）。
 * models 是它报了多少个模型——切之前看得见要切到哪儿去。 */
static json providers_payload(const CoreContext& ctx, const PluginManager& mgr) {
    json arr = json::array();
    const Plugin* cur = current_provider(mgr, *ctx.config);
    for (const auto& name : mgr.providers_of(PLUGIN_CAP_REQUEST_REFINE)) {
        const Plugin* p = mgr.find(name);
        if (!p) continue;
        std::size_t n = 0;
        if (auto list = cap_of<plugin_model_list_fn>(*p, PLUGIN_CAP_MODEL_LIST)) {
            if (const char* text = list.fn(list.self)) // 借阅：读完即用，不释放
                if (const auto arr2 = json::parse(text); arr2 && arr2->is_array()) n = arr2->size();
        }
        json e;
        e["name"] = name;
        e["current"] = (cur == p);
        e["models"] = (long long)n;
        arr.push_back(std::move(e));
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

/* 状态栏载荷：配的模型名 + 当前 provider 清单里查到的元数据。
 * 查不到就只回名字——模型清单是参考资料，不是白名单（ADR-0009）。
 *
 * on_tick 每次都会调它做差分，而清单是现问现答的（要调插件 + 解析 JSON），
 * 所以按 (provider, 模型名) 备忘：这两个都是 core 自己的状态，比较不要钱。
 * 这正是[[能力]]词条允许的那种副本——可随时丢弃，失效点明写。 */
static json statusline_payload(const CoreContext& ctx, const PluginManager& mgr) {
    static std::string memo_key;
    static json memo;
    const std::string name = ctx.config->model(ModelTier::Main);
    const Plugin* prov = current_provider(mgr, *ctx.config);
    const std::string key = (prov ? prov->name : std::string("-")) + "/" + name;
    if (key == memo_key) return memo;

    memo_key = key;
    memo = json{};
    memo["model"] = name;
    if (const auto parsed = json::parse(models_json(mgr, *ctx.config)); parsed && parsed->is_array()) {
        for (std::size_t i = 0; i < parsed->size(); ++i) {
            const json m = (*parsed)[i];
            if (m["name"].as_string().value_or("") != name) continue;
            memo["owned_by"] = m["owned_by"];
            memo["context"] = m["context"];
            break;
        }
    }
    return memo;
}

int main(int argc, char** argv) {
    // 配置缺项不是错（缺的取默认值，见 config_defaults.hpp），但文件坏了就地退出：
    // 读不懂用户写了什么，就别带着半份配置往下跑
    auto loaded = Config::load();
    if (!loaded) {
        fprintf(stderr, "[config] %s\n", loaded.error().c_str());
        return 1;
    }
    auto cfg = std::move(*loaded); // 非 const：CoreContext::config 需写路径（插件禁用清单 persist）
    CoreContext ctx;
    ctx.config = &cfg;

    // 线程安全事件队列：agent 线程 emit → 事件循环 on_tick flush 到推送流
    // （ADR-0002 线程模型：quiche 非线程安全，推送必须在事件循环线程）。
    // 声明在加载之前：插件 init 里就可能 emit，那时队列必须已经在。
    std::mutex ev_mtx;
    std::deque<std::pair<std::string, std::string>> ev_queue;

    CoreHost host(ctx);
    host.set_sink([&ev_mtx, &ev_queue](const std::string& type, const std::string& payload) {
        std::lock_guard<std::mutex> lk(ev_mtx);
        ev_queue.emplace_back(type, payload);
    });

    PluginManager mgr(host);
    mgr.load_all(); // 收尾回调 CoreHost::on_reload → resolve_slots

    // 事件的唯一出口：一份送客户端（CoreHost::emit → 队列 → 推送流），
    // 一份扇出给订阅插件（event.observe，ADR-0001）。两路都在 PluginManager::emit 里，
    // 所以不会有哪条事件只走一边——插件自己调 core_api->emit 也是走这同一条。
    ctx.emit_fn = [&mgr](const std::string& type, const std::string& payload) {
        mgr.emit(type, payload);
    };

    fprintf(stderr, "=== 容器 %zu 个 ===\n", mgr.plugins().size());
    for (const auto& t : mgr.tools())
        fprintf(stderr, "  tool: %s (dangerous=%d)\n", t.name.c_str(), t.def->dangerous);
    for (const auto& c : mgr.commands())
        fprintf(stderr, "  command: /%s\n", c.name.c_str());

    if (argc > 1 && std::string(argv[1]) == "test-tools") {
        return run_tool_test(ctx, mgr);
    }

    // —— 常驻服务模式 ——
    ApprovalCoordinator approval;
    approval.set_emit(ctx.emit_fn); // permission_request 也走队列
    Executor exe(ctx, mgr, approval);
    Agent agent(ctx, mgr, exe);
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
    // 斜杠命令列表（GET /commands，TUI 菜单数据源）。
    // core 内置命令（/new /resume /plugins /model /provider）+ 各容器交出的命令合并。
    // 容器那部分现问现答——向 command.list 拉一遍，core 不存表（ADR-0012）。
    // 与下方 /message 的斜杠命令分支共用同一内置命令集——新增内置命令两处同步（v1 义务）。
    // 内置命令与插件命令撞不上：插件命令带命名空间前缀（<容器>:<命令>），冒号让它
    // 不可能等于裸的内置名。只有进了 plugins.unprefixed 名单才退化成裸名——那是
    // 用户明写的配置动作，真出现再谈怎么处理，不预先设防。
    cbs.on_commands = [&mgr]() {
        json arr = json::array();
        {
            json cmd;
            cmd["name"] = "new";
            cmd["description"] = "新建会话（清空当前对话，旧会话留在盘上）";
            arr.push_back(std::move(cmd));
        }
        {
            json cmd;
            cmd["name"] = "resume";
            cmd["description"] = "查看会话列表（/resume <id> 恢复某个会话）";
            arr.push_back(std::move(cmd));
        }
        {
            json cmd;
            cmd["name"] = "plugins";
            cmd["description"] = "查看插件列表（/plugins enable|disable <name> 启停插件）";
            arr.push_back(std::move(cmd));
        }
        {
            json cmd;
            cmd["name"] = "model";
            cmd["description"] = "查看模型清单（/model <name> 切换主模型）";
            arr.push_back(std::move(cmd));
        }
        {
            json cmd;
            cmd["name"] = "provider";
            cmd["description"] = "查看 Provider 壳（/provider <name> 切换当前 provider）";
            arr.push_back(std::move(cmd));
        }
        // 插件提供的斜杠命令：现问现答（ADR-0012），core 不存表
        for (const auto& c : mgr.commands()) {
            json cmd;
            cmd["name"] = c.name;
            cmd["description"] = c.def->description ? c.def->description : "";
            arr.push_back(std::move(cmd));
        }
        return arr.dump();
    };
    // 斜杠命令的唯一实现。两个门进来（`POST /message` 的 `/` 前缀、`POST /command`），
    // 一份代码——两份实现迟早只改一边，那时同一条命令在两个端点上行为不同，
    // 谁都查不出为什么。命令不启动 agent，直接返回结果。
    // 与 agent 互斥加锁：/new /resume 会换掉对话历史，执行中换等于把地板抽走。
    auto handle_command = [&](const std::string& user_input) -> std::string {
        std::lock_guard<std::mutex> lk(agent_mtx);
        if (user_input == "/new") {
            agent.reset(); // 新建会话：清空历史 + 换一个 JSONL 文件（旧的留在盘上）
            json out;
            out["ok"] = true;
            out["command"] = "new";
            out["data"] = sessions_payload(agent);
            return out.dump();
        }
        // 首空白分词为命令名：/resume[ <id>]、/plugins[ enable|disable <name>]、
        // /model[ <name>]、/provider[ <name>]
        const std::string cmd = user_input.substr(0, user_input.find(' '));
        // 命令参数：命令名之后去掉首尾空白的那一段（无参即空串）
        const auto arg_of = [&user_input, &cmd]() {
            std::string a = user_input.size() > cmd.size() ? user_input.substr(cmd.size() + 1)
                                                           : std::string();
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
        if (cmd == "/provider") {
            // 无参 = 列候选；带名 = 换协议槽指针 + 写回 settings.json。
            // 不重载插件：多个壳早就 init 好了，切换就是换个指针（ADR-0011）
            const std::string name = arg_of();
            json out;
            out["command"] = "provider";
            if (!name.empty()) {
                // 先验候选、再落盘、最后重解析管线：任一步失败则槽位与文件都没动过
                const auto& cands = mgr.providers_of(PLUGIN_CAP_REQUEST_REFINE);
                if (std::find(cands.begin(), cands.end(), name) == cands.end()) {
                    out["ok"] = false;
                    out["error"] = "unknown provider: " + name;
                    return out.dump();
                }
                if (!ctx.config->persist("provider", json(name))) {
                    out["ok"] = false;
                    out["error"] = "写入 settings.json 失败";
                    return out.dump();
                }
                resolve_slots(ctx, mgr); // 换的是管线四段的指针，不重载任何容器
            }
            out["ok"] = true;
            out["data"] = providers_payload(ctx, mgr);
            return out.dump();
        }
        if (cmd == "/model") {
            // 无参 = 列清单；带名 = 切主模型（写回 settings.json，下一次调用即生效）。
            // 只认当前 provider 清单里的模型：交互式选择就该从已知的里挑，打字选中不存在的
            // 只会得到一个端点 400。启动时不校验配置是另一回事（ADR-0009）。
            std::string name = user_input.size() > cmd.size()
                ? user_input.substr(cmd.size() + 1) : "";
            while (!name.empty() && name.back() == ' ') name.pop_back();
            json out;
            out["command"] = "model";
            if (!name.empty()) {
                const json avail = models_payload(ctx, mgr);
                bool known = false;
                for (std::size_t i = 0; i < avail.size(); ++i)
                    if (avail[i]["name"].as_string().value_or("") == name) known = true;
                if (!known) {
                    out["ok"] = false;
                    out["error"] = "unknown model: " + name;
                    return out.dump();
                }
                // 点对点写：只改文件里的 model 这一个键。statusline 帧不在这里推——
                // 事件循环发现载荷变了自己会推（见 on_tick），改配置的地方不必操心通知谁
                if (!ctx.config->persist("model", json(name))) {
                    out["ok"] = false;
                    out["error"] = "写入 settings.json 失败";
                    return out.dump();
                }
            }
            out["ok"] = true;
            out["data"] = models_payload(ctx, mgr);
            return out.dump();
        }
        if (cmd == "/plugins") {
            const std::string rest = user_input.size() > cmd.size()
                ? user_input.substr(cmd.size() + 1) : "";
            if (!rest.empty()) {
                const std::string action = rest.substr(0, rest.find(' '));
                const std::string name = rest.size() > action.size()
                    ? rest.substr(action.size() + 1) : "";
                if (action == "enable") {
                    if (name.empty() || !mgr.enable(name)) {
                        json err;
                        err["ok"] = false;
                        err["command"] = "plugins";
                        err["error"] = "plugin enable failed: " + name;
                        return err.dump();
                    }
                } else if (action == "disable") {
                    if (name.empty() || !mgr.disable(name)) {
                        json err;
                        err["ok"] = false;
                        err["command"] = "plugins";
                        err["error"] = "plugin disable failed: " + name;
                        return err.dump();
                    }
                } else {
                    json err;
                    err["ok"] = false;
                    err["command"] = "plugins";
                    err["error"] = "unknown action: " + action;
                    return err.dump();
                }
            }
            json out;
            out["ok"] = true;
            out["command"] = "plugins";
            out["data"] = plugins_payload(mgr.list());
            return out.dump();
        }
        // 插件提供的命令（ADR-0012：现问现答 + 按名分发，命令定义里不带函数指针）
        {
            const auto cmds = mgr.commands();
            const std::string want = cmd.substr(1); // 去掉 '/'
            const auto it = std::find_if(cmds.begin(), cmds.end(),
                                         [&](const CommandView& c) { return c.name == want; });
            if (it != cmds.end()) {
                auto exec = cap_of<plugin_command_execute_fn>(*it->owner,
                                                              PLUGIN_CAP_COMMAND_EXECUTE);
                if (!exec) return std::string("{\"error\":\"command not executable\"}");
                json args;
                args["args"] = arg_of();
                const auto r = exec.fn(exec.self, it->def->name, args.dump().c_str());
                json out;
                out["ok"] = (r.status == 0);
                out["command"] = want;
                if (r.messages) {
                    const auto parsed = json::parse(r.messages);
                    out["data"] = parsed ? *parsed : json(r.messages);
                    std::free(const_cast<char*>(r.messages)); // 转移：core 释放
                }
                return out.dump();
            }
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
    cbs.on_sessions = [&agent, &agent_mtx]() {
        std::lock_guard<std::mutex> lk(agent_mtx);
        return sessions_payload(agent).dump();
    };
    // POST /session：体带 id = 恢复，体空 = 新建。与 /resume /new 同一套动作，
    // 只是给不走斜杠命令的客户端留的门。
    cbs.on_session = [&agent, &agent_mtx](const std::string& body) {
        auto b = json::parse(body).value_or(json{});
        const std::string id = std::string(b["id"].as_string().value_or(""));
        std::lock_guard<std::mutex> lk(agent_mtx);
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
    // —— 插件管理端点（TUI /plugins 命令的数据源） ——
    // 写操作锁 agent_mtx（对齐 /new 线程纪律：与 agent 运行互斥，避免执行中装卸容器）
    cbs.on_plugins = [&mgr]() { return plugins_payload(mgr.list()).dump(); };
    // 状态栏数据（GET /statusline）：客户端启动时拉一次，之后由 statusline 帧推更新
    cbs.on_statusline = [&ctx, &mgr]() { return statusline_payload(ctx, mgr).dump(); };
    cbs.on_plugin_enable = [&mgr, &agent_mtx](const std::string& name) {
        std::lock_guard<std::mutex> lk(agent_mtx);
        if (mgr.enable(name)) return std::string("{\"ok\":true}");
        json err;
        err["error"] = "plugin enable failed: " + name;
        return err.dump();
    };
    cbs.on_plugin_disable = [&mgr, &agent_mtx](const std::string& name) {
        std::lock_guard<std::mutex> lk(agent_mtx);
        if (mgr.disable(name)) return std::string("{\"ok\":true}");
        json err;
        err["error"] = "plugin disable failed: " + name;
        return err.dump();
    };
    // 事件循环每轮：状态栏盯一眼，把 agent 线程入队的事件 flush 到推送流
    //
    // 状态栏载荷变了就推一帧。载荷本身就是信号——谁改的配置、怎么改的，这里不关心，
    // 改配置的代码路径也就不需要记得通知谁（漏不掉，也不会为无关变更白推）。
    // 不为此单开线程：推帧必须在事件循环线程（ADR-0002），线程只能把活儿再传回来。
    // 启动值取一次，避免首轮推一帧与客户端 GET /statusline 重复的内容。
    std::string last_statusline = statusline_payload(ctx, mgr).dump();
    cbs.on_tick = [&]() {
        if (std::string cur = statusline_payload(ctx, mgr).dump(); cur != last_statusline) {
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
