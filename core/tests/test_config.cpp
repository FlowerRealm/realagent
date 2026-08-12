/*
 * test_config.cpp — 配置加载与模型档位单元测试
 *
 * 直接编译 src/config.cpp，不起服务、不碰网络。验证：
 *   - 必需键缺失 → load 失败，错误信息点名缺哪个（不回退、不猜默认）
 *   - settings.json 坏了 → load 失败（不静默跳过）
 *   - 全局打底 + 项目级覆盖
 *   - 模型档位：两档各取各的，共用 base_url
 *   - session_dir 是 core 常量：settings.json 写什么都不生效
 *   - persist：配置树整体落盘，重载一致
 *
 * 隔离：把 HOME 与 cwd 指到临时目录，避免读到用户真实 ~/.realagent/settings.json。
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

/* 临时项目根：带 .realagent/ 目录，令 find_project_root 停在此处 */
static fs::path make_sandbox(const char* tag) {
    const fs::path dir = fs::temp_directory_path() /
                         ("realagent-cfg-test-" + std::string(tag) + "-" +
                          std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir / ".realagent");
    return dir;
}

static void write_settings(const fs::path& root, const std::string& body) {
    std::ofstream f(root / ".realagent" / "settings.json");
    f << body;
}

/* 必需键齐活的一份最小配置 */
static const char* k_full =
    R"({"api_key":"sk-test","base_url":"https://example.test/anthropic",)"
    R"("model":"m-main","small_model":"m-small"})";

int main() {
    const fs::path project = make_sandbox("proj");
    const fs::path home = make_sandbox("home"); // 全局配置落这儿，与项目根分开
    ::setenv("HOME", home.c_str(), 1);
    fs::current_path(project);

    printf("== 无配置：直接失败，不回落默认 ==\n");
    {
        write_settings(project, "{}");
        const auto r = Config::load();
        CHECK(!r, "缺必需键 → load 失败");
        if (!r) {
            const std::string& e = r.error();
            CHECK(e.find("api_key") != std::string::npos, "错误点名 api_key");
            CHECK(e.find("base_url") != std::string::npos, "错误点名 base_url");
            CHECK(e.find("model") != std::string::npos, "错误点名 model");
            CHECK(e.find("small_model") != std::string::npos, "错误点名 small_model");
            CHECK(e.find(project.string()) != std::string::npos, "错误指出该往哪个文件补");
        }
    }

    printf("== 缺一个键也不放行 ==\n");
    {
        write_settings(project,
                       R"({"api_key":"sk-test","base_url":"https://example.test",)"
                       R"("model":"m-main"})");
        const auto r = Config::load();
        CHECK(!r, "只缺 small_model → 仍然失败（小模型不回落主模型）");
        if (!r) CHECK(r.error().find("small_model") != std::string::npos, "错误只点名 small_model");
    }

    printf("== 空串等于没配 ==\n");
    {
        write_settings(project,
                       R"({"api_key":"","base_url":"https://example.test",)"
                       R"("model":"m-main","small_model":"m-small"})");
        const auto r = Config::load();
        CHECK(!r, "api_key 为空串 → 失败");
    }

    printf("== settings.json 坏了：硬错，不静默跳过 ==\n");
    {
        write_settings(project, "{ not json");
        const auto r = Config::load();
        CHECK(!r, "解析失败 → load 失败");
        if (!r) CHECK(r.error().find("JSON") != std::string::npos, "错误说明是 JSON 问题");
    }

    printf("== 配齐：各键取值 + 两档模型 ==\n");
    {
        write_settings(project, k_full);
        const auto r = Config::load();
        CHECK(r.has_value(), "配齐 → load 成功");
        if (r) {
            CHECK(r->get("api_key") == "sk-test", "api_key");
            CHECK(r->get("base_url") == "https://example.test/anthropic", "base_url");
            CHECK(r->model(ModelTier::Main) == "m-main", "主模型");
            CHECK(r->model(ModelTier::Small) == "m-small", "小模型独立");
            CHECK(r->get("nonexistent").empty(), "未知键返回空串");
        }
    }

    printf("== 项目级覆盖全局 ==\n");
    {
        write_settings(home, k_full);                              // 全局打底
        write_settings(project, R"({"model":"proj-main"})");       // 项目只改主模型
        const auto r = Config::load();
        CHECK(r.has_value(), "全局补齐必需键 → load 成功");
        if (r) {
            CHECK(r->model(ModelTier::Main) == "proj-main", "项目级覆盖全局主模型");
            CHECK(r->model(ModelTier::Small) == "m-small", "未覆盖项沿用全局");
            CHECK(r->get("api_key") == "sk-test", "凭证沿用全局");
        }
    }

    printf("== session_dir 是常量，不可配置 ==\n");
    {
        write_settings(home, "{}");
        write_settings(project,
                       R"({"api_key":"sk-test","base_url":"https://example.test",)"
                       R"("model":"m-main","small_model":"m-small",)"
                       R"("session_dir":"/tmp/somewhere-else"})");
        const auto r = Config::load();
        CHECK(r.has_value(), "load 成功");
        if (r) CHECK(r->session_dir() == ".realagent/sessions", "session_dir 恒为 core 常量");
    }

    printf("== persist：整树落盘，重载一致 ==\n");
    {
        write_settings(home, "{}");
        write_settings(project, k_full);
        auto r = Config::load();
        CHECK(r.has_value(), "load 成功");
        if (r) {
            r->set("plugins", realagent::json::parse(R"({"disabled":["perm-ask"]})").value());
            CHECK(r->persist(), "persist 成功");
            const auto re = Config::load();
            CHECK(re.has_value(), "重载成功");
            if (re) {
                CHECK(re->model(ModelTier::Small) == "m-small", "落盘后必需键仍在");
                CHECK(re->has("plugins"), "运行时改动落盘");
            }
        }
    }

    fs::current_path(fs::temp_directory_path());
    fs::remove_all(project);
    fs::remove_all(home);
    if (failures == 0)
        printf("\n全部通过\n");
    else
        printf("\n失败 %d 项\n", failures);
    return failures == 0 ? 0 : 1;
}
