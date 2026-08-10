/*
 * main.cpp — realagent core 入口
 *
 * 首版行为（M0-M2 验证）：
 *   realagent-core            → 加载配置 + 插件，打印注册表
 *   realagent-core test-tools → 验证工具执行链路（read/edit/bash + 权限检查）
 * M3/M5 接入 agent loop 与 QUIC/HTTP3 服务后演进为常驻进程。
 */
#include <cstdio>
#include <string>

#include "config.hpp"
#include "extension/loader.hpp"
#include "agent/executor.hpp"

static int run_tool_test(realagent::CoreContext& ctx) {
    realagent::Executor exe(ctx);

    // read
    {
        const auto r = exe.execute("read", "{\"file_path\":\".realagent/extensions/core-tools/plugin.json\"}");
        printf("read: status=%d messages=%s\n\n", r.status, r.messages.c_str());
    }
    // edit：创建文件（+x-0 语义）
    {
        const auto r = exe.execute("edit", "{\"file_path\":\"/tmp/ra_edit_test.txt\",\"new_string\":\"hello realagent\\n\"}");
        printf("edit(create): status=%d messages=%s\n", r.status, r.messages.c_str());
    }
    // edit：追加（old_string 空）
    {
        const auto r = exe.execute("edit", "{\"file_path\":\"/tmp/ra_edit_test.txt\",\"old_string\":\"\",\"new_string\":\"line2\\n\"}");
        printf("edit(append): status=%d messages=%s\n", r.status, r.messages.c_str());
    }
    // read 回读验证
    {
        const auto r = exe.execute("read", "{\"file_path\":\"/tmp/ra_edit_test.txt\"}");
        printf("read(verify): status=%d messages=%s\n", r.status, r.messages.c_str());
    }
    // bash
    {
        const auto r = exe.execute("bash", "{\"command\":\"echo core-tools-ok && ls /tmp/ra_edit_test.txt\"}");
        printf("bash: status=%d messages=%s\n", r.status, r.messages.c_str());
    }
    // 未知工具（应拒绝）
    {
        const auto r = exe.execute("nope", "{}");
        printf("unknown: status=%d messages=%s\n", r.status, r.messages.c_str());
    }
    return 0;
}

int main(int argc, char** argv) {
    using namespace realagent;

    const auto cfg = Config::load();
    CoreContext ctx;
    ctx.config = &cfg;
    ctx.emit_fn = [](const std::string& type, const std::string& payload) {
        fprintf(stderr, "[event] %s %s\n", type.c_str(), payload.c_str());
    };

    PluginManager mgr(ctx);
    mgr.load_all();
    for (const auto& p : mgr.plugins()) ctx.all_plugins.push_back(p.get());

    fprintf(stderr, "=== 插件 %zu 个 ===\n", mgr.plugins().size());
    fprintf(stderr, "=== 工具 ===\n");
    for (const auto& [name, t] : ctx.tools)
        fprintf(stderr, "  - %s (dangerous=%d)\n", name.c_str(), t.def.dangerous);

    if (argc > 1 && std::string(argv[1]) == "test-tools") {
        printf("\n===== 工具执行链路测试 =====\n");
        return run_tool_test(ctx);
    }

    fprintf(stderr, "\n用法: realagent-core [test-tools]\n");
    mgr.shutdown();
    return 0;
}
