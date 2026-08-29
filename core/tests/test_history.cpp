/*
 * test_history.cpp — 会话历史 → 事件帧（ADR-0020）
 *
 * 这段转换存在的理由是「实时看和翻历史看长得一样」，所以要压住的就是那件事：
 * 同一段对话喂进去，出来的帧序列与推送流那份逐帧对得上（PROTOCOL.md 的帧表）。
 *
 * 纯函数，不起服务、不碰盘、不碰网络。
 */
#include <cstdio>
#include <string>

#include "agent/history.hpp"

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

/* 帧序列压平成 "type type type"，一眼看得出顺序对不对 */
static std::string types(const nlohmann::json &frames)
{
    std::string s;
    for (const auto &f : frames) s += (s.empty() ? "" : " ") + f["type"].get<std::string>();
    return s;
}

int main()
{
    printf("== 空历史 ==\n");
    CHECK(history_frames(nlohmann::json::array()).empty(), "空数组进，空数组出");
    CHECK(history_frames(nlohmann::json("不是数组")).empty(), "不是数组也不崩，回空");

    printf("== 一问一答 ==\n");
    const nlohmann::json qa = nlohmann::json::array({
        {{"role", "user"}, {"content", {{{"type", "text"}, {"text", "在吗"}}}}},
        {{"role", "assistant"}, {"content", {{{"type", "text"}, {"text", "在"}}}}},
    });
    const nlohmann::json f = history_frames(qa);
    CHECK(types(f) == "message_start turn_start message_update turn_end", "帧序列与实时流同形");
    CHECK(f[0]["data"]["text"] == "在吗", "用户正文随 message_start 走");
    CHECK(f[2]["data"]["delta"] == "在", "assistant 正文走 message_update 的 delta");

    printf("== 工具调用 ==\n");
    const nlohmann::json tool = nlohmann::json::array({
        {{"role", "assistant"},
         {"content", {{{"type", "tool_use"}, {"id", "c1"}, {"name", "bash"}, {"input", nlohmann::json::object()}}}}},
        {{"role", "user"},
         {"content", {{{"type", "tool_result"}, {"tool_use_id", "c1"}, {"content", "hi\n"}}}}},
    });
    const nlohmann::json g = history_frames(tool);
    CHECK(types(g) == "turn_start tool_execution_start turn_end tool_output tool_execution_end",
          "工具帧齐全且按实时顺序");
    CHECK(g[4]["data"]["name"] == "bash", "tool_execution_end 补上了工具名（结果块里没有）");
    CHECK(g[4]["data"]["status"] == 0, "没标 is_error 就是 status 0");
    CHECK(g[3]["data"]["text"] == "hi\n", "工具输出回放成 tool_output");

    printf("== 失败的工具 ==\n");
    const nlohmann::json bad = nlohmann::json::array({
        {{"role", "user"},
         {"content", {{{"type", "tool_result"}, {"tool_use_id", "c9"}, {"content", "boom"}, {"is_error", true}}}}},
    });
    CHECK(history_frames(bad)[1]["data"]["status"] == 1, "is_error 变成 status 1");

    printf("== 思考块 ==\n");
    const nlohmann::json think = nlohmann::json::array({
        {{"role", "assistant"},
         {"content", {{{"type", "thinking"}, {"thinking", "嗯"}, {"signature", "sig"}}}}},
    });
    const nlohmann::json t = history_frames(think);
    CHECK(types(t) == "turn_start thinking_start thinking_update thinking_stop turn_end",
          "思考块回放成三帧，与实时流同形");
    CHECK(t[1]["data"]["signature"] == "sig" && t[2]["data"]["delta"] == "嗯", "签名与正文各归各位");

    printf("\n%s\n", failures ? "有失败" : "全部通过");
    return failures ? 1 : 0;
}
