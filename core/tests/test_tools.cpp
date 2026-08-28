/*
 * test_tools.cpp — 内置工具与权限裁决单元测试
 *
 * 直接编译 src/tools/tools.cpp + src/agent/executor.cpp + src/agent/approval.cpp，
 * 不起服务、不碰网络。这里守的是 ADR-0016 真正换掉的那半边：
 * 从前 read/edit/bash 是一个 C 容器、两种权限是两个动态库，现在是静态表 + 一个 switch。
 *
 * 验证：
 *   - 工具清单：三个工具、危险位、查不到的名字返回 nullptr
 *   - read：正常读、文件不存在、缺参数
 *   - edit：不存在则创建 / 空 old_string 追加 / 替换命中 / old_string 找不到
 *   - bash：stdout 回传、退出码透传、缺参数
 *   - 权限（ADR-0005 / ADR-0016）：allow-all 放行、deny 拒绝、ask 真等裁决、
 *     认不出的值按 ask 处理（写错配置该多问一句，不该多放一次行）
 *   - 中止（ADR-0002 R8）：执行前的中止标记撞得上，跑着的 bash 打得断，reset 抹得掉
 *
 * 隔离：HOME 指到临时目录（Config 只认 ~/.realagent/settings.json），
 * 文件操作全在临时目录里，cwd 也指过去。
 */
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "agent/approval.hpp"
#include "agent/executor.hpp"
#include "config.hpp"
#include "context.hpp"
#include "tools/tools.hpp"

namespace fs = std::filesystem;
using namespace realagent;
using nlohmann::json;

