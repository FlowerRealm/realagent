/*
 * config_defaults.hpp — 默认配置树（ADR-0010）
 *
 * 这里是配置键清单的唯一来源。想知道"一共能配什么、不配是什么值、各自干嘛用"，
 * 看这一个函数就够——不必翻 core、翻插件、翻文档。加新配置项就往这棵树里加一行。
 *
 * 分层两级：这棵树打底，~/.realagent/settings.json 覆盖。没有 env 那一层。
 * 没有"必需键"这回事：配置缺失不是错误状态，只是取到默认值，core 不校验缺了什么。
 *
 * 值一律是空串，且 core 的默认值不带任何供应商身份（ADR-0004）：
 * 真实的端点与模型名默认值属于 Provider 壳，不属于 core。
 *
 * 【别拿假 URL 之类的非空值占位】整条兜底链路——壳的 init、refine_body、refine_headers——
 * 都以 empty() 判断"用户没配"。填一个 https://example.invalid 进来，壳会认为用户配过了，
 * 整套兜底当场失效，用户拿到的是一个连不上的假端点。空串不是占位符，它是这条链路的协议。
 * 取值形状要说明就写进注释，不要写进值里。
 */
#pragma once

#include "json.hpp"

namespace realagent {

inline json config_defaults() {
    json d;

    // —— 协议链配置（core 统一注入给插件，插件不自行解析）——
    // 凭证。形如 sk-xxx。空 = 不发 Authorization 头，服务端通常回 401
    d["api_key"] = "";
    // 端点根，形如 https://api.example.com/anthropic（不含 /v1/messages 这一段）
    // 空 = 由 Provider 壳填它自己的默认端点；裸协议层无壳时拼出相对 URL，libcurl 会报错
    d["base_url"] = "";
    // 主模型名，形如 deepseek-v4-flash。空 = 由 Provider 壳填它的默认模型
    d["model"] = "";
    // 小模型名（标题/摘要一类杂活）。空 = 空模型名原样下传，同样由壳兜底。
    // core 不做档位间回落：壳只看见一个空模型名，分辨不出是哪一档——档位知识到 core 为止
    d["small_model"] = "";

    // —— 插件 ——
    // 禁用清单（插件名数组）。默认空数组 = 扫到的插件全部启用。
    // 合并时数组整个替换而非追加：["x"] 的意思是"就禁这一个"
    d["plugins"]["disabled"] = json::array();

    return d;
}

} // namespace realagent
