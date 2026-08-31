/*
 * test_command.cpp — 斜杠命令里管配置的那两条：/provider 与 /model（ADR-0023）
 *
 * 只验命令层自己的知识，不碰网络、不起服务：
 *   - /provider 与 /model 回同一份载荷（provider + 补好元数据的清单）
 *   - 带 data = 覆盖写，只改 provider 这一个键，碰不到 permission
 *   - /model <name> 是老用法，仍然只认 provider 清单里的名字
 *
 * 隔离：HOME 指到临时目录，读写的都是那份假 settings.json。
 */
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "agent/agent.hpp"
#include "agent/agents.hpp"
#include "agent/approval.hpp"
#include "agent/command.hpp"
#include "agent/context.hpp"
#include "config.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
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

static fs::path g_home;

static void write_settings(const std::string &body)
{
    std::ofstream f(g_home / ".realagent" / "settings.json");
    f << body;
}

static std::string read_settings_raw()
{
    std::ifstream f(g_home / ".realagent" / "settings.json");
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

/* 一份配好的 provider 配置 */
static const char *k_one =
    R"({"provider":{"protocol":"anthropic-messages","base_url":"https://one.test",)"
    R"("api_key":"sk-one","models":["m-a","m-b"],"model":"m-a","small_model":"m-b"}})";

int main()
{
    g_home = fs::temp_directory_path() / ("realagent-cmd-test-" + std::to_string(::getpid()));
    fs::remove_all(g_home);
    fs::create_directories(g_home / ".realagent");
    ::setenv("HOME", g_home.c_str(), 1);
    fs::current_path(fs::temp_directory_path());

    ApprovalCoordinator approval;

    // 每条命令都在自己的 Config 上跑：命令会写盘，下一条要读到新的那份
    auto run = [&](const char *input, const json &data = json()) {
        auto cfg = Config::load();
        if (!cfg)
        {
            printf("  FAIL: 配置加载失败：%s\n", cfg.error().c_str());
            ++failures;
            return json();
        }
        std::string price_err;
        const Pricing pricing = Pricing::load(*cfg, &price_err);
        CoreContext ctx{.config = &*cfg, .pricing = &pricing, .emit_fn = nullptr};
        Agents pool(ctx, approval);
        std::string err;
        const int id = pool.create(g_home.string(), 0, {}, {}, err);
        Agent *a = pool.find(id);
        return json::parse(handle_command(ctx, pool, *a, input, data), nullptr, false);
    };

    printf("== /provider 与 /model 回同一份载荷 ==\n");
    {
        write_settings(k_one);
        const json prov = run("/provider");
        const json mdl = run("/model");
        CHECK(prov.value("ok", false) && mdl.value("ok", false), "两条都 ok");
        CHECK(prov["data"] == mdl["data"], "同一份东西，只是客户端拿去渲染两个面板");
        CHECK(prov["data"]["provider"].is_object(), "provider 对象在");
        CHECK(prov["data"]["provider"]["api_key"] == "sk-one",
              "api_key 明文回去——客户端拿不到旧值就会清空凭证");
        CHECK(prov["data"]["models"].size() == 2, "清单出自 provider 的 models");
        CHECK(prov["data"]["models"][0]["name"] == "m-a" &&
                  prov["data"]["models"][0]["current"] == true,
              "主档标出来了");
        CHECK(prov["data"]["models"][1]["small"] == true, "小档也标出来了");
        CHECK(!prov["data"]["models"][0].contains("owned_by"),
              "数据表里查不到就只剩名字——表是参考资料，不是白名单");
    }

    printf("== 覆盖写只认 provider 这一个键 ==\n");
    {
        // permission 是安全默认，不归这条命令管：放开就等于让 /provider 顺手关掉权限
        write_settings(R"({"permission":"ask","provider":{"protocol":"anthropic-messages"}})");
        const json r = run("/provider", json{{"provider", json{{"protocol", "openai-chat"}}},
                                             {"permission", "allow-all"}});
        CHECK(r.value("ok", false), "多带的键不报错，只是不生效");
        const json on_disk = json::parse(read_settings_raw(), nullptr, false);
        CHECK(!on_disk.is_discarded() && on_disk["permission"] == "ask", "permission 没被改掉");
        CHECK(!on_disk.is_discarded() && on_disk["provider"]["protocol"] == "openai-chat", "provider 被改掉了");

        write_settings(k_one);
        const json empty = run("/provider", json{{"随便什么", 1}});
        CHECK(!empty.value("ok", true), "没带 provider 键 → 说清楚，不静默什么都不做");
        CHECK(read_settings_raw() == k_one, "盘上原样");
    }

    printf("== 一概不校验：配成什么样都存得下 ==\n");
    {
        // 空着就空着，写错就写错。core 不替用户拦——缺了就让它以本来的方式失败：
        // 空 base_url 换回 libcurl 一句 URL 格式错，端点不认的模型名换回一个 400。
        // 多一道校验就多一份会跟真正的失败漂移的真相
        write_settings(k_one);
        const json r = run("/provider", json{{"provider", json{{"protocol", "打错了"}}}});
        CHECK(r.value("ok", false), "protocol 写错 → 照存不误");
        const json on_disk = json::parse(read_settings_raw(), nullptr, false);
        CHECK(!on_disk.is_discarded() && on_disk["provider"]["protocol"] == "打错了",
              "原样落盘");

        const json empty = run("/provider", json{{"provider", json::object()}});
        CHECK(empty.value("ok", false), "空 provider → 也存得下");
        CHECK(empty["data"]["models"].empty(), "清单跟着空，不是错");
    }

    printf("== /model <name>：老用法原样保留 ==\n");
    {
        write_settings(k_one);
        const json bad = run("/model m-c"); // m-c 不在清单里
        CHECK(!bad.value("ok", true), "不在 provider 清单里的名字 → 拒绝（本来就拒）");

        const json ok = run("/model m-b");
        CHECK(ok.value("ok", false), "清单里的另一个 → 成功");
        const json on_disk = json::parse(read_settings_raw(), nullptr, false);
        CHECK(!on_disk.is_discarded() && on_disk["provider"]["model"] == "m-b",
              "写进的是 provider 里那个 model，不是顶层");
    }

    printf("== 没有 provider 时说人话 ==\n");
    {
        write_settings(R"({"provider":null})");
        const json list = run("/model");
        CHECK(list.value("ok", false) && list["data"]["models"].empty(),
              "无参列清单 → 空清单，不是错");
        const json set = run("/model 随便什么");
        CHECK(!set.value("ok", true) && set["error"].get<std::string>().find("/provider") !=
                                            std::string::npos,
              "要改就得先有 provider，报错里直接说去哪儿配");
    }

    fs::current_path(fs::temp_directory_path());
    fs::remove_all(g_home);
    if (failures == 0)
        printf("\n全部通过\n");
    else
        printf("\n失败 %d 项\n", failures);
    return failures == 0 ? 0 : 1;
}
