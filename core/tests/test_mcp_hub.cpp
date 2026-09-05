/*
 * test_mcp_hub.cpp — 读配置、合并、连接复用、拼表（ADR-0023 §4）
 *
 * 不碰网络：对手是 mcp_stub_server。验的是：
 *   - 两处来源，同名**整条覆盖**（不是逐字段合并）
 *   - `{"enabled": false}` 在项目级关掉全局那个
 *   - `type` 非 stdio、`${...}` 模板、坏 JSON —— 各自跳过一个，不牵连别的
 *   - **类型上的错由 json 库判**：缺键、值不是字符串、条目不是对象，报的都是库的原话
 *   - **一份配置一个连接**：两个 Lease 拿到的是同一个 McpClient；参数不同才起第二个进程
 *   - 最后一个 Lease 松手 → 连接关掉（池里存的是 weak_ptr，不自己写计数器）
 *   - **归一**：启动规格是一份 JSON，只剩 name / command / args / env，未知键在解析处落下
 *   - 工具表：名字 `<键>__<原名>`、`_core.dangerous` 一律为真、MCP 的 title/annotations 不收
 *
 * 隔离：HOME 与 workdir 都指到临时目录——全局那一处是从 HOME 算出来的。
 */
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "mcp/mcp.hpp"

namespace fs = std::filesystem;
using realagent::load_mcp_config;
using realagent::McpHub;

static int failures = 0;
#define CHECK(cond, msg)                 \
    do                                   \
    {                                    \
        if (cond)                        \
        {                                \
            printf("  ok: %s\n", msg);   \
        }                                \
        else                             \
        {                                \
            printf("  FAIL: %s\n", msg); \
            ++failures;                  \
        }                                \
    } while (0)

static fs::path g_home, g_work;

