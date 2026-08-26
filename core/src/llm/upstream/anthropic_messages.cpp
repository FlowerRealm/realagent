/*
 * upstream/anthropic_messages.cpp — 抽象对话 → /v1/messages 请求
 *
 * 协议归协议：请求结构、system / messages / tools / tool_choice / stream、
 * thinking 块原样回传带 signature。端点与凭证从配置读，本文件认不出对面是谁。
 *
 * 认证头两个一起发（ADR-0017）：Anthropic 原厂认 x-api-key，
 * DeepSeek 一类兼容端点认 Authorization: Bearer（它们的文档让你设 ANTHROPIC_AUTH_TOKEN）。
 * 同一个凭证的两个名字，实测原厂对多出来的那个头视而不见——
 * 于是不必为"对面是哪一家"开一个配置项，也不必在代码里认 URL。
 */
#include <boost/json.hpp>

#include "llm/llm.hpp"

namespace realagent {
namespace bj = boost::json;

HttpRequest build_request(protocol::AnthropicMessages, const Config& cfg, const json& dialog) {
    const bj::object& d = dialog.as_object();
    const std::string model =
        d.contains("model") ? bj::value_to<std::string>(d.at("model")) : std::string();
    const std::string system =
        d.contains("system") ? bj::value_to<std::string>(d.at("system")) : std::string();

    bj::object body;
    body["model"] = model;
    body["max_tokens"] = 4096;
    body["stream"] = true;
    if (!system.empty()) body["system"] = system;

    // messages：抽象对话 → /v1/messages 格式（合并相邻同 role）
    bj::array msgs;
    if (d.contains("messages") && d.at("messages").is_array()) {
        for (const auto& m : d.at("messages").as_array()) {
            const bj::object& mo = m.as_object();
            const std::string role = bj::value_to<std::string>(mo.at("role"));
            bj::array blocks;
            if (mo.contains("content") && mo.at("content").is_array()) {
                for (const auto& blk : mo.at("content").as_array()) {
                    const bj::object& b = blk.as_object();
                    const std::string bt = bj::value_to<std::string>(b.at("type"));
                    bj::object out_block;
                    if (bt == "text") {
                        out_block["type"] = "text";
                        out_block["text"] = b.at("text");
                    } else if (bt == "tool_use") {
                        out_block["type"] = "tool_use";
                        out_block["id"] = b.at("id");
                        out_block["name"] = b.at("name");
                        out_block["input"] = b.contains("input") ? b.at("input") : bj::object{};
                    } else if (bt == "tool_result") {
                        out_block["type"] = "tool_result";
                        out_block["tool_use_id"] = b.at("tool_use_id");
                        out_block["content"] = b.at("content");
                        if (b.contains("is_error") && b.at("is_error").as_bool())
                            out_block["is_error"] = true;
                    } else if (bt == "thinking") {
                        // thinking 块（协议固有内容）原样回传，带 signature（缺失时省略）
                        out_block["type"] = "thinking";
                        out_block["thinking"] = b.at("thinking");
                        if (b.contains("signature")) out_block["signature"] = b.at("signature");
                    }
                    blocks.push_back(out_block);
                }
            }
            // 合并相邻同 role：若上一条 message 同 role，并入其 content
            if (!msgs.empty() && msgs.back().as_object().at("role").as_string() == role) {
                auto& last_blocks = msgs.back().as_object().at("content").as_array();
                for (auto& blk : blocks) last_blocks.push_back(blk);
            } else {
                bj::object mout;
                mout["role"] = role;
                mout["content"] = blocks;
                msgs.push_back(mout);
            }
        }
    }
    body["messages"] = msgs;

    if (d.contains("tools") && d.at("tools").is_array()) {
        bj::array tools;
        for (const auto& t : d.at("tools").as_array()) {
            const bj::object& to = t.as_object();
            bj::object tool;
            tool["name"] = to.at("name");
            if (to.contains("description")) tool["description"] = to.at("description");
            tool["input_schema"] =
                to.contains("input_schema") ? to.at("input_schema") : bj::object{};
            tools.push_back(tool);
        }
        body["tools"] = tools;
        bj::object tc;
        tc["type"] = "auto";
        body["tool_choice"] = tc;
    }

    HttpRequest req;
    req.url = cfg.get("base_url") + "/v1/messages";
    if (const std::string key = cfg.get("api_key"); !key.empty()) {
        req.headers.push_back("x-api-key: " + key);
        req.headers.push_back("Authorization: Bearer " + key);
    }
    req.headers.push_back("Content-Type: application/json");
    req.headers.push_back("anthropic-version: 2023-06-01");
    req.body = bj::serialize(body);
    return req;
}

} // namespace realagent
