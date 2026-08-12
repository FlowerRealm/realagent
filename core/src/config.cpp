#include "config.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>

namespace realagent {
namespace fs = std::filesystem;

std::string getenv_or(std::string_view name, std::string_view fallback) {
    if (const char* v = std::getenv(std::string(name).c_str()); v != nullptr)
        return std::string(v);
    return std::string(fallback);
}

std::string find_project_root(std::string_view start_dir) {
    fs::path dir(start_dir.empty() ? fs::current_path() : fs::path(start_dir));
    // 向上找 .realagent/ 或 .git
    for (;;) {
        if (fs::exists(dir / ".realagent") || fs::exists(dir / ".git"))
            return dir.string();
        const fs::path parent = dir.parent_path();
        // 已到根：libc++ 下 path("/").parent_path() == "/"（has_parent_path 恒真），
        // 必须显式判无进展，否则死循环。
        if (parent.empty() || parent == dir) break;
        dir = parent;
    }
    return "";
}

// 从文件读取 json；文件不存在/解析失败返回 nullopt
static std::optional<json> load_json_file(const fs::path& path) {
    std::ifstream f(path);
    if (!f) return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return json::parse(text);
}

// 解析键 → env 变量名（get 时实时读取，保证 env 恒为最高优先级）
static const char* config_env_name(std::string_view key) {
    if (key == "api_key") return "ANTHROPIC_API_KEY";
    if (key == "base_url") return "DEEPSEEK_BASE_URL";
    if (key == "model") return "DEEPSEEK_MODEL";
    if (key == "small_model") return "DEEPSEEK_SMALL_MODEL";
    return nullptr;
}

// 解析键的内置默认（small_model 无内置默认：未配置回落主模型，见 Config::model）
static std::string config_builtin(std::string_view key) {
    if (key == "base_url") return "https://api.deepseek.com/anthropic";
    if (key == "model") return "deepseek-v4-flash";
    return "";
}

Config Config::load() {
    Config cfg;
    const std::string home = getenv_or("HOME", ".");

    // 1) 默认值
    cfg.settings_ = json{};
    cfg.settings_["base_url"] = "https://api.deepseek.com/anthropic";
    cfg.settings_["model"] = "deepseek-v4-flash";
    cfg.settings_["session_dir"] = ".realagent/sessions";

    // 2) settings.json 覆盖（全局先，项目级后——项目覆盖全局）
    if (auto j = load_json_file(fs::path(home) / ".realagent" / "settings.json")) {
        for (const auto& k : j->keys()) cfg.settings_[k] = (*j)[k];
    }
    const std::string root = find_project_root(fs::current_path().string());
    if (!root.empty()) {
        if (auto j = load_json_file(fs::path(root) / ".realagent" / "settings.json")) {
            for (const auto& k : j->keys()) cfg.settings_[k] = (*j)[k];
        }
    }

    // 3) env 覆盖（env > settings.json），记录注入键 → persist() 不写盘
    //    约定：ANTHROPIC_API_KEY / DEEPSEEK_BASE_URL / DEEPSEEK_MODEL
    if (const auto v = getenv_or("ANTHROPIC_API_KEY"); !v.empty()) {
        cfg.settings_["api_key"] = v;
        cfg.env_keys_.push_back("api_key");
    }
    if (const auto v = getenv_or("DEEPSEEK_BASE_URL"); !v.empty()) {
        cfg.settings_["base_url"] = v;
        cfg.env_keys_.push_back("base_url");
    }
    if (const auto v = getenv_or("DEEPSEEK_MODEL"); !v.empty()) {
        cfg.settings_["model"] = v;
        cfg.env_keys_.push_back("model");
    }
    if (const auto v = getenv_or("DEEPSEEK_SMALL_MODEL"); !v.empty()) {
        cfg.settings_["small_model"] = v;
        cfg.env_keys_.push_back("small_model");
    }

    return cfg;
}

std::string Config::get(std::string_view key, std::string_view default_value) const {
    std::lock_guard<std::mutex> lk(*mutex_);
    // 解析键：env > 平铺字段 > 内置默认
    if (const char* env = config_env_name(key)) {
        if (const auto v = getenv_or(env); !v.empty()) return v;
        if (const auto s = settings_[key].as_string()) return *s;
        return config_builtin(key);
    }
    if (const auto s = settings_[key].as_string()) return *s;
    return std::string(default_value);
}

bool Config::has(std::string_view key) const {
    std::lock_guard<std::mutex> lk(*mutex_);
    return settings_.contains(key);
}

std::string Config::model(ModelTier tier) const {
    // 小模型未配置 = 回落主模型（不设独立默认，配置里少一档也照跑）
    if (tier == ModelTier::Small) {
        if (auto m = get("small_model"); !m.empty()) return m;
    }
    return get("model");
}

void Config::set(std::string_view key, const json& v) {
    std::lock_guard<std::mutex> lk(*mutex_);
    settings_[key] = v;
}

bool Config::persist_locked() {
    // 项目级 .realagent/settings.json（无项目根时退回 cwd）
    std::string root = find_project_root(fs::current_path().string());
    if (root.empty()) root = fs::current_path().string();
    const fs::path dir = fs::path(root) / ".realagent";

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        fprintf(stderr, "[config] persist: 创建目录失败 %s\n", ec.message().c_str());
        return false;
    }

    // 以当前合并树为准整体写出；env 注入键不入盘（避免烘焙 ANTHROPIC_API_KEY 等）
    json out;
    for (const auto& k : settings_.keys()) {
        if (std::find(env_keys_.begin(), env_keys_.end(), k) != env_keys_.end()) continue;
        out[k] = settings_[k];
    }

    // tmp + rename 原子写
    const fs::path target = dir / "settings.json";
    const fs::path tmp = dir / "settings.json.tmp";
    {
        std::ofstream f(tmp);
        if (!f) {
            fprintf(stderr, "[config] persist: 无法写 %s\n", tmp.c_str());
            return false;
        }
        f << out.dump() << "\n";
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        fprintf(stderr, "[config] persist: rename 失败 %s\n", ec.message().c_str());
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool Config::persist() {
    std::lock_guard<std::mutex> lk(*mutex_);
    return persist_locked();
}

// —— 既有 API（原语义不变，仅加锁） ——

std::vector<std::string> Config::extension_dirs() const {
    std::vector<std::string> dirs;
    const std::string home = getenv_or("HOME", ".");
    dirs.push_back(fs::path(home) / ".realagent" / "extensions");
    const std::string root = find_project_root(fs::current_path().string());
    if (!root.empty()) dirs.push_back(fs::path(root) / ".realagent" / "extensions");
    return dirs;
}

std::string Config::session_dir() const { return get("session_dir", ".realagent/sessions"); }

json Config::to_json() const {
    std::lock_guard<std::mutex> lk(*mutex_);
    return settings_;
}

} // namespace realagent
