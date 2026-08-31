/*
 * test_config.cpp — 配置加载与模型档位单元测试
 *
 * 直接编译 src/config.cpp，不起服务、不碰网络。验证（ADR-0010）：
 *   - 默认树打底：文件不存在 / 空对象 / 只配一部分，都不是错，没配的取默认值
 *   - settings.json 坏了 → load 失败（不静默跳过，读不懂就别往下跑）
 *   - 覆盖生效、两档模型各取各的（不做档位回落）
 *   - 唯一来源：全局 ~/.realagent/settings.json，不看 cwd
 *   - 合并粒度：文件里的键覆盖默认，未提及的默认原样保留
 *   - persist 点对点：只改目标键，用户文件其余原样，默认值不渗进文件
 *   - persist 的两个边界：文件不存在按空对象起头；文件坏了拒绝写入
 *
 * ADR-0023 之后再加三条：
 *   - 端点那五个键住在 provider 里，get() 自己去取；调用方一个字不用改
 *   - provider 不是对象 / 没配 provider → 一律空串，
 *     与"这个键没配"逐字相同——下游不必分辨这两种
 *   - 老格式那种顶层端点键被无视，不写一行兼容代码
 *
 * 隔离：把 HOME 指到临时目录，避免读到用户真实 ~/.realagent/settings.json。
 */
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "config.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
using realagent::Config;
using realagent::ModelTier;

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

/* 临时 HOME：全局配置唯一落点 */
static fs::path make_sandbox(const char *tag)
{
    const fs::path dir = fs::temp_directory_path() /
                         ("realagent-cfg-test-" + std::string(tag) + "-" +
                          std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir / ".realagent");
    return dir;
}

static void write_settings(const fs::path &home, const std::string &body)
{
    std::ofstream f(home / ".realagent" / "settings.json");
    f << body;
}

static void remove_settings(const fs::path &home)
{
    fs::remove(home / ".realagent" / "settings.json");
}

