/*
 * test_session.cpp — 会话持久化（JSONL）单元测试
 *
 * 直接编译 src/agent/session.cpp + src/config.cpp，不起服务、不碰网络。验证（PLAN.md R9）：
 *   - 落盘即内存那一份：append 什么，resume 就原样读回什么（**同形，不转换**）
 *   - 空会话不建文件：没说过话的会话不该在清单里占一行
 *   - 清单：id / 标题（第一条 user 消息现取）/ 条数（行数）/ 按 mtime 倒序
 *   - 恢复不存在的 id → false，且对象不变（宁可命令没生效，也不能把人扔进空白）
 *   - 坏行跳过而不是整份作废（append-only 文件的末尾可能是断电时写了一半的）
 *   - 一行一条：正文里的换行不许把一条记录劈成两行
 *
 * 隔离：cwd 指到临时目录——会话目录相对 cwd（按项目分家）。
 */
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "agent/session.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
using realagent::Session;

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

/* 造一条 user 消息（与 Agent::run 产出的形状一致） */
static json user_msg(const std::string &text)
{
    return json{{"role", "user"},
                {"content", json::array({json{{"type", "text"}, {"text", text}}})}};
}

/* 帧序列压平成 "type type type"，一眼看得出顺序对不对 */
static std::string types(const json &frames)
{
    std::string s;
    for (const auto &f : frames) s += (s.empty() ? "" : " ") + f["type"].get<std::string>();
    return s;
}

