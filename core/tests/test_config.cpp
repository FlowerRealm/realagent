/*
 * test_config.cpp — 配置加载与模型档位单元测试
 *
 * 直接编译 src/config.cpp，不起服务、不碰网络。验证（ADR-0010）：
 *   - 默认树打底：文件不存在 / 空对象 / 只配一部分，都不是错，没配的取默认值
 *   - settings.json 坏了 → load 失败（不静默跳过，读不懂就别往下跑）
 *   - 覆盖生效、两档模型各取各的；small_model 空就是空串（core 不回落主模型）
 *   - 唯一来源：全局 ~/.realagent/settings.json，不看 cwd
 *   - session_dir 是 core 常量：settings.json 写什么都不生效
 *   - 合并粒度：对象递归合并（嵌套默认值不被顶层替换带走），数组整个替换
 *   - persist 点对点：只改目标键，用户文件其余原样，默认值不渗进文件
 *   - persist 的两个边界：文件不存在按空对象起头；文件坏了拒绝写入
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
using realagent::Config;
using realagent::ModelTier;
using realagent::json;

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

/* 临时 HOME：全局配置唯一落点 */
static fs::path make_sandbox(const char* tag) {
    const fs::path dir = fs::temp_directory_path() /
                         ("realagent-cfg-test-" + std::string(tag) + "-" +
                          std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir / ".realagent");
    return dir;
}

static void write_settings(const fs::path& home, const std::string& body) {
    std::ofstream f(home / ".realagent" / "settings.json");
    f << body;
}

static void remove_settings(const fs::path& home) {
    fs::remove(home / ".realagent" / "settings.json");
}

static std::string read_settings_raw(const fs::path& home) {
    std::ifstream f(home / ".realagent" / "settings.json");
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

/* 四个协议链配置齐活的一份配置 */
static const char* k_full =
    R"({"api_key":"sk-test","base_url":"https://example.test/anthropic",)"
    R"("model":"m-main","small_model":"m-small"})";

int main() {
    const fs::path home = make_sandbox("home");
    ::setenv("HOME", home.c_str(), 1);
    fs::current_path(fs::temp_directory_path()); // cwd 不参与配置读取，随便在哪都一样

    printf("== 默认树打底：没配不是错，配了的才覆盖 ==\n");
    {
        // 文件不存在与空对象是两条不同的读取路径（nullopt vs 空树），都该取到默认
        remove_settings(home);
        const auto none = Config::load();
        CHECK(none.has_value() && none->get("model").empty(), "文件不存在 → 成功，取默认空串");

        write_settings(home, "{}");
        const auto empty = Config::load();
        CHECK(empty.has_value() && empty->has("plugins"), "空对象 → 成功，默认树的 plugins 节在");

        write_settings(home, R"({"api_key":"sk-only"})");
        const auto partial = Config::load();
        CHECK(partial.has_value(), "只配一个键 → 成功（没有必需键这回事）");
        if (partial) {
            CHECK(partial->get("api_key") == "sk-only", "配了的用文件里的值");
            CHECK(partial->get("base_url").empty(),
                  "没配的取默认空串（不带供应商身份，也不用假 URL 占位）");
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
        if (r) {
            CHECK(r->get("base_url") == "https://example.test/anthropic", "文件值覆盖默认");
            CHECK(r->model(ModelTier::Main) == "m-main", "主模型");
            CHECK(r->model(ModelTier::Small) == "m-small", "小模型独立，不是主模型");
            CHECK(r->get("nonexistent").empty(), "未知键返回空串");
        }
    }

    printf("== core 不做档位回落：small_model 空就是空串 ==\n");
    {
        write_settings(home, R"({"model":"m-main"})");
        const auto r = Config::load();
        CHECK(r.has_value() && r->model(ModelTier::Small).empty(),
              "缺 small_model → 空串下传，core 不替它填主模型");
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

    printf("== session_dir 是常量，不可配置 ==\n");
    {
        write_settings(home, R"({"session_dir":"/tmp/somewhere-else"})");
        const auto r = Config::load();
        CHECK(r.has_value() && r->session_dir() == ".realagent/sessions",
              "settings.json 写它不生效");
    }

    printf("== 合并粒度：对象递归合并，数组整个替换 ==\n");
    {
        // 默认树里 plugins 下有 disabled；文件只写 plugins.extra，
        // 顶层整键替换会带走 disabled，递归合并不会
        write_settings(home, R"({"plugins":{"extra":1}})");
        const auto r = Config::load();
        CHECK(r.has_value(), "load 成功");
        if (r) {
            const json p = r->to_json()["plugins"];
            CHECK(p["disabled"].is_array(), "嵌套默认值没被顶层替换带走");
            CHECK(p["extra"].as_int64().value_or(0) == 1, "文件里的嵌套键生效");
        }

        write_settings(home, R"({"plugins":{"disabled":["perm-ask"]}})");
        const auto r2 = Config::load();
        CHECK(r2.has_value() && r2->to_json()["plugins"]["disabled"].size() == 1,
              "数组整个替换，不与默认空数组做追加");
    }

    printf("== persist 点对点：只改目标键，默认值不渗进文件 ==\n");
    {
        write_settings(home, R"({"api_key":"sk-test","model":"m-old"})");
        auto r = Config::load();
        CHECK(r.has_value(), "load 成功");
        if (r) {
            CHECK(r->persist("model", json("m-new")), "persist 成功");
            CHECK(r->model(ModelTier::Main) == "m-new", "内存树已更新");

            const auto on_disk = json::parse(read_settings_raw(home));
            CHECK(on_disk.has_value(), "落盘内容是合法 JSON");
            if (on_disk) {
                CHECK((*on_disk)["model"].as_string().value_or("") == "m-new", "目标键写进去了");
                CHECK((*on_disk)["api_key"].as_string().value_or("") == "sk-test",
                      "用户原有的键原样保留");
                // size==2 一并覆盖了 small_model / plugins 等默认值没被写进来
                CHECK(on_disk->size() == 2, "文件里只有用户配过的两个键，默认值没渗进去");
            }
        }
    }

    printf("== persist 的两个边界 ==\n");
    {
        remove_settings(home);
        auto fresh = Config::load();
        CHECK(fresh.has_value() && fresh->persist("model", json("m-fresh")), "文件不存在 → 写成功");
        const auto on_disk = json::parse(read_settings_raw(home));
        CHECK(on_disk.has_value() && on_disk->size() == 1, "按空对象起头，只有这一个键");

        write_settings(home, k_full);
        auto r = Config::load();
        CHECK(r.has_value(), "load 成功");
        if (r) {
            write_settings(home, "{ not json");
            CHECK(!r->persist("model", json("m-should-not-land")), "文件坏了 → 拒绝写入");
            CHECK(r->model(ModelTier::Main) == "m-main", "内存树没被改（先落盘成功才改内存）");
            CHECK(read_settings_raw(home) == "{ not json", "用户的坏文件原样保留，没被覆盖");
        }
    }

    fs::current_path(fs::temp_directory_path());
    fs::remove_all(home);
    if (failures == 0)
        printf("\n全部通过\n");
    else
        printf("\n失败 %d 项\n", failures);
    return failures == 0 ? 0 : 1;
}
