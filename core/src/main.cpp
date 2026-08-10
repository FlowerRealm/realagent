/*
 * main.cpp — realagent core 入口（常驻服务）
 *
 * M0-M2 验证模式：
 *   realagent-core test-tools → 工具执行链路验证（read/edit/bash + 权限）
 * M5+ 常驻模式（默认）：
 *   realagent-core → 加载插件 + 启动 QUIC/HTTP3 服务（PROTOCOL.md 端点）
 *   POST /message → 启动 agent loop（DeepSeek）
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

int main(int argc, char** argv) {
    const auto cfg = Config::load();
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
    cbs.on_message = [&](const std::string& body) {
        // POST /message：body 为 {"message":"..."}。agent 在独立线程运行
        // （ADR-0002 线程模型）：不阻塞事件循环，审批等待期间仍能收裁决。
        auto msg = json::parse(body).value_or(json{});
        const std::string user_input = msg["message"].as_string().value_or("");
        if (user_input.empty()) return std::string("{\"error\":\"empty message\"}");
        std::thread([&agent, &agent_mtx, user_input]() {
            std::lock_guard<std::mutex> lk(agent_mtx);
            agent.run(user_input);
        }).detach();
        return std::string("{\"status\":\"processing\"}");
    };
    cbs.on_approval_response = [&approval](const std::string& id, bool allow) {
        approval.respond(id, allow);
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
