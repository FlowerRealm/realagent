/*
 * test_agent.cpp — agent 的收件箱与线程生命周期（ADR-0019）
 *
 * 这里不测对话质量，测的是 Agent 内化那条线程之后最容易坏的三件事：
 *   - 空收件箱时析构不挂（loop 阻塞在条件变量上，closing_ 得叫得醒它）
 *   - 投进去的消息排队、不丢、按顺序
 *   - 正在跑的时候析构也不挂（先 interrupt 再 join，不等它自己跑完）
 *
 * 端点没配（默认树里 base_url 为空），LLM 调用当场失败——正好：
 * 这里要验的是消息进出收件箱，不是模型回了什么。
 */
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include "agent/agent.hpp"
#include "agent/agents.hpp"
#include "agent/approval.hpp"
#include "agent/context.hpp"
#include "agent/session.hpp"
#include "config.hpp"

namespace fs = std::filesystem;
using namespace realagent;

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

/* 问这个 agent 记了什么：**问盘，不问内存**。idle 的 agent 内存里那份已经还回去了
 * （ADR-0019 §7），盘上那份才是一直在的那一份。 */
static nlohmann::json history(const Agent &a)
{
    nlohmann::json out = nlohmann::json::array();
    Session::read(a.session_dir(), a.session_id(), out);
    return out;
}

