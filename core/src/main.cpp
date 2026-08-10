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

#include "config.hpp"
#include "extension/loader.hpp"
#include "agent/executor.hpp"
#include "agent/agent.hpp"
#include "server/quic_server.hpp"

using namespace realagent;

static int run_tool_test(CoreContext& ctx) {
    Executor exe(ctx);

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
    Executor exe(ctx);
    Agent agent(ctx, exe);

    // 事件 → stderr（M6 接入推送流）
    ctx.emit_fn = [](const std::string& type, const std::string& payload) {
        fprintf(stderr, "[event] %s %s\n", type.c_str(), payload.c_str());
    };

    QuicServerConfig scfg;
    QuicCallbacks cbs;
    cbs.on_message = [&agent](const std::string& body) {
        // POST /message：body 为 {"message":"..."}
        auto msg = json::parse(body).value_or(json{});
        const std::string user_input = msg["message"].as_string().value_or("");
        if (user_input.empty()) return std::string("{\"error\":\"empty message\"}");
        agent.run(user_input);
        // 返回 agent 最后一条 assistant 文本
        json reply;
        reply["status"] = "ok";
        return reply.dump();
    };

    QuicServer server(scfg);
    server.set_callbacks(cbs);
    server.run(); // 阻塞事件循环

    return 0;
}