static std::string read_settings_raw(const fs::path &home)
{
    std::ifstream f(home / ".realagent" / "settings.json");
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

/* 端点配置齐活的一份配置：端点那一束住在 provider 里（ADR-0023） */
static const char *k_full =
    R"({"provider":{"protocol":"anthropic-messages",)"
    R"("base_url":"https://example.test/anthropic","api_key":"sk-test",)"
    R"("models":["m-main","m-small"],"model":"m-main","small_model":"m-small"}})";

int main()
{
    const fs::path home = make_sandbox("home");
    ::setenv("HOME", home.c_str(), 1);
    fs::current_path(fs::temp_directory_path()); // cwd 不参与配置读取，随便在哪都一样

    printf("== 默认树打底：没配不是错，配了的才覆盖 ==\n");
    {
        // 文件不存在与空对象是两条不同的读取路径（nullopt vs 空树），都该取到默认
        remove_settings(home);
        const auto none = Config::load();
        CHECK(none.has_value(), "文件不存在 → load 成功（缺配置不是 load 的错）");
        CHECK(none.has_value() && none->get("/permission") == "ask",
              "有默认值的键取到默认");
        CHECK(none.has_value() && none->get("/provider/model").empty(),
              "端点那一束没有默认值（ADR-0017）：model 是空的，不替用户猜一个");
        CHECK(none.has_value() && none->get("/provider/base_url").empty(), "base_url 同上");
        CHECK(none.has_value() && none->get("/provider/protocol").empty(), "protocol 同上");

        write_settings(home, "{}");
        const auto empty = Config::load();
        CHECK(empty.has_value() && empty->get("/permission") == "ask",
              "空对象 → 成功，默认树在（权限默认 ask，不是 allow）");

        // 老格式（端点键写在顶层）在新 core 眼里就是未知键：被无视，不兼容、不迁移。
        // 全世界只有一份老配置，手改一次即可，为它写代码是给只跑一次的路径留永久成本
        write_settings(home, R"({"api_key":"sk-only","base_url":"https://old.test","model":"m-old"})");
        const auto legacy = Config::load();
        CHECK(legacy.has_value(), "老格式 → load 仍成功（load 只管 JSON 读不读得懂）");
        if (legacy)
        {
            CHECK(legacy->provider().empty(), "老格式没有那一束");
            CHECK(legacy->get("/provider/api_key").empty(), "顶层 api_key 进不了那一束");
            CHECK(legacy->get("/provider/base_url").empty(), "顶层 base_url 同上");
            CHECK(legacy->get("/provider/model").empty(),
                  "顶层 model 同上：不兼容、不迁移，也没人为它报错");
            CHECK(legacy->get("/permission") == "ask", "不属于任何路径的键仍留在顶层，取到默认");
        }
    }

    printf("== settings.json 坏了：硬错，不静默跳过 ==\n");
    {
        write_settings(home, "{ not json");
        const auto r = Config::load();
        CHECK(!r, "解析失败 → load 失败");
        if (!r) CHECK(r.error().find("JSON") != std::string::npos, "错误说明是 JSON 问题");
    }

    printf("== 覆盖生效 + 两档模型各取各的 ==\n");
    {
        write_settings(home, k_full);
        const auto r = Config::load();
        CHECK(r.has_value(), "load 成功");
        if (r)
        {
            CHECK(r->get("/provider/base_url") == "https://example.test/anthropic", "文件值覆盖默认");
            CHECK(r->model(ModelTier::Main) == "m-main", "主模型");
            CHECK(r->model(ModelTier::Small) == "m-small", "小模型独立，不是主模型");
            CHECK(r->get("/nonexistent").empty(), "未知键返回空串");
        }
    }

    printf("== 不做档位回落：小模型是独立一档 ==\n");
    {
        // 只配主模型：小模型取默认树里的那个，不是"跟着主模型走"
        write_settings(home,
                       R"({"provider":{"protocol":"anthropic-messages","models":["m-main"],"model":"m-main"}})");
        const auto r = Config::load();
        CHECK(r.has_value() && r->model(ModelTier::Small).empty(),
              "provider 里缺 small_model → 空串，不替它填主模型");
    }

    printf("== cwd 不参与配置：换个目录结果不变 ==\n");
    {
        write_settings(home, k_full);
        const fs::path elsewhere = make_sandbox("elsewhere");
        fs::current_path(elsewhere);
        const auto r = Config::load();
        CHECK(r.has_value() && r->model(ModelTier::Main) == "m-main", "只认 HOME，与 cwd 无关");
        fs::current_path(fs::temp_directory_path());
        fs::remove_all(elsewhere);
    }

    printf("== 合并粒度：文件里的键覆盖默认，未提及的默认原样保留 ==\n");
    {
        // 合并仍然只逐键覆盖顶层，不递归（ADR-0023）：唯一的嵌套是 provider 那个对象，
        // 整个替换就是对的。
        write_settings(home, k_full);
        const auto r = Config::load();
        CHECK(r.has_value(), "load 成功");
        if (r)
        {
            CHECK(r->model(ModelTier::Main) == "m-main", "文件里的 provider 生效");
            CHECK(r->get("/permission") == "ask", "没提及的默认键原样在，不被顶层替换带走");
            CHECK(r->get("/nonexistent").empty(),
                  "没提及、也没有默认值的键就是空——不是丢了默认，是本来就没有");
        }
    }

    printf("== persist 点对点：只改目标键，默认值不渗进文件 ==\n");
    {
        write_settings(home, R"({"permission":"deny","provider":{)"
                             R"("protocol":"anthropic-messages","models":["m-old"],"model":"m-old"}})");
        auto r = Config::load();
        CHECK(r.has_value(), "load 成功");
        if (r)
        {
            json p = r->provider();
            p["model"] = "m-new";
            CHECK(r->persist("provider", p), "persist 成功");
            CHECK(r->model(ModelTier::Main) == "m-new", "内存树已更新");

            const json on_disk = json::parse(read_settings_raw(home), nullptr, false);
            CHECK(!on_disk.is_discarded(), "落盘内容是合法 JSON");
            if (!on_disk.is_discarded())
            {
                CHECK(on_disk["provider"]["model"] == "m-new", "目标键写进去了");
                CHECK(on_disk["permission"] == "deny", "用户原有的键原样保留");
                CHECK(on_disk.size() == 2, "文件里只有用户配过的两个键，默认值没渗进去");
            }
        }
    }

    printf("== persist 的两个边界 ==\n");
    {
        remove_settings(home);
        auto fresh = Config::load();
        CHECK(fresh.has_value() && fresh->persist("permission", json("deny")),
              "文件不存在 → 写成功");
        const json on_disk = json::parse(read_settings_raw(home), nullptr, false);
        CHECK(!on_disk.is_discarded() && on_disk.size() == 1, "按空对象起头，只有这一个键");

        write_settings(home, k_full);
        auto r = Config::load();
        CHECK(r.has_value(), "load 成功");
        if (r)
        {
            write_settings(home, "{ not json");
            CHECK(!r->persist("permission", json("should-not-land")), "文件坏了 → 拒绝写入");
            CHECK(r->model(ModelTier::Main) == "m-main", "内存树没被改（先落盘成功才改内存）");
            CHECK(read_settings_raw(home) == "{ not json", "用户的坏文件原样保留，没被覆盖");
        }
    }

    printf("== 取不到就是空串：不多造一种错误类型 ==\n");
    {
        write_settings(home, R"({"provider":null})");
        const auto r = Config::load();
        CHECK(r.has_value() && r->get("/provider/base_url").empty(),
              "provider 为 null → 空串（与没配逐字相同）");
        CHECK(r.has_value() && r->provider().empty(), "provider() 是空对象，不是 null——null.value() 会抛");

        write_settings(home, R"({})");
        const auto none = Config::load();
        CHECK(none.has_value() && none->get("/provider/base_url").empty(), "没配 provider → 空串");
        CHECK(none.has_value() && none->provider().empty(), "provider() 是空对象，不是 null");

        write_settings(home, R"({"provider":"不是对象"})");
        const auto bad = Config::load();
        CHECK(bad.has_value() && bad->provider().empty(),
              "provider 不是对象 → null，不崩（load 只管 JSON 读不读得懂）");
    }

    fs::current_path(fs::temp_directory_path());
    fs::remove_all(home);
    if (failures == 0)
        printf("\n全部通过\n");
    else
        printf("\n失败 %d 项\n", failures);
    return failures == 0 ? 0 : 1;
}