static int failures = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (cond) {                                                             \
            printf("  ok: %s\n", msg);                                          \
        } else {                                                                \
            printf("  FAIL: %s\n", msg);                                        \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

static fs::path g_home;

static void write_settings(const std::string& body) {
    std::ofstream f(g_home / ".realagent" / "settings.json");
    f << body;
}

/* 按 permission 值造一份配置（其余键取默认） */
static Config config_with(const std::string& permission) {
    write_settings(R"({"permission":")" + permission + R"("})");
    auto c = Config::load();
    if (!c) {
        printf("  FAIL: 配置加载失败 %s\n", c.error().c_str());
        ++failures;
        std::exit(1);
    }
    return *c;
}

static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static json call(const std::string& name, const std::string& params) {
    return run_tool("call-1", name, params, nullptr); // emit 为空：工具不推实时帧也照跑
}

/* 结果 json 的三个字段：{"status", "output"}（executor 再加 "interrupted"） */
static int st(const json& r) { return r.value("status", -1); }
static std::string msg(const json& r) { return r.value("output", std::string()); }
static bool intr(const json& r) { return r.value("interrupted", false); }

/* 参数里的路径要进 JSON 字符串，临时目录路径里不含需要转义的字符 */
static std::string q(const fs::path& p) { return "\"" + p.string() + "\""; }

int main() {
    g_home = fs::temp_directory_path() / ("realagent-tools-test-" + std::to_string(::getpid()));
    fs::remove_all(g_home);
    fs::create_directories(g_home / ".realagent");
    ::setenv("HOME", g_home.c_str(), 1);
    fs::current_path(g_home);

    printf("== 工具清单 ==\n");
    {
        CHECK(tool_defs().size() == 3, "三个内置工具");
        const ToolDef* r = find_tool("read");
        const ToolDef* e = find_tool("edit");
        const ToolDef* b = find_tool("bash");
        CHECK(r && !r->dangerous, "read 是只读工具，不触发权限检查点");
        CHECK(e && e->dangerous, "edit 危险");
        CHECK(b && b->dangerous, "bash 危险");
        CHECK(find_tool("core-tools_bash") == nullptr,
              "没有命名空间前缀这回事了（ADR-0016）");
        CHECK(st(call("nope", "{}")) != 0, "未知工具返回错误，不是空成功");
    }

    printf("== read ==\n");
    {
        const fs::path f = g_home / "r.txt";
        { std::ofstream o(f); o << "hello\nworld\n"; }
        const auto ok = call("read", R"({"file_path":)" + q(f) + "}");
        CHECK(st(ok) == 0 && msg(ok) == "hello\nworld\n", "读到原文");

        const auto missing = call("read", R"({"file_path":)" + q(g_home / "nope.txt") + "}");
        CHECK(st(missing) != 0 && msg(missing).find("cannot open") != std::string::npos,
              "文件不存在 → 报错，不是空内容");

        const auto noarg = call("read", "{}");
        CHECK(st(noarg) != 0 && msg(noarg).find("file_path") != std::string::npos,
              "缺 file_path → 报错点名缺的是哪个");
    }

    printf("== edit（+x-0 = 创建） ==\n");
    {
        const fs::path f = g_home / "e.txt";
        fs::remove(f);
        const auto created = call("edit", R"({"file_path":)" + q(f) + R"(,"new_string":"a\n"})");
        CHECK(st(created) == 0 && msg(created).find("created") != std::string::npos,
              "文件不存在 → 创建（报的是 created，不是 appended）");
        CHECK(slurp(f) == "a\n", "创建的内容就是 new_string");

        const auto appended =
            call("edit", R"({"file_path":)" + q(f) + R"(,"old_string":"","new_string":"b\n"})");
        CHECK(st(appended) == 0 && msg(appended).find("appended") != std::string::npos,
              "空 old_string → 追加");
        CHECK(slurp(f) == "a\nb\n", "追加在末尾，原内容不动");

        const auto edited =
            call("edit", R"({"file_path":)" + q(f) + R"(,"old_string":"a","new_string":"X"})");
        CHECK(st(edited) == 0 && slurp(f) == "X\nb\n", "命中 → 只换第一处");

        const auto miss =
            call("edit", R"({"file_path":)" + q(f) + R"(,"old_string":"zzz","new_string":"X"})");
        CHECK(st(miss) != 0 && msg(miss).find("not found") != std::string::npos,
              "old_string 找不到 → 报错");
        CHECK(slurp(f) == "X\nb\n", "报错时文件原样，没被写坏");

        const auto noarg = call("edit", R"({"file_path":)" + q(f) + "}");
        CHECK(st(noarg) != 0 && msg(noarg).find("new_string") != std::string::npos,
              "缺 new_string → 报错");
    }

    printf("== bash ==\n");
    {
        const auto ok = call("bash", R"({"command":"echo tools-ok"})");
        CHECK(st(ok) == 0 && msg(ok) == "tools-ok\n", "stdout 原样回传");

        const auto rc = call("bash", R"({"command":"exit 3"})");
        CHECK(st(rc) == 3, "退出码透传（非零即错）");

        const auto noarg = call("bash", "{}");
        CHECK(st(noarg) != 0 && msg(noarg).find("command") != std::string::npos,
              "缺 command → 报错");

        // B1（ADR-0017）：只接 stdout 的话，失败命令回给模型的是"退出码非零，无话可说"
        const auto err = call("bash", R"({"command":"echo BOOM >&2; exit 7"})");
        CHECK(st(err) == 7, "退出码照旧透传");
        CHECK(msg(err).find("BOOM") != std::string::npos,
              "stderr 也回传——报错原文是模型判断该不该重试的唯一依据");

        const auto both = call("bash", R"({"command":"echo one; echo two >&2; echo three"})");
        CHECK(msg(both) == "one\ntwo\nthree\n",
              "两条流合流，顺序就是人在终端里看见的顺序");
    }

    printf("== read 的截断边界 ==\n");
    {
        // 恰好读满与真被截断，从前读回来一模一样（gcount 都是上限），
        // 于是刚好读满的文件被无辜削三个字节
        const auto write_n = [](const fs::path& p, std::size_t n) {
            std::ofstream o(p, std::ios::binary);
            o << std::string(n, 'x');
        };
        const fs::path exact = g_home / "exact.txt";
        const fs::path over = g_home / "over.txt";
        write_n(exact, 50000);
        write_n(over, 50001);
        const auto r_exact = call("read", R"({"file_path":)" + q(exact) + "}");
        const auto r_over = call("read", R"({"file_path":)" + q(over) + "}");
        CHECK(msg(r_exact).size() == 50000 && msg(r_exact).back() == 'x',
              "恰好读满 → 原样回传，不加省略号");
        CHECK(msg(r_over).size() == 50000 && msg(r_over).substr(50000 - 3) == "...",
              "真的更大 → 截断并标出来");
        CHECK(msg(r_exact) != msg(r_over), "两种情况现在区分得开");
    }

    printf("== 权限裁决（一个配置键，一个 switch） ==\n");
    {
        // allow-all：危险工具直接跑，没有任何问询
        Config cfg = config_with("allow-all");
        ApprovalCoordinator ap;
        int asked = 0;
        ap.set_emit([&asked](const std::string& t, const std::string&) {
            if (t == "permission_request") ++asked;
        });
        CoreContext ctx{.config = &cfg, .emit_fn = nullptr};
        Executor exe(ctx, ap);
        const auto r = exe.execute("c1", "bash", R"({"command":"echo allow"})");
        CHECK(st(r) == 0 && msg(r) == "allow\n", "allow-all → 危险工具照跑");
        CHECK(asked == 0, "allow-all → 一句都不问");
    }
    {
        // deny：危险工具一律拒，只读工具不受影响
        Config cfg = config_with("deny");
        ApprovalCoordinator ap;
        int asked = 0;
        ap.set_emit([&asked](const std::string& t, const std::string&) {
            if (t == "permission_request") ++asked;
        });
        CoreContext ctx{.config = &cfg, .emit_fn = nullptr};
        Executor exe(ctx, ap);
        const auto r = exe.execute("c2", "bash", R"({"command":"echo nope"})");
        CHECK(st(r) != 0 && msg(r).find("permission policy") != std::string::npos,
              "deny → 拒绝，理由说明是策略拒的");
        CHECK(asked == 0, "deny → 不问人（问了也白问）");

        const fs::path f = g_home / "r.txt";
        const auto ro = exe.execute("c3", "read", R"({"file_path":)" + q(f) + "}");
        CHECK(st(ro) == 0, "deny 只管危险工具，read 照读");
    }
    {
        // ask：真等裁决。裁决在 emit 回调里同步回传——await 是先登记后发帧，
        // 收帧的人立刻应答也接得住（不必靠另起线程与 sleep 来碰运气）
        Config cfg = config_with("ask");
        ApprovalCoordinator ap;
        std::string seen_tool;
        ap.set_emit([&ap, &seen_tool](const std::string& t, const std::string& payload) {
            if (t != "permission_request") return;
            const json ev = json::parse(payload);
            seen_tool = ev["tool"];
            ap.respond(ev["id"], true);
        });
        CoreContext ctx{.config = &cfg, .emit_fn = nullptr};
        Executor exe(ctx, ap);
        const auto r = exe.execute("c4", "bash", R"({"command":"echo asked"})");
        CHECK(seen_tool == "bash", "ask → 发出 permission_request，点名是哪个工具");
        CHECK(st(r) == 0 && msg(r) == "asked\n", "用户放行 → 跑");
    }
    {
        Config cfg = config_with("ask");
        ApprovalCoordinator ap;
        ap.set_emit([&ap](const std::string& t, const std::string& payload) {
            if (t != "permission_request") return;
            const json ev = json::parse(payload);
            ap.respond(ev["id"], false);
        });
        CoreContext ctx{.config = &cfg, .emit_fn = nullptr};
        Executor exe(ctx, ap);
        const auto r = exe.execute("c5", "bash", R"({"command":"echo denied"})");
        CHECK(st(r) != 0 && msg(r).find("denied by user") != std::string::npos,
              "用户拒绝 → 不跑，理由是人拒的（与策略拒的分开）");
    }
    {
        // 认不出的值按 ask：配置写错该多问一句，不该多放一次行
        Config cfg = config_with("yolo");
        ApprovalCoordinator ap;
        int asked = 0;
        ap.set_emit([&ap, &asked](const std::string& t, const std::string& payload) {
            if (t != "permission_request") return;
            ++asked;
            const json ev = json::parse(payload);
            ap.respond(ev["id"], false);
        });
        CoreContext ctx{.config = &cfg, .emit_fn = nullptr};
        Executor exe(ctx, ap);
        const auto r = exe.execute("c6", "bash", R"({"command":"echo yolo"})");
        CHECK(asked == 1, "permission 值认不出 → 走 ask，不是放行");
        CHECK(st(r) != 0, "拒绝生效");
    }

    printf("== 中止（ADR-0002 R8） ==\n");
    {
        Config cfg = config_with("allow-all");
        ApprovalCoordinator ap;
        CoreContext ctx{.config = &cfg, .emit_fn = nullptr};
        Executor exe(ctx, ap);

        // 手上没有在跑的：标记留给紧随其后的那次 execute 撞上
        exe.interrupt();
        const auto after = exe.execute("i1", "bash", R"({"command":"echo should-not-run"})");
        CHECK(intr(after) && st(after) != 0 && msg(after).find("should-not-run") == std::string::npos,
              "中止后紧接着的一次 execute 直接拒掉，命令没跑");

        exe.reset();
        const auto back = exe.execute("i2", "bash", R"({"command":"echo back"})");
        CHECK(st(back) == 0 && !intr(back), "reset 抹掉中止痕迹，下一轮照常");

        // 跑着的 bash 打得断：sleep 30 必须在 1 秒内被打掉
        const auto t0 = std::chrono::steady_clock::now();
        json r;
        std::thread worker([&] { r = exe.execute("i3", "bash", R"({"command":"sleep 30"})"); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        exe.interrupt();
        worker.join();
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        CHECK(elapsed < std::chrono::seconds(5), "在跑的 bash 被打断，不是等它自己跑完");
        CHECK(intr(r), "结果标出这次是被中止的（与工具自己失败不是一回事）");
    }

    fs::current_path(fs::temp_directory_path());
    fs::remove_all(g_home);
    printf(failures == 0 ? "\n全部通过\n" : "\n%d 项失败\n", failures);
    return failures == 0 ? 0 : 1;
}