int main()
{
    const fs::path work = fs::temp_directory_path() /
                          ("realagent-session-test-" + std::to_string(::getpid()));
    fs::remove_all(work);
    fs::create_directories(work);
    fs::current_path(work);
    // 会话目录由调用方给（ADR-0019：跟着 agent 的工作目录走，不是进程级的）
    const std::string dir = (work / ".realagent" / "sessions").string();

    printf("== 空会话不建文件 ==\n");
    {
        Session s(dir);
        CHECK(!s.id().empty(), "新会话有 id");
        CHECK(Session::list(dir).empty(), "没写过话 → 清单为空，盘上无文件");
    }

    printf("\n== 落盘即内存那一份：append 什么就读回什么 ==\n");
    std::string first_id;
    {
        Session s(dir);
        first_id = s.id();
        s.append(user_msg("第一句"));
        // thinking 块带 signature：转换层最容易丢的就是这种字段，同形存储不存在这个问题
        const json am{
            {"role", "assistant"},
            {"content", json::array({json{{"type", "thinking"},
                                          {"thinking", "想一想"},
                                          {"signature", "sig-1"}},
                                     json{{"type", "text"}, {"text", "答"}}})}};
        s.append(am);

        Session r(dir);
        json loaded;
        CHECK(r.resume(first_id, loaded), "resume 成功");
        CHECK(loaded.is_array() && loaded.size() == 2, "读回 2 条");
        CHECK(loaded[1].dump() == am.dump(), "assistant 消息逐字节相同（含 signature）");
        CHECK(r.id() == first_id, "resume 后 id 切到目标会话");
    }

    printf("\n== 一行一条：正文换行不许劈开记录 ==\n");
    {
        Session s(dir);
        s.append(user_msg("上一行\n下一行"));
        std::ifstream f(fs::path(".realagent/sessions") / (s.id() + ".jsonl"));
        std::string line;
        int n = 0;
        while (std::getline(f, line)) ++n;
        CHECK(n == 1, "含换行的正文仍只占一行");

        Session r(dir);
        json loaded;
        r.resume(s.id(), loaded);
        CHECK(loaded.size() == 1 && loaded[0]["content"][0]["text"] == "上一行\n下一行",
              "换行原样读回");
    }

    printf("\n== 清单：标题现取、条数即行数、按最近写入倒序 ==\n");
    {
        const auto list = Session::list(dir);
        CHECK(list.size() == 2, "两个会话（空的那个不算）");
        bool found = false;
        for (const auto &e : list)
            if (e.id == first_id)
            {
                found = true;
                CHECK(e.title == "第一句", "标题取自第一条 user 消息");
                CHECK(e.messages == 2, "条数 = 行数");
                CHECK(e.mtime > 0, "mtime 有值");
            }
        CHECK(found, "清单里有第一个会话");
        CHECK(list[0].mtime >= list[1].mtime, "按 mtime 倒序（最近的在前）");
    }

    printf("\n== 恢复不存在的 id：失败且对象不变 ==\n");
    {
        Session s(dir);
        const std::string before = s.id();
        json loaded;
        CHECK(!s.resume("no-such-session", loaded), "resume 返回 false");
        CHECK(s.id() == before, "对象没被改——还是原来那个会话");
    }

    printf("\n== 坏行跳过，不牵连整份会话 ==\n");
    {
        Session s(dir);
        s.append(user_msg("好行一"));
        {
            std::ofstream f(fs::path(".realagent/sessions") / (s.id() + ".jsonl"), std::ios::app);
            f << "{ 这不是 JSON\n"; // 模拟断电时写了一半的末行
        }
        s.append(user_msg("好行二"));

        Session r(dir);
        json loaded;
        CHECK(r.resume(s.id(), loaded), "有坏行仍能恢复");
        CHECK(loaded.size() == 2, "两条好行都在，坏行被跳过");
    }

    printf("\n== Session::to_frames 事件帧回放（与推送流同形，ADR-0020） ==\n");
    {
        CHECK(Session::to_frames(json::array()).empty(), "空数组进，空数组出");
        CHECK(Session::to_frames(json("不是数组")).empty(), "不是数组也不崩，回空");

        const json qa = json::array({
            {{"role", "user"}, {"content", {{{"type", "text"}, {"text", "在吗"}}}}},
            {{"role", "assistant"}, {"content", {{{"type", "text"}, {"text", "在"}}}}},
        });
        const json f = Session::to_frames(qa);
        CHECK(types(f) == "message_start turn_start message_update turn_end", "帧序列与实时流同形");
        CHECK(f[0]["data"]["text"] == "在吗", "用户正文随 message_start 走");
        CHECK(f[2]["data"]["delta"] == "在", "assistant 正文走 message_update 的 delta");

        const json tool = json::array({
            {{"role", "assistant"},
             {"content", {{{"type", "tool_use"}, {"id", "c1"}, {"name", "bash"}, {"input", json::object()}}}}},
            {{"role", "user"},
             {"content", {{{"type", "tool_result"}, {"tool_use_id", "c1"}, {"content", "hi\n"}}}}},
        });
        const json g = Session::to_frames(tool);
        CHECK(types(g) == "turn_start tool_execution_start turn_end tool_output tool_execution_end",
              "工具帧齐全且按实时顺序");
        CHECK(g[4]["data"]["name"] == "bash", "tool_execution_end 补上了工具名");
        CHECK(g[4]["data"]["status"] == 0, "没标 is_error 就是 status 0");
        CHECK(g[3]["data"]["text"] == "hi\n", "工具输出回放成 tool_output");

        const json bad = json::array({
            {{"role", "user"},
             {"content", {{{"type", "tool_result"}, {"tool_use_id", "c9"}, {"content", "boom"}, {"is_error", true}}}}},
        });
        CHECK(Session::to_frames(bad)[1]["data"]["status"] == 1, "is_error 变成 status 1");

        const json think = json::array({
            {{"role", "assistant"},
             {"content", {{{"type", "thinking"}, {"thinking", "嗯"}, {"signature", "sig"}}}}},
        });
        const json t = Session::to_frames(think);
        CHECK(types(t) == "turn_start thinking_start thinking_update thinking_stop turn_end",
              "思考块回放成三帧，与实时流同形");
        CHECK(t[1]["data"]["signature"] == "sig" && t[2]["data"]["delta"] == "嗯", "签名与正文各归各位");
    }

    printf("\n== Session::read_frames 从文件读取并直接转换为事件帧 ==\n");
    {
        const json frames = Session::read_frames(dir, first_id);
        CHECK(!frames.empty(), "read_frames 成功读回已落盘会话并转为帧");
        CHECK(frames[0]["type"] == "message_start" && frames[0]["data"]["text"] == "第一句",
              "首帧为第一句 user 消息");
        CHECK(Session::read_frames(dir, "non-existent-id").empty(), "不存在的会话返回空数组");
    }

    fs::current_path(fs::temp_directory_path());
    fs::remove_all(work);
    if (failures == 0)
        printf("\n全部通过\n");
    else
        printf("\n失败 %d 项\n", failures);
    return failures == 0 ? 0 : 1;
}
