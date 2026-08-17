/*
 * config.hpp — 配置（core 统一收集，注入给插件）
 *
 * 分层两级（ADR-0010）：代码里的默认树（config_defaults.hpp）打底，
 * ~/.realagent/settings.json 覆盖。不看 cwd，不看项目目录，没有 env 那一层。
 * 默认树是键清单的唯一来源——想知道能配什么、不配是什么值，看那一个函数。
 *
 * 没有"必需键"：配置缺失不是错误状态，只是取到默认值，load() 不校验缺了什么。
 * 端点与模型名的真实默认值属于 Provider 壳（壳以 empty() 判断"未配"再兜底），
 * core 侧一律空串。load() 只在一种情况下失败：settings.json 存在但不是合法 JSON。
 *
 * 模型档位（ModelTier）：主模型 model 干正事，小模型 small_model 干杂活。
 * 两档共用 base_url / api_key——档位只换模型名，不换端点凭证。
 * core 不做档位间回落：small_model 空就是空串，原样下传，填什么由壳决定。
 *
 * 会话目录不是配置项，是 core 自己的落盘路径：写死 .realagent/sessions。
 *
 * 插件初始化时经 ra_core_api.get_config 读取合并结果，插件不自行解析配置。
 * 线程安全：内部 mutex 保护配置树——agent 线程并发 get / 事件循环 persist 均安全。
 *
 * 写回只有 persist(key, value) 一个入口，而且是点对点的：读文件、只改那一个键、
 * 原子写回（tmp+rename）。不 dump 内存树，所以默认值永远渗不进用户的 settings.json——
 * 文件里只有用户自己配过的东西。
 *
 * 不做热重载（ADR-0010）：启动时读一次，之后 core 不再看这个文件。手改配置需重启。
 */
#pragma once

#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"

namespace realagent {

/* 模型档位：一次 LLM 调用用哪一档模型 */
enum class ModelTier {
    Main,  // 主模型（对话主链路）
    Small  // 小模型（标题/摘要一类杂活）
};

class Config {
public:
    // 加载：默认树打底 + ~/.realagent/settings.json 覆盖（对象递归合并）。
    // 只有一种失败：文件存在但不是合法 JSON——读不懂就别带着半份配置往下跑。
    // 配置"缺项"不是失败，缺的取默认值。
    static std::expected<Config, std::string> load();

    // 读取配置项（合并后的值；未知键返回空串）
    std::string get(std::string_view key) const;
    bool has(std::string_view key) const;

    // 按档位取模型名（键名只此一处知道）
    std::string model(ModelTier tier) const;

    // 点对点写回：改内存树 + 只改 settings.json 里的这一个键，文件其余部分原样不碰。
    // key 支持点分路径（"plugins.disabled"）：只落到那个叶子，兄弟键不受影响。
    // 先落盘成功才改内存——失败时内存与文件都没变，不会出现"切了档但没写进去"。
    // 文件不存在按空对象起头；文件是坏 JSON 则拒绝写入并返回 false：
    // 宁可这次改动不生效，也不能拿内存树盖掉读不懂的用户数据（里面有 api_key）。
    bool persist(std::string_view key, const json& v);

    // 插件发现目录（全局 ~/.realagent/extensions，唯一来源）
    std::vector<std::string> extension_dirs() const;

    // 会话存储目录（core 常量，不可配置）。相对 cwd —— 会话按项目分家，
    // 与插件目录（全局 ~/.realagent）不同层。
    // static：它不读任何配置，Session 的清单扫描是静态的也照样能问到它。
    static std::string session_dir();

    // 模型数据表的运行时落点（用户接管版）：~/.realagent/models/<插件名>.json。
    // 内容是插件的数据（含单价），core 只认路径不认内容（ADR-0009）
    std::string models_path(std::string_view plugin_name) const;

    // 合并后的完整配置（JSON，注入给插件 init 用）
    json to_json() const;

private:
    json settings_;  // 合并后的配置树（默认树 + settings.json）
    // mutex 不可拷贝/移动，用 shared_ptr 包装保持 Config 可拷贝（load() 按值返回）
    mutable std::shared_ptr<std::mutex> mutex_ = std::make_shared<std::mutex>();
};

// 环境变量读取（找不到返回 fallback）。配置不走 env，此处只服务 HOME 一类进程环境。
std::string getenv_or(std::string_view name, std::string_view fallback = "");

} // namespace realagent
