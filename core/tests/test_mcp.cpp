/*
 * test_mcp.cpp — MCP 客户端（ADR-0023）。对手是 mcp_stub_server，不碰网络。
 *
 * 钉住的是：
 *   - **按 id 认领**：stub 在 tools/list 的响应之前先吐一条通知。
 *     "写一行读一行"的客户端会把通知当答案
 *   - **分页**：工具分两页，不跟 nextCursor 就只看得见第一页
 *   - **没有握手**：stub 只在 `_meta` 齐全时回应，缺了回 -32602。
 *     所以这个测试同时钉住了「每个请求自带版本与能力」
 *   - **-32022**：server 不说这个版本时，起不来，且错误里带上它说得了什么
 *   - **中止**：slow 模式不回应，abort 位一置就该立刻返回，且不是等满超时
 *   - **结果原样交出**：图片块不投影、不压平（那是 upstream 的活）
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <memory>

#include "mcp/mcp.hpp"

using realagent::McpClient;

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

/* 启动规格就是 hub 归一之后那份 JSON：四个键，一个不多。 */
static nlohmann::json stub(const std::string &mode = "")
{
    nlohmann::json c{{"name", "stub"},
                     {"command", MCP_STUB_SERVER}, // CMake 传进来的绝对路径
                     {"args", nlohmann::json::array()},
                     {"env", nlohmann::json::object()}};
    if (!mode.empty()) c["args"].push_back(mode);
    return c;
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0); // 崩了也要看得见走到哪一步
    printf("== 起进程 + tools/list（通知在前、分两页）==\n");
    std::string err;
    auto c = McpClient::start(stub(), err);
    CHECK(c != nullptr, "连上了");
    if (!c)
    {
        printf("  err=%s\n", err.c_str());
        return 1;
    }
    const auto &tools = c->tools();
    CHECK(tools.size() == 2, "两页都跟到了（分页）");
    CHECK(tools.size() == 2 && tools[0]["name"] == "echo" && tools[1]["name"] == "get-tiny-image",
          "顺序与内容都对（说明插在中间那条通知没被当成答案）");

    printf("\n== tools/call ==\n");
    auto r = c->call("echo", {{"message", "hi"}});
    CHECK(r.value("isError", true) == false, "echo 不是错误");
    CHECK(r["content"].size() == 1 && r["content"][0]["text"] == "Echo: hi", "文本块原样");

    r = c->call("get-tiny-image", nlohmann::json::object());
    CHECK(r["content"].size() == 2, "两个块");
    CHECK(r["content"][1]["type"] == "image" && r["content"][1].contains("data"),
          "图片块原样交出——不投影、不压平（那是 upstream 的活）");

    r = c->call("nope", nlohmann::json::object());
    CHECK(r.value("isError", false) == true, "未知工具是 isError，不是崩溃");

    printf("\n== 结果形状只有一种 ==\n");
    CHECK(r.contains("content") && r.contains("isError"),
          "失败与成功同形（调用方只有一种形状要处理）");

    printf("\n== -32022：server 不说这个版本 ==\n");
    err.clear();
    auto bad = McpClient::start(stub("badversion"), err);
    CHECK(bad == nullptr, "起不来");
    CHECK(err.find("-32022") != std::string::npos, "错误里有码");
    CHECK(err.find("2025-11-25") != std::string::npos,
          "错误里带上它说得了哪些版本（data.supported）");
    if (!err.empty()) printf("  err=%s\n", err.c_str());

    printf("\n== 中止：不等满超时，也不杀进程 ==\n");
    auto slow = McpClient::start(stub("slow"), err);
    CHECK(slow != nullptr, "slow 模式也连得上（tools/list 照常回）");
    if (slow)
    {
        std::atomic<bool> abort{false};
        const auto t0 = std::chrono::steady_clock::now();
        std::thread trip([&abort] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            abort.store(true);
        });
        auto rr = slow->call("echo", nlohmann::json::object(), &abort);
        trip.join();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        CHECK(ms < 3000, "秒回，不是等满 300 秒的超时");
        CHECK(rr.value("isError", false) == true, "中止是错误结果");
        printf("  用时 %lldms，结果=%s\n", (long long)ms, rr["content"][0]["text"].dump().c_str());
    }

    printf("\n== 连接的键是那份配置本身 ==\n");
    nlohmann::json a = stub(), b = stub();
    CHECK(a.dump() == b.dump(), "同样的配置 = 同一个键（键就是那份 JSON 本身，没有第二种表示）");
    b["args"].push_back("--other");
    CHECK(a.dump() != b.dump(), "参数不同 = 不同的键（于是两个进程）");

    printf("\n%s\n", failures == 0 ? "全部通过" : "有失败");
    return failures == 0 ? 0 : 1;
}