static void write_cfg(const fs::path &root, const std::string &json)
{
    fs::create_directories(root / ".realagent");
    std::ofstream(root / ".realagent" / "mcp.json") << json;
}
static void drop_cfg(const fs::path &root)
{
    std::error_code ec;
    fs::remove(root / ".realagent" / "mcp.json", ec);
}
/* 一段指向 stub 的配置。mode 非空时当参数传给 stub。 */
static std::string stub_cfg(const std::string &key, const std::string &mode = "")
{
    std::string args = mode.empty() ? "[]" : "[\"" + mode + "\"]";
    return R"({"mcpServers":{")" + key + R"(":{"command":")" + MCP_STUB_SERVER +
           R"(","args":)" + args + "}}}";
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_home = fs::temp_directory_path() / ("ra_mcp_home_" + std::to_string(getpid()));
    g_work = fs::temp_directory_path() / ("ra_mcp_work_" + std::to_string(getpid()));
    fs::create_directories(g_home);
    fs::create_directories(g_work);
    setenv("HOME", g_home.c_str(), 1);

    printf("== 一个配置都没有 ==\n");
    {
        McpHub hub;
        auto l = hub.open(g_work.string());
        CHECK(l.tools.empty() && l.errors.empty(), "空表、无错（文件不存在不是错）");
    }

    printf("\n== 全局一个，连上并拼表 ==\n");
    write_cfg(g_home, stub_cfg("stub"));
    {
        McpHub hub;
        auto l = hub.open(g_work.string());
        CHECK(l.errors.empty(), "没有错");
        CHECK(l.tools.size() == 2, "两个工具（分页跟到底了）");
        if (l.tools.size() == 2)
        {
            CHECK(l.tools[0]["name"] == "stub__echo", "名字是 <键>__<原名>");
            CHECK(l.tools[0]["_core"]["dangerous"] == true, "MCP 来的一律 dangerous");
            CHECK(l.tools[0]["_core"]["remote_name"] == "echo", "转发名是原名，不带前缀");
            CHECK(l.tools[0].contains("input_schema"), "schema 换成了端点的键名");
            /* schema 原样转发，`$schema` 也在里面。真 server（官方参考实现）发的就是
             * draft-07，而端点收不收 draft-07 **没有验过**（这台机器上没有凭证）。
             * 钉住现状：哪天决定要剥掉 $schema，这条断言会告诉你改了什么。 */
            CHECK(l.tools[0]["input_schema"].value("$schema", std::string()) ==
                      "http://json-schema.org/draft-07/schema#",
                  "inputSchema 原样转发，$schema 一并带过去（端点收不收未验证）");
            CHECK(!l.tools[0].contains("title") && !l.tools[0].contains("annotations"),
                  "title / annotations 不收（端点没有它们的位置）");
        }
    }

    printf("\n== 项目级同名：整条覆盖，不逐字段合并 ==\n");
    write_cfg(g_work, stub_cfg("stub", "badversion")); // 同名，参数不同
    {
        McpHub hub;
        auto l = hub.open(g_work.string());
        CHECK(l.tools.empty(), "近的那条赢了（它连不上，所以没有工具）");
        CHECK(l.errors.size() == 1 && l.errors[0].find("-32022") != std::string::npos,
              "错的是近的那条，不是全局那条");
    }

    printf("\n== 项目级 {\"enabled\": false} 关掉全局那个 ==\n");
    write_cfg(g_work, R"({"mcpServers":{"stub":{"enabled":false}}})");
    {
        const auto cfg = load_mcp_config(g_work.string());
        CHECK(cfg.servers.empty(), "关掉的不进清单");
        CHECK(cfg.errors.empty(), "关掉的条目不必是一条能跑的规格，没有 command 也不报错");
    }

    printf("\n== 归一：只剩四个键，未知键落下 ==\n");
    write_cfg(g_work,
              R"({"mcpServers":{"stub":{"//":"这是注释","command":"/bin/true","timeout":9,)"
              R"("cwd":"/tmp"}}})");
    {
        const auto cfg = load_mcp_config(g_work.string());
        CHECK(cfg.servers.size() == 1 && cfg.servers[0].size() == 4,
              "name / command / args / env，多一个都没有");
        CHECK(!cfg.servers[0].contains("timeout"),
              "未知键不进启动规格——那份对象 dump 出来就是连接的键，"
              "留着一个被忽略的 timeout 会让同一个 server 起两个进程");
    }

    printf("\n== 跳过一个不牵连别的 ==\n");
    write_cfg(g_work, R"({"mcpServers":{
        "remote": {"type":"http","url":"https://example.com/mcp"},
        "tpl":    {"command":"npx","args":["-y","x","${workspaceFolder}"]}
    }})");
    {
        const auto cfg = load_mcp_config(g_work.string());
        CHECK(cfg.servers.size() == 1 && cfg.servers[0]["name"] == "stub",
              "全局那个 stub 照常在（近处两条都坏，但坏的是它们自己）");
        bool http = false, tpl = false;
        for (const auto &e : cfg.errors)
        {
            if (e.find("type=\"http\"") != std::string::npos) http = true;
            if (e.find("${workspaceFolder}") != std::string::npos &&
                e.find("绝对路径") != std::string::npos)
                tpl = true;
        }
        CHECK(http, "type 非 stdio：说的是「本项目不支持」");
        CHECK(tpl, "${...}：点名 + 告诉他改成绝对路径，而不是安静地传过去");
    }

    printf("\n== 坏 JSON：记一条，不拒绝启动，且说清坏在哪 ==\n");
    write_cfg(g_work, "{ this is not json");
    {
        const auto cfg = load_mcp_config(g_work.string());
        CHECK(cfg.servers.size() == 1, "全局那份照常用");
        CHECK(cfg.errors.size() == 1 && cfg.errors[0].find(g_work.string()) != std::string::npos,
              "错误说的是哪个文件");
        /* 这句是 json 库写的，不是我们拼的：带行号列号和「本来期待什么」。
         * 手写一句「不是合法 JSON」的话，用户还得自己去数括号。 */
        CHECK(cfg.errors[0].find("parse error") != std::string::npos &&
                  cfg.errors[0].find("line 1") != std::string::npos,
              "库的原话：parse error at line 1, column ...");
        printf("  %s\n", cfg.errors[0].c_str());
    }

    printf("\n== 形状不对：库自己指出是哪个键、实际是什么类型 ==\n");
    write_cfg(g_work, R"({"mcpServers":{
        "nocmd": {"args":["x"]},
        "badarg": {"command":"/bin/true","args":[1,2]},
        "notobj": 5,
        "badon": {"enabled":"yes","command":"/bin/true"}
    }})");
    {
        const auto cfg = load_mcp_config(g_work.string());
        CHECK(cfg.servers.size() == 1 && cfg.servers[0]["name"] == "stub",
              "四条都坏，全局那个 stub 照常在");
        std::string all;
        for (const auto &e : cfg.errors) all += e + "\n";
        CHECK(all.find("key 'command' not found") != std::string::npos,
              "缺 command：库点名说是哪个键，不是我们手写的一句「缺 command」");
        // args 里混了数字必须当场爆出来：悄悄丢掉的话，用户拿到一个不带参数的 server
        CHECK(all.find("type must be string, but is number") != std::string::npos,
              "args 里混了数字：不再悄悄丢掉");
        CHECK(all.find("notobj") != std::string::npos, "条目根本不是对象：也接得住，不炸整个 core");
        /* `value("enabled", true)` 撞上字符串会 throw，那条路上必须有 catch：
         * core 是常驻服务，一份手滑的配置不能带走所有 agent。 */
        CHECK(all.find("badon") != std::string::npos,
              "enabled 不是 bool：接住，不是抛穿常驻服务");
        printf("%s", all.c_str());
    }

    printf("\n== 一份配置一个连接 ==\n");
    drop_cfg(g_work);
    {
        McpHub hub;
        auto a = hub.open(g_work.string());
        auto b = hub.open(g_work.string());
        CHECK(a.conns.size() == 1 && b.conns.size() == 1, "各拿到一个");
        CHECK(a.conns[0].get() == b.conns[0].get(), "同一份配置 = 同一个进程，不起第二个");

        std::weak_ptr<realagent::McpClient> watch = a.conns[0];
        a.conns.clear();
        CHECK(!watch.expired(), "还有人拿着，连接不关");
        b.conns.clear();
        CHECK(watch.expired(), "最后一个松手，连接自己关（池里是 weak_ptr，没有计数器）");
    }

    printf("\n== 参数不同 = 两个进程 ==\n");
    write_cfg(g_work, stub_cfg("other")); // 另一个名字、另一份规格
    {
        McpHub hub;
        auto l = hub.open(g_work.string());
        CHECK(l.conns.size() == 2, "两个连接");
        CHECK(l.conns[0].get() != l.conns[1].get(), "是两个不同的进程");
        CHECK(l.tools.size() == 4, "两份工具表拼在一起");
        bool has_other = false, has_stub = false;
        for (const auto &t : l.tools)
        {
            if (t["name"] == "other__echo") has_other = true;
            if (t["name"] == "stub__echo") has_stub = true;
        }
        CHECK(has_other && has_stub, "同一个原名，靠前缀分得开");
    }

    printf("\n== 工具名规范化：抄端点第一方客户端那一行 ==\n");
    write_cfg(g_work, stub_cfg("srv", "weirdnames"));
    drop_cfg(g_home);
    {
        McpHub hub;
        auto l = hub.open(g_work.string());
        std::vector<std::string> names;
        for (const auto &t : l.tools) names.push_back(t["name"].get<std::string>());
        auto has = [&](const std::string &n) {
            return std::find(names.begin(), names.end(), n) != names.end();
        };
        CHECK(has("srv__greet"), "本来就合法的原样不动");
        CHECK(has("srv__greet__with_Icons_"),
              "空格和括号换成 _（Go SDK 示例里真实存在的名字）");
        CHECK(has("srv__elicit__form_"), "同一条规则，没有第二种写法");
        for (const auto &t : l.tools)
            if (t["name"] == "srv__greet__with_Icons_")
                CHECK(t["_core"]["remote_name"] == "greet (with Icons)",
                      "转发名是**原名**，一个字符没改——规范化只发生在给模型看的那一面");
        CHECK(names.size() == 3, "三个都在");
    }

    std::error_code ec;
    fs::remove_all(g_home, ec);
    fs::remove_all(g_work, ec);
    printf("\n%s\n", failures == 0 ? "全部通过" : "有失败");
    return failures == 0 ? 0 : 1;
}
