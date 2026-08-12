#include "config.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace realagent {
namespace fs = std::filesystem;

namespace {

// 会话落盘路径：core 自己的实现细节，不是配置项
constexpr std::string_view kSessionDir = ".realagent/sessions";

// 必需键：缺一个就不启动。core 不猜端点、不猜模型
constexpr std::string_view kRequired[] = {"api_key", "base_url", "model", "small_model"};

fs::path settings_path(const fs::path& dir) { return dir / ".realagent" / "settings.json"; }

// 项目根（无则退回 cwd）——配置写入与项目级读取共用同一落点
fs::path project_dir() {
    const std::string root = find_project_root(fs::current_path().string());
    return root.empty() ? fs::current_path() : fs::path(root);
}

// 合入一份 settings.json：文件不存在 = 跳过；存在但读不了/解析不了 = 硬错
std::expected<void, std::string> merge_file(json& into, const fs::path& path) {
    if (!fs::exists(path)) return {};
    std::ifstream f(path);
    if (!f) return std::unexpected(path.string() + " 打不开");
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const auto j = json::parse(text);
    if (!j) return std::unexpected(path.string() + " 不是合法 JSON");
    for (const auto& k : j->keys()) into[k] = (*j)[k];
    return {};
}

std::string join(const std::vector<std::string_view>& items, const char* sep) {
    std::string out;
    for (const auto& s : items) {
        if (!out.empty()) out += sep;
        out += s;
    }
    return out;
}

} // namespace

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

std::vector<std::string_view> Config::required_keys() {
    return {std::begin(kRequired), std::end(kRequired)};
}

std::expected<Config, std::string> Config::load() {
    Config cfg;
    cfg.settings_ = json{};

    // 全局打底，项目级覆盖
    const fs::path global = settings_path(getenv_or("HOME", "."));
    if (auto r = merge_file(cfg.settings_, global); !r) return std::unexpected(r.error());
    const fs::path project = settings_path(project_dir());
    if (project != global) {
        if (auto r = merge_file(cfg.settings_, project); !r) return std::unexpected(r.error());
    }

    // 必需键一次报全：别让用户补一个跑一次
    // （走 const 引用取值：非 const operator[] 会给缺失键插 null，污染配置树）
    const json& tree = cfg.settings_;
    std::vector<std::string_view> missing;
    for (const auto& k : kRequired) {
        if (tree[k].as_string().value_or("").empty()) missing.push_back(k);
    }
    if (!missing.empty()) {
        return std::unexpected("配置缺必需键 [" + join(missing, ", ") + "]，补进 " +
                               project.string() + "（必需：" +
                               join(required_keys(), " / ") + "）");
    }
    return cfg;
}

std::string Config::get(std::string_view key) const {
    std::lock_guard<std::mutex> lk(*mutex_);
    return settings_[key].as_string().value_or("");
}

bool Config::has(std::string_view key) const {
    std::lock_guard<std::mutex> lk(*mutex_);
    return settings_.contains(key);
}

std::string Config::model(ModelTier tier) const {
    return get(tier == ModelTier::Small ? "small_model" : "model");
}

void Config::set(std::string_view key, const json& v) {
    std::lock_guard<std::mutex> lk(*mutex_);
    settings_[key] = v;
}

bool Config::persist_locked() {
    const fs::path target = settings_path(project_dir());

    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
        fprintf(stderr, "[config] persist: 创建目录失败 %s\n", ec.message().c_str());
        return false;
    }

    // tmp + rename 原子写（以当前配置树为准整体写出）
    const fs::path tmp = target.string() + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) {
            fprintf(stderr, "[config] persist: 无法写 %s\n", tmp.c_str());
            return false;
        }
        f << settings_.dump() << "\n";
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

std::vector<std::string> Config::extension_dirs() const {
    std::vector<std::string> dirs;
    const std::string home = getenv_or("HOME", ".");
    dirs.push_back(fs::path(home) / ".realagent" / "extensions");
    const std::string root = find_project_root(fs::current_path().string());
    if (!root.empty()) dirs.push_back(fs::path(root) / ".realagent" / "extensions");
    return dirs;
}

std::string Config::session_dir() const { return std::string(kSessionDir); }

json Config::to_json() const {
    std::lock_guard<std::mutex> lk(*mutex_);
    return settings_;
}

} // namespace realagent
