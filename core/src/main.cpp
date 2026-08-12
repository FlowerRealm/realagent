/*
 * main.cpp — realagent core 入口（常驻服务）
 *
 * M0-M2 验证模式：
 *   realagent-core test-tools → 工具执行链路验证（read/edit/bash + 权限）
 * M5+ 常驻模式（默认）：
 *   realagent-core → 加载插件 + 启动 QUIC/HTTP3 服务（PROTOCOL.md 端点）
 *   POST /message → 启动 agent loop
 */
#include <cstdio>
#include <string>
#include <thread>
#include <deque>
#include <mutex>

#include "config.hpp"
#include "extension/loader.hpp"
#include "agent/executor.hpp"
#include "agent/agent.hpp"
#include "server/quic_server.hpp"

using namespace realagent;

static int run_tool_test(CoreContext& ctx) {
    ApprovalCoordinator approval; // 测试模式无客户端，ASK 会 30s 超时按 deny
    Executor exe(ctx, approval);

    {
        const auto r = exe.execute("read", "{\"file_path\":\".realagent/extensions/core-tools/plugin.json\"}");
        printf("read: status=%d messages=%s\n\n", r.status, r.messages.c_str());
    }
    {
        const auto r = exe.execute("edit", "{\"file_path\":\"/tmp/ra_edit_test.txt\",\"new_string\":\"hello realagent\\n\"}");
        printf("edit(create): status=%d messages=%s\n", r.status, r.messages.c_str());
    }
    {
        const auto r = exe.execute("edit", "{\"file_path\":\"/tmp/ra_edit_test.txt\",\"old_string\":\"\",\"new_string\":\"line2\\n\"}");
        printf("edit(append): status=%d messages=%s\n", r.status, r.messages.c_str());
    }
    {
        const auto r = exe.execute("read", "{\"file_path\":\"/tmp/ra_edit_test.txt\"}");
        printf("read(verify): status=%d messages=%s\n", r.status, r.messages.c_str());
    }
    {
        const auto r = exe.execute("bash", "{\"command\":\"echo core-tools-ok && ls /tmp/ra_edit_test.txt\"}");
        printf("bash: status=%d messages=%s\n", r.status, r.messages.c_str());
    }
    return 0;
}

/* GET /plugins 响应：PluginInfo → JSON。字段名与顺序由 BOOST_DESCRIBE_STRUCT
 * 的字段表生成，即结构体声明本身——契约与结构体不可能再走散。 */
static json plugins_payload(const std::vector<PluginInfo>& list) { return to_json(list); }

