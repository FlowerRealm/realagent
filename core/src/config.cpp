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

// 全局配置落点：~/.realagent/settings.json——唯一来源，不看 cwd
fs::path global_dir() { return fs::path(getenv_or("HOME", ".")); }

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

// settings.json 的 mtime（取不到 = 0，与"文件不存在"同值）
int64_t mtime_of(const fs::path& path) {
    std::error_code ec;
    const auto t = fs::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<int64_t>(t.time_since_epoch().count());
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

std::vector<std::string_view> Config::required_keys() {
    return {std::begin(kRequired), std::end(kRequired)};
}

std::expected<Config, std::string> Config::load() {
    Config cfg;
    cfg.settings_ = json{};

    // 唯一来源：全局 ~/.realagent/settings.json。不看 cwd，不看项目
    const fs::path global = settings_path(global_dir());
    if (auto r = merge_file(cfg.settings_, global); !r) return std::unexpected(r.error());

    // 必需键一次报全：别让用户补一个跑一次
    // （走 const 引用取值：非 const operator[] 会给缺失键插 null，污染配置树）
    const json& tree = cfg.settings_;
    std::vector<std::string_view> missing;
    for (const auto& k : kRequired) {
        if (tree[k].as_string().value_or("").empty()) missing.push_back(k);
    }
    if (!missing.empty()) {
        return std::unexpected("配置缺必需键 [" + join(missing, ", ") + "]，补进 " +
                               global.string() + "（必需：" +
                               join(required_keys(), " / ") + "）");
    }
    cfg.mtime_ = mtime_of(global);
    return cfg;
}

// 重载 = 再 load 一次，把配置树整个换掉。加载的本质就是复写，没有第二套读取/校验路径。
bool Config::reload_if_changed() {
    const int64_t mt = mtime_of(settings_path(global_dir()));

    std::lock_guard<std::mutex> lk(*mutex_);
    if (mt == 0 || mt == mtime_) return false; // 文件没了或没动过
    mtime_ = mt;                               // 坏文件也记：别每轮重读同一个坏文件

    // 坏 JSON / 缺必需键保留旧树：启动缺配置该退出，跑着的会话不该被一次手滑写崩
    auto fresh = Config::load();
    if (!fresh) {
        fprintf(stderr, "[config] reload 忽略：%s\n", fresh.error().c_str());
        return false;
    }
    settings_ = std::move(fresh->settings_);
    return true;
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
    const fs::path target = settings_path(global_dir());

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
    mtime_ = 0; // 写完就作废：自己写的和别人写的一样，下一轮照常重读
    return true;
}

bool Config::persist() {
    std::lock_guard<std::mutex> lk(*mutex_);
    return persist_locked();
}

std::vector<std::string> Config::extension_dirs() const {
    return {(global_dir() / ".realagent" / "extensions").string()};
}

std::string Config::session_dir() const { return std::string(kSessionDir); }

std::string Config::models_path(std::string_view plugin_name) const {
    return (global_dir() / ".realagent" / "models" / (std::string(plugin_name) + ".json")).string();
}

json Config::to_json() const {
    std::lock_guard<std::mutex> lk(*mutex_);
    return settings_;
}

} // namespace realagent
