/*
 * main.cpp — realagent core 入口（M0/M1 阶段：配置 + 插件加载验证）
 *
 * 首版行为：加载配置 → 扫描加载插件 → 打印注册表 → 退出。
 * M3/M5 接入 agent loop 与 QUIC/HTTP3 服务后演进为常驻进程。
 */
#include <cstdio>
#include <string>

#include "config.hpp"
#include "extension/loader.hpp"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    using namespace realagent;

    const auto cfg = Config::load();
    CoreContext ctx;
    ctx.config = &cfg;

    // 事件出口：M5 接入推送流，首版打日志
    ctx.emit_fn = [](const std::string& type, const std::string& payload) {
        fprintf(stderr, "[event] %s %s\n", type.c_str(), payload.c_str());
    };

    PluginManager mgr(ctx);
    mgr.load_all();

    fprintf(stderr, "\n=== 插件 %zu 个 ===\n", mgr.plugins().size());
    fprintf(stderr, "=== 工具 ===\n");
    for (const auto& [name, t] : ctx.tools)
        fprintf(stderr, "  - %s (dangerous=%d)\n", name.c_str(), t.def.dangerous);
    fprintf(stderr, "=== 命令 ===\n");
    for (const auto& [name, c] : ctx.commands)
        fprintf(stderr, "  - /%s\n", name.c_str());

    mgr.shutdown();
    return 0;
}
