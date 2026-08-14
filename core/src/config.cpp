#include "config.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "config_defaults.hpp"

namespace realagent {
namespace fs = std::filesystem;

namespace {

// 会话落盘路径：core 自己的实现细节，不是配置项
constexpr std::string_view kSessionDir = ".realagent/sessions";

fs::path settings_path(const fs::path& dir) { return dir / ".realagent" / "settings.json"; }

// 全局配置落点：~/.realagent/settings.json——唯一的覆盖来源，不看 cwd
fs::path global_dir() { return fs::path(getenv_or("HOME", ".")); }

// 读一份 settings.json。文件不存在 → nullopt（不是错误，用默认树就行）；
// 打不开 / 解析不了 → 错误（读不懂就别猜）
std::expected<std::optional<json>, std::string> read_settings(const fs::path& path) {
    if (!fs::exists(path)) return std::optional<json>{};
    std::ifstream f(path);
    if (!f) return std::unexpected(path.string() + " 打不开");
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto j = json::parse(text);
    if (!j) return std::unexpected(path.string() + " 不是合法 JSON");
    return std::optional<json>{std::move(*j)};
}

// 对象递归合并，数组与标量整个替换。
// 数组不合并是刻意的：disabled: ["x"] 的意思是"就禁这一个"，不是"在默认基础上再加一个"。
// 顶层整键替换会静默丢掉嵌套默认值（只写 plugins.disabled 就会带走 plugins 下的其他默认），
// 而且丢的方式很隐蔽——不报错，值变成类型默认值。
void merge_into(json& dst, const json& src) {
    for (const auto& k : src.keys()) {
        const json v = src[k];
        // 先 contains 再取 dst[k]：非 const operator[] 会给缺失键插 null，污染配置树
        if (v.is_object() && dst.contains(k) && dst[k].is_object()) merge_into(dst[k], v);
        else dst[k] = v;
    }
}

// tmp + rename 原子写。断电或进程被杀只会留下临时文件，不会留半截的 settings.json
bool write_atomic(const fs::path& target, const std::string& text) {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
        fprintf(stderr, "[config] persist: 创建目录失败 %s\n", ec.message().c_str());
        return false;
    }
    const fs::path tmp = target.string() + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) {
            fprintf(stderr, "[config] persist: 无法写 %s\n", tmp.c_str());
            return false;
        }
        f << text << "\n";
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        fprintf(stderr, "[config] persist: rename 失败 %s\n", ec.message().c_str());
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace

std::string getenv_or(std::string_view name, std::string_view fallback) {
    if (const char* v = std::getenv(std::string(name).c_str()); v != nullptr)
        return std::string(v);
    return std::string(fallback);
}

std::expected<Config, std::string> Config::load() {
    Config cfg;
    cfg.settings_ = config_defaults();

    // 默认树打底，settings.json 覆盖。缺键不是错，缺的就用默认值——不校验必需项。
    auto file = read_settings(settings_path(global_dir()));
    if (!file) return std::unexpected(file.error());
    if (*file) merge_into(cfg.settings_, **file);

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

// core 不做档位间回落：small_model 空就是空串，原样下传给 Provider 壳去兜底
std::string Config::model(ModelTier tier) const {
    return get(tier == ModelTier::Small ? "small_model" : "model");
}

bool Config::persist(std::string_view key, const json& v) {
    const fs::path target = settings_path(global_dir());

    // 点对点：读出文件原样，只改这一个键。不 dump 内存树——默认值不进用户的文件。
    auto file = read_settings(target);
    if (!file) {
        // 坏 JSON：拒绝写入。要写就只能整树覆盖，那会抹掉我们没读懂的用户数据（含 api_key）
        fprintf(stderr, "[config] persist 放弃：%s\n", file.error().c_str());
        return false;
    }
    json tree = file->value_or(json{}); // 文件不存在 → 空对象起头
    tree[key] = v;
    if (!write_atomic(target, tree.dump())) return false;

    // 落盘成功才改内存：失败时内存与文件都没变，不会出现"切了档但没写进去"
    std::lock_guard<std::mutex> lk(*mutex_);
    settings_[key] = v;
    return true;
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