/* 等 agent 的历史里攒够 n 条消息。等不到就返回 false，不无限挂着。 */
static bool wait_messages(const Agent &a, size_t n)
{
    for (int i = 0; i < 400; ++i) // 最多 4 秒
    {
        if (history(a).size() >= n) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

int main()
{
    const fs::path home =
        fs::temp_directory_path() / ("realagent-agent-test-" + std::to_string(::getpid()));
    fs::remove_all(home);
    fs::create_directories(home / ".realagent");
    ::setenv("HOME", home.c_str(), 1);

    auto cfg = Config::load();
    if (!cfg)
    {
        printf("  FAIL: 配置加载失败\n");
        return 1;
    }
    CoreContext ctx{.config = &*cfg, .pricing = nullptr, .emit_fn = nullptr};
    ApprovalCoordinator approval;

    printf("== 空收件箱时析构不挂 ==\n");
    {
        // loop 此刻正阻塞在条件变量上。closing_ 叫不醒它的话，这里 join 就是永远
        Agent a(ctx, approval, home.string(), "a1");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(true, "构造后立刻析构，join 得回来");

    printf("== 收件箱排队、不丢、按顺序 ==\n");
    {
        Agent a(ctx, approval, home.string(), "a2");
        a.post("第一条");
        a.post("第二条"); // 第一条还在跑的时候投进来，要排队不要覆盖

        CHECK(wait_messages(a, 2), "两条都被处理，没有一条被吞掉");
        const nlohmann::json m = history(a);
        CHECK(m.size() >= 2 && m[0]["role"] == "user" && m[1]["role"] == "user",
              "两条都以 user 身份入账——凡是从 agent 外面来的输入都是 user");
        CHECK(m.size() >= 2 && m[0]["content"][0]["text"] == "第一条" &&
                  m[1]["content"][0]["text"] == "第二条",
              "顺序就是投递顺序，收件箱是队列不是集合");
    }
    CHECK(true, "处理完之后析构，join 得回来");

    printf("== idle 不把对话历史留在内存里（ADR-0019 §7）==\n");
    {
        Agent a(ctx, approval, home.string(), "a4");
        a.post("记一条");
        CHECK(wait_messages(a, 1), "盘上记下了");
        bool freed = false;
        for (int i = 0; i < 400 && !freed; ++i) // 收件箱空了 loop 才丢，等它一下
        {
            freed = a.resident() == 0;
            if (!freed) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(freed, "跑完且收件箱空了，内存里那份还回去了");
        CHECK(history(a).size() >= 1, "盘上那份一直在——丢的是副本，不是信息");

        // 再投一条：醒来先把历史读回来，接着往下写，不是从空的重来
        a.post("再记一条");
        CHECK(wait_messages(a, 2), "醒来读回了旧历史，新的接在后面");
    }

    printf("== 正在跑的时候析构不挂 ==\n");
    {
        Agent a(ctx, approval, home.string(), "a3");
        a.post("在跑");
        // 不等它跑完就走人：析构要先 interrupt 再 join，不能傻等
    }
    CHECK(true, "带着未处理的消息析构，也 join 得回来");

    printf("== 图：边只能在创建时定，且授不出自己没有的 ==\n");
    {
        Agents pool(ctx, approval);
        std::string err;

        CHECK(pool.create("c1", "", "", {}, {}, err).empty() && !err.empty(), "workdir 必传，没有默认值");

        // 人建的那个：人不是图上的节点，所以这道门建出来的 agent 没有边
        const std::string a = pool.create("c1", home.string(), "", {}, {}, err);
        CHECK(!a.empty(), "客户端建得出第一个 agent");
        CHECK(pool.create("c1", home.string(), "", {a}, {}, err).empty(),
              "人不是图上的节点，从这道门给不出任何边");

        // a 派生 b，把自己写进 b 的入边 → a 收得到 b 的完成通知
        const std::string b = pool.create("c1", home.string(), a, {a}, {}, err);
        CHECK(!b.empty() && err.empty(), "a 派生 b，并把自己写进 b 的入边");
        CHECK(pool.has_edge(a, b), "a → b 建起来了");
        CHECK(!pool.has_edge(b, a), "反向没建就是没有——双向要两条边");

        // b 不认识 a 之外的任何人，所以它派生时不能把别人写进边里
        const std::string c = pool.create("c1", home.string(), a, {}, {}, err);
        CHECK(!c.empty(), "a 再派生一个 c");
        CHECK(pool.create("c1", home.string(), b, {c}, {}, err).empty() && !err.empty(),
              "b 不认识 c，就不能把 c 写进边里——授不出自己没有的能力");

        CHECK(!pool.post("nope", "", "x"), "投给不存在的 agent 返回 false");
        CHECK(pool.post(b, a, "干活"), "投得进去");
        CHECK(pool.list("c1").size() == 3, "清单列出全部——人不受边的约束");
    }

    printf("== 完成通知沿入边逆向回流 ==\n");
    {
        Agents pool(ctx, approval);
        std::string err;
        const std::string a = pool.create("c1", home.string(), "", {}, {}, err);
        const std::string b = pool.create("c1", home.string(), a, {a}, {}, err); // a → b
        Agent *pa = pool.find("c1", a);
        const auto snapshot = [pa] { return history(*pa); };
        const size_t before = snapshot().size();

        pool.post(b, a, "去干活"); // b 跑完会沿入边通知 a
        nlohmann::json m;
        for (int i = 0; i < 400; ++i)
        {
            m = snapshot();
            if (m.size() > before) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(m.size() > before, "b 跑完，a 的收件箱里多了一条");
        const std::string got = m.empty() ? "" : m.back()["content"][0]["text"].get<std::string>();
        CHECK(got.find(b) != std::string::npos && got.find("已完成") != std::string::npos,
              "通知点名是谁跑完了");
        CHECK(!m.empty() && m.back()["role"] == "user",
              "role 是 user——凡是从 agent 外面来的输入都是 user");
    }

    printf("== close：指向它的边一起删 ==\n");
    {
        Agents pool(ctx, approval);
        std::string err;
        const std::string a = pool.create("c1", home.string(), "", {}, {}, err);
        const std::string b = pool.create("c1", home.string(), a, {a}, {}, err);
        CHECK(pool.has_edge(a, b), "先有这条边");
        pool.close(b);
        CHECK(!pool.has_edge(a, b), "b 没了，指向它的边跟着没——边的语义是「我知道它存在」");
        CHECK(pool.find("c1", b) == nullptr && pool.list("c1").size() == 1, "清单里也没有它了");
    }

    printf("== 组：隔离是硬的，跨组一律当不存在（ADR-0021） ==\n");
    {
        Agents pool(ctx, approval);
        std::string err;
        const std::string x = pool.create("c1", home.string(), "", {}, {}, err);
        const std::string y = pool.create("c2", home.string(), "", {}, {}, err);
        CHECK(!x.empty() && !y.empty(), "两个客户端各建一个");

        CHECK(pool.find("c1", x) != nullptr, "自己那组找得到");
        CHECK(pool.find("c2", x) == nullptr,
              "跨组当不存在——区分「不存在」与「不是你的」等于告诉调用方别的组里有什么");
        CHECK(pool.list("c1").size() == 1 && pool.list("c2").size() == 1, "各自只看得见自己那一个");
        CHECK(pool.list("c3").empty(), "没建过组的客户端看到的是空");

        // 边不跨组：c1 的 agent 认不出 c2 的
        CHECK(pool.create("", home.string(), x, {y}, {}, err).empty() && !err.empty(),
              "x 不能把别组的 y 写进边里");

        // spawn 出来的随创建者的组，不可能属于别的组
        const std::string sub = pool.create("", home.string(), x, {x}, {}, err);
        CHECK(!sub.empty(), "x 派生一个");
        CHECK(pool.find("c1", sub) != nullptr && pool.find("c2", sub) == nullptr,
              "派生出来的归创建者那一组");
        CHECK(pool.list("c1").size() == 2, "c1 那组现在两个");
    }

    printf("== close_group：整组一起走，别组不受影响 ==\n");
    {
        Agents pool(ctx, approval);
        std::string err;
        const std::string x = pool.create("c1", home.string(), "", {}, {}, err);
        const std::string sub = pool.create("", home.string(), x, {x}, {}, err);
        const std::string y = pool.create("c2", home.string(), "", {}, {}, err);
        pool.post(sub, x, "干着活呢"); // 让它忙起来，关组要能把在跑的也停下来

        pool.close_group("c1");
        CHECK(pool.list("c1").empty(), "c1 那组连派生出来的一起没了");
        CHECK(pool.find("c1", x) == nullptr && pool.find("c1", sub) == nullptr, "都拆干净了");
        CHECK(pool.list("c2").size() == 1 && pool.find("c2", y) != nullptr, "别组一根汗毛没动");
    }

    printf("== 会话落点由「谁创建的」决定，不是一个参数（ADR-0021） ==\n");
    {
        const fs::path wd = home / "wd";
        fs::create_directories(wd);
        Agents pool(ctx, approval);
        std::string err;
        const std::string top = pool.create("c1", wd.string(), "", {}, {}, err);
        const std::string sub = pool.create("", wd.string(), top, {top}, {}, err);

        const std::string dtop = pool.find("c1", top)->session_dir();
        const std::string dsub = pool.find("c1", sub)->session_dir();
        CHECK(dtop == (wd / ".realagent" / "sessions").string(), "客户端建的落 sessions/");
        CHECK(dsub == (wd / ".realagent" / "sessions" / "sub").string(), "派生的落 sessions/sub/");
        CHECK(dtop != dsub, "两个落点不同——清单只扫顶层，于是 sub 的不进列表");
    }

    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    fs::remove_all(home);
    return failures ? 1 : 0;
}