int main(int argc, char** argv) {
    // 配置是刚需：缺键/配置文件坏了就地退出，不带残缺配置往下跑
    auto loaded = Config::load();
    if (!loaded) {
        fprintf(stderr, "[config] %s\n", loaded.error().c_str());
        return 1;
    }
    auto cfg = std::move(*loaded); // 非 const：CoreContext::config 需写路径（插件禁用清单 persist）
    CoreContext ctx;
    ctx.config = &cfg;

    PluginManager mgr(ctx);
    mgr.load_all();
    for (const auto& p : mgr.plugins()) ctx.all_plugins.push_back(p.get());

    fprintf(stderr, "=== 插件 %zu 个 ===\n", mgr.plugins().size());
    for (const auto& [name, t] : ctx.tools)
        fprintf(stderr, "  tool: %s (dangerous=%d)\n", name.c_str(), t.def.dangerous);

    if (argc > 1 && std::string(argv[1]) == "test-tools") {
        return run_tool_test(ctx);
    }

    // —— 常驻服务模式 ——
    // 线程安全事件队列：agent 线程 emit → 事件循环 on_tick flush 到推送流
    // （ADR-0002 线程模型：quiche 非线程安全，推送必须在事件循环线程）
    std::mutex ev_mtx;
    std::deque<std::pair<std::string, std::string>> ev_queue;
    ctx.emit_fn = [&ev_mtx, &ev_queue](const std::string& type, const std::string& payload) {
        std::lock_guard<std::mutex> lk(ev_mtx);
        ev_queue.emplace_back(type, payload);
    };

    ApprovalCoordinator approval;
    approval.set_emit(ctx.emit_fn); // permission_request 也走队列
    Executor exe(ctx, approval);
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
    // 斜杠命令列表（GET /commands，TUI 菜单数据源）。
    // 内置 new/resume + 插件注册命令（ctx.commands，loader register_command 注册表）合并，
    // 与下方 /message 的斜杠命令分支共用同一内置命令集——新增内置命令两处同步（v1 义务）。
    cbs.on_commands = [&ctx]() {
        json arr = json::array();
        {
            json cmd;
            cmd["name"] = "new";
            cmd["description"] = "新建会话，清空当前对话";
            arr.push_back(std::move(cmd));
        }
        {
            json cmd;
            cmd["name"] = "resume";
            cmd["description"] = "查看当前会话消息数";
            arr.push_back(std::move(cmd));
        }
        {
            json cmd;
            cmd["name"] = "plugins";
            cmd["description"] = "查看插件列表（/plugins enable|disable <name> 启停插件）";
            arr.push_back(std::move(cmd));
        }
        // 插件注册的斜杠命令（名称不带 '/'，core 是唯一真相源）
        for (const auto& [name, ce] : ctx.commands) {
            json cmd;
            cmd["name"] = name;
            cmd["description"] = ce.def.description ? ce.def.description : "";
            arr.push_back(std::move(cmd));
        }
        return arr.dump();
    };
    cbs.on_message = [&](const std::string& body) {
        // POST /message：body 为 {"message":"..."}。agent 在独立线程运行
        // （ADR-0002 线程模型）：不阻塞事件循环，审批等待期间仍能收裁决。
        auto msg = json::parse(body).value_or(json{});
        const std::string user_input = msg["message"].as_string().value_or("");
        if (user_input.empty()) return std::string("{\"error\":\"empty message\"}");

        // 斜杠命令：会话管理（v1 内置 /new /resume，与 agent 互斥加锁；
        // session-manager 插件化后置）。插件管理（/plugins）同锁。
        // 命令不启动 agent，直接返回结果。
        if (user_input[0] == '/') {
            std::lock_guard<std::mutex> lk(agent_mtx);
            if (user_input == "/new") {
                agent.reset(); // 新建会话：清空对话历史
                return std::string("{\"ok\":true,\"command\":\"new\"}");
            }
            if (user_input == "/resume") {
                // v1 仅报会话统计；完整 JSONL 恢复后置（PLAN.md R9）
                const int n = (int)agent.messages().size();
                return std::string("{\"ok\":true,\"command\":\"resume\",\"messages\":" +
                                   std::to_string(n) + "}");
            }
            // 首空白分词为命令名：/plugins[ enable|disable <name>]
            const std::string cmd = user_input.substr(0, user_input.find(' '));
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
            return std::string("{\"error\":\"unknown command\"}");
        }

        std::thread([&agent, &agent_mtx, user_input]() {
            std::lock_guard<std::mutex> lk(agent_mtx);
            agent.run(user_input);
        }).detach();
        return std::string("{\"status\":\"processing\"}");
    };
    cbs.on_interrupt = [&agent, &approval]() {
        agent.interrupt();
        approval.cancel_all();
    };
    cbs.on_approval_response = [&approval](const std::string& id, bool allow) {
        approval.respond(id, allow);
    };
    // —— 插件管理端点（TUI /plugins 命令的数据源） ——
    // 写操作锁 agent_mtx（对齐 /new 线程纪律：与 agent 运行互斥，避免执行中改注册表）
    cbs.on_plugins = [&mgr]() { return plugins_payload(mgr.list()).dump(); };
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
    // 事件循环每轮：把 agent 线程入队的事件 flush 到推送流
    cbs.on_tick = [&]() {
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
