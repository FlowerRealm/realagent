#include "config.hpp"

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
        if (!dir.has_parent_path()) break;
        dir = dir.parent_path();
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

    // 3) env 覆盖（env > settings.json）
    //    约定：ANTHROPIC_API_KEY / DEEPSEEK_BASE_URL / DEEPSEEK_MODEL
    if (const auto v = getenv_or("ANTHROPIC_API_KEY"); !v.empty()) cfg.settings_["api_key"] = v;
    if (const auto v = getenv_or("DEEPSEEK_BASE_URL"); !v.empty()) cfg.settings_["base_url"] = v;
    if (const auto v = getenv_or("DEEPSEEK_MODEL"); !v.empty()) cfg.settings_["model"] = v;

    return cfg;
}

std::string Config::get(std::string_view key, std::string_view default_value) const {
    if (const auto s = settings_[key].as_string()) return *s;
    return std::string(default_value);
}

bool Config::has(std::string_view key) const { return settings_.contains(key); }

std::vector<std::string> Config::extension_dirs() const {
    std::vector<std::string> dirs;
    const std::string home = getenv_or("HOME", ".");
    dirs.push_back(fs::path(home) / ".realagent" / "extensions");
    const std::string root = find_project_root(fs::current_path().string());
    if (!root.empty()) dirs.push_back(fs::path(root) / ".realagent" / "extensions");
    return dirs;
}

std::string Config::session_dir() const { return get("session_dir", ".realagent/sessions"); }

json Config::to_json() const { return settings_; }

} // namespace realagent
