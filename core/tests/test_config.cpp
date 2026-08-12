/*
 * test_config.cpp — 配置与模型档位单元测试
 *
 * 直接编译 src/config.cpp，不起服务、不碰网络。验证：
 *   - 模型档位：small_model 未配置 → 回落主模型；配置了 → 各取各的
 *   - 优先级链：env > settings.json > 内置默认（主模型与小模型同规则）
 *   - persist：env 注入的键不入盘（不把 DEEPSEEK_SMALL_MODEL 烘焙进文件）
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
static fs::path make_sandbox() {
    const fs::path dir = fs::temp_directory_path() /
                         ("realagent-cfg-test-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir / ".realagent");
    return dir;
}

static void write_settings(const fs::path& root, const std::string& body) {
    std::ofstream f(root / ".realagent" / "settings.json");
    f << body;
}

static void clear_env() {
    ::unsetenv("DEEPSEEK_MODEL");
    ::unsetenv("DEEPSEEK_SMALL_MODEL");
    ::unsetenv("DEEPSEEK_BASE_URL");
    ::unsetenv("ANTHROPIC_API_KEY");
}

int main() {
    const fs::path sandbox = make_sandbox();
    ::setenv("HOME", sandbox.c_str(), 1); // 全局 settings 也落在沙箱内（不存在 = 不读）
    fs::current_path(sandbox);

    printf("== 无配置：小模型回落主模型 ==\n");
    {
        clear_env();
        write_settings(sandbox, "{}");
        const Config cfg = Config::load();
        CHECK(cfg.model(ModelTier::Main) == "deepseek-v4-flash", "主模型取内置默认");
        CHECK(cfg.model(ModelTier::Small) == "deepseek-v4-flash", "小模型未配置 → 回落主模型");
    }

    printf("== 只配主模型：小模型跟着主模型走 ==\n");
    {
        clear_env();
        write_settings(sandbox, R"({"model":"deepseek-v4-pro"})");
        const Config cfg = Config::load();
        CHECK(cfg.model(ModelTier::Main) == "deepseek-v4-pro", "主模型取 settings.json");
        CHECK(cfg.model(ModelTier::Small) == "deepseek-v4-pro", "小模型回落到配置的主模型");
    }

    printf("== 两档都配：各取各的 ==\n");
    {
        clear_env();
        write_settings(sandbox,
                       R"({"model":"deepseek-v4-pro","small_model":"deepseek-v4-flash"})");
        const Config cfg = Config::load();
        CHECK(cfg.model(ModelTier::Main) == "deepseek-v4-pro", "主模型独立");
        CHECK(cfg.model(ModelTier::Small) == "deepseek-v4-flash", "小模型独立");
        CHECK(cfg.get("base_url") == "https://api.deepseek.com/anthropic",
              "两档共用端点（档位不换 base_url）");
    }

    printf("== env 覆盖 settings.json ==\n");
    {
        clear_env();
        write_settings(sandbox,
                       R"({"model":"deepseek-v4-pro","small_model":"deepseek-v4-flash"})");
        ::setenv("DEEPSEEK_SMALL_MODEL", "env-small", 1);
        const Config cfg = Config::load();
        CHECK(cfg.model(ModelTier::Small) == "env-small", "DEEPSEEK_SMALL_MODEL 优先于 settings");
        CHECK(cfg.model(ModelTier::Main) == "deepseek-v4-pro", "小模型的 env 不污染主模型");
    }

    printf("== env 注入值不入盘 ==\n");
    {
        clear_env();
        write_settings(sandbox, R"({"model":"deepseek-v4-pro"})");
        ::setenv("DEEPSEEK_SMALL_MODEL", "env-small", 1);
        Config cfg = Config::load();
        CHECK(cfg.persist(), "persist 成功");
        clear_env();
        std::ifstream f(sandbox / ".realagent" / "settings.json");
        const std::string text((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
        CHECK(text.find("env-small") == std::string::npos, "env 注入的 small_model 未写入文件");
        CHECK(text.find("deepseek-v4-pro") != std::string::npos, "settings.json 原有主模型保留");
        // 落盘后重载：小模型键不存在 → 回落主模型
        const Config re = Config::load();
        CHECK(re.model(ModelTier::Small) == "deepseek-v4-pro", "重载后小模型回落主模型");
    }

    fs::current_path(fs::temp_directory_path());
    fs::remove_all(sandbox);
    if (failures == 0)
        printf("\n全部通过\n");
    else
        printf("\n失败 %d 项\n", failures);
    return failures == 0 ? 0 : 1;
}
