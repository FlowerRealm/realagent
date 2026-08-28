/*
 * config.hpp — 配置
 *
 * 分层两级（ADR-0010）：代码里的默认打底（config.cpp 的 defaults()），
 * ~/.realagent/settings.json 覆盖。不看 cwd，不看项目目录，没有 env 那一层。
 *
 * 有默认值的键只有一个：permission（安全默认）。其余键缺了就是空串。
 * 端点那一束（protocol / base_url / model）坚持不给默认，理由见 llm.hpp。
 *
 * 没有"必需键"：配置缺失不是错误状态，只是取到默认值，load() 不校验缺了什么。
 * load() 只在一种情况下失败：settings.json 存在但不是合法 JSON。
 *
 * 模型档位（ModelTier）：主模型 model 干正事，小模型 small_model 干杂活。
 * 两档共用 base_url / api_key——档位只换模型名，不换端点凭证。
 * 不做档位间回落：small_model 空就是空串。
 *
 * 会话目录不是配置项，是 core 自己的落盘路径：写死 .realagent/sessions。
 *
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

#include "json.hpp"

namespace realagent {

/* 模型档位：一次 LLM 调用用哪一档模型 */
enum class ModelTier {
    Main, // 主模型（对话主链路）
    Small // 小模型（标题/摘要一类杂活）
};

class Config {
  public:
    // 加载：默认树打底 + ~/.realagent/settings.json 逐键覆盖（配置树是平的）。
    // 只有一种失败：文件存在但不是合法 JSON——读不懂就别带着半份配置往下跑。
    // 配置"缺项"不是失败，缺的取默认值。
    static std::expected<Config, std::string> load();

    // 读取配置项（合并后的值；未知键返回空串）
    std::string get(std::string_view key) const;
    bool has(std::string_view key) const;

    // 按档位取模型名（键名只此一处知道）
    std::string model(ModelTier tier) const;

    // 点对点写回：改内存树 + 只改 settings.json 里的这一个键，文件其余部分原样不碰。
    // 先落盘成功才改内存——失败时内存与文件都没变，不会出现"切了档但没写进去"。
    // 文件不存在按空对象起头；文件是坏 JSON 则拒绝写入并返回 false：
    // 宁可这次改动不生效，也不能拿内存树盖掉读不懂的用户数据（里面有 api_key）。
    bool persist(std::string_view key, const nlohmann::json &v);

    // 会话存储目录（core 常量，不可配置）。相对 cwd —— 会话按项目分家，
    // 与全局配置（~/.realagent）不同层。
    // static：它不读任何配置，Session 的清单扫描是静态的也照样能问到它。
    static std::string session_dir();

    // 模型数据表的用户接管版：~/.realagent/models.json。
    // 存在即整表替换出厂表（ADR-0009），不合并
    std::string models_path() const;

    // 合并后的完整配置树
    nlohmann::json to_json() const;

  private:
    nlohmann::json settings_; // 合并后的配置树（默认树 + settings.json）
    // mutex 不可拷贝/移动，用 shared_ptr 包装保持 Config 可拷贝（load() 按值返回）
    mutable std::shared_ptr<std::mutex> mutex_ = std::make_shared<std::mutex>();
};

// 环境变量读取（找不到返回 fallback）。配置不走 env，此处只服务 HOME 一类进程环境。
std::string getenv_or(std::string_view name, std::string_view fallback = "");

} // namespace realagent
