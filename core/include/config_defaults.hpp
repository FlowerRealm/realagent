/*
 * config_defaults.hpp — 默认配置树（ADR-0010 / ADR-0017）
 *
 * 这里是配置键清单的唯一来源。想知道"一共能配什么、不配是什么值、各自干嘛用"，
 * 看这一个函数加下面这段注释就够。加新配置项就往这棵树里加一行。
 *
 * 分层两级：这棵树打底，~/.realagent/settings.json 覆盖。没有 env 那一层。
 *
 * —— 有默认值的键在树里；没有默认值的键不在树里 ——
 *
 * 端点那一束三个键 **没有默认值，必须用户填**（ADR-0017）：
 *
 *   protocol    对面说哪套协议：anthropic-messages / openai-chat / openai-responses
 *   base_url    端点根
 *   model       主模型名
 *
 * 不给默认不是懒，是这三个键**不填就完全用不了**，而填错产生的报错
 * （端点 404、请求体形状不认、流解析出空）恰是最难自己诊断的一类。
 * 给个默认值等于替用户猜，猜错了他还以为是自己配的——报错了却啥都不知道，
 * 那比启动时被拦下来难受得多。缺哪个报哪个、一次报全、附样例，见
 * `endpoint_config_error()`（llm.hpp）。
 *
 * 剩下这些留默认，因为它们不满足"不填就用不了"：
 *   api_key      本地端点（llama.cpp 一类）不填就是能用；填错了服务端回 401，是句清楚的话
 *   small_model  缺了只是杂活不跑，主链路照常
 *   permission   这是**安全**默认，缺了不该放行——与"填了才能用"是两码事
 *
 * 不做热重载（ADR-0010）：启动时读一次，之后 core 不再看这个文件。
 */
#pragma once

#include "json.hpp"

namespace realagent {

inline nlohmann::json config_defaults()
{
    nlohmann::json d;

    // —— 凭证 ——
    // 形如 sk-xxx。空 = 不发认证头，需要鉴权的端点会回 401
    d["api_key"] = "";

    // —— 模型 ——
    // 小模型名（标题/摘要一类杂活）。不做档位间回落：空就是空串
    d["small_model"] = "";

    // —— 权限（ADR-0005 / ADR-0016）——
    // dangerous 工具执行前怎么裁决：
    //   ask       危险工具一律问用户（默认）
    //   allow-all 一律放行——打通链路用，不是安全策略
    //   deny      一律拒绝
    d["permission"] = "ask";

    return d;
}

} // namespace realagent
