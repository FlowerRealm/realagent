#include "config.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace realagent {
namespace fs = std::filesystem;

namespace {

// 默认配置树（ADR-0010）：load() 用它打底，settings.json 再逐键覆盖。
// 只有一个键：permission。这是安全默认，缺了不该放行——它也是唯一一个不属于
// 那一束端点配置、因而留在顶层的键（ADR-0023）。
// provider 没有默认：没配时 get() 取那五个键本就返回空串，
// 再写一行 d["provider"] = {} 是把编译器免费做的事运行期重做一遍；
// provider 里那几个键为什么坚持不给默认（也不校验），见 CONTEXT.md 的[[端点束]]。
nlohmann::json defaults()
{
    // dangerous 工具执行前怎么裁决：ask 问用户（默认）/ allow-all 一律放行 / deny 一律拒绝
    return {{"permission", "ask"}};
}

fs::path settings_path(const fs::path &dir) { return dir / ".realagent" / "settings.json"; }

// 全局配置落点：~/.realagent/settings.json——唯一的覆盖来源，不看 cwd
fs::path global_dir() { return fs::path(getenv_or("HOME", ".")); }

// 读一份 settings.json。文件不存在 → nullopt（不是错误，用默认树就行）；
// 打不开 / 解析不了 → 错误（读不懂就别猜）
std::expected<std::optional<nlohmann::json>, std::string> read_settings(const fs::path &path)
{
    if (!fs::exists(path)) return std::optional<nlohmann::json>{};
    std::ifstream f(path);
    if (!f) return std::unexpected(path.string() + " 打不开");
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    // 第三个参数 false = 解析失败不抛，返回一个 discarded 值
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded()) return std::unexpected(path.string() + " 不是合法 JSON");
    return std::optional<nlohmann::json>{std::move(j)};
}

// tmp + rename 原子写。断电或进程被杀只会留下临时文件，不会留半截的 settings.json
bool write_atomic(const fs::path &target, const std::string &text)
{
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec)
    {
        fprintf(stderr, "[config] persist: 创建目录失败 %s\n", ec.message().c_str());
        return false;
    }
    const fs::path tmp = target.string() + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f)
        {
            fprintf(stderr, "[config] persist: 无法写 %s\n", tmp.c_str());
            return false;
        }
        f << text << "\n";
    }
    fs::rename(tmp, target, ec);
    if (ec)
    {
        fprintf(stderr, "[config] persist: rename 失败 %s\n", ec.message().c_str());
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace

std::string getenv_or(std::string_view name, std::string_view fallback)
{
    if (const char *v = std::getenv(std::string(name).c_str()); v != nullptr)
        return std::string(v);
    return std::string(fallback);
}

std::expected<Config, std::string> Config::load()
{
    Config cfg;
    cfg.settings_ = defaults();

    // 默认树打底，settings.json 逐个顶层键覆盖（update 默认不递归，正是要的：
    // 唯一的嵌套是 provider 那个对象，它的语义就是"这一束一起换"）。
    // 缺键不是错，缺的就用默认值——不校验必需项。
    // 合法 JSON 但不是对象（比如一个数组）就当没配：update 对非对象会抛。
    auto file = read_settings(settings_path(global_dir()));
    if (!file) return std::unexpected(file.error());
    if (*file && (*file)->is_object()) cfg.settings_.update(**file);

    return cfg;
}

std::string Config::get(std::string_view path) const
{
    std::lock_guard<std::mutex> lk(*mutex_);
    return settings_.value(nlohmann::json::json_pointer(std::string(path)), std::string());
}

nlohmann::json Config::provider() const
{
    std::lock_guard<std::mutex> lk(*mutex_);
    const nlohmann::json p = settings_.value("/provider"_json_pointer, nlohmann::json::object());
    return p.is_object() ? p : nlohmann::json::object(); // 配成别的类型 = 当没配
}

// 不做档位间回落：small_model 空就是空串。回落会让"我明明配了小模型"与
// "我没配所以用了主模型"长得一模一样，出账单时才发现区别
std::string Config::model(ModelTier tier) const
{
    return get(tier == ModelTier::Small ? "/provider/small_model" : "/provider/model");
}

bool Config::persist(std::string_view key, const nlohmann::json &v)
{
    const fs::path target = settings_path(global_dir());

    // 点对点：读出文件原样，只改这一个键。不 dump 内存树——
    // 默认值不进用户的文件，core 还不认识的键也原样留着。
    auto file = read_settings(target);
    if (!file)
    {
        // 坏 JSON：拒绝写入。要写就只能整树覆盖，那会抹掉我们没读懂的用户数据（含 api_key）
        fprintf(stderr, "[config] persist 放弃：%s\n", file.error().c_str());
        return false;
    }
    nlohmann::json tree = file->value_or(nlohmann::json::object()); // 文件不存在 → 空对象起头
    tree[std::string(key)] = v;                                     // 只动这一个键，用户配的其余键原样留在文件里
    if (!write_atomic(target, tree.dump())) return false;

    // 落盘成功才改内存：失败时内存与文件都没变，不会出现"切了档但没写进去"
    std::lock_guard<std::mutex> lk(*mutex_);
    settings_[std::string(key)] = v;
    return true;
}

std::string Config::models_path() const
{
    return (global_dir() / ".realagent" / "models.json").string();
}

} // namespace realagent
