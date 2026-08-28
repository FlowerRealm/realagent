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
#include "llm/llm.hpp"

namespace realagent {

HttpRequest build_request(protocol::AnthropicMessages, const Config& cfg, const nlohmann::json& dialog) {
    const nlohmann::json& d = dialog;

    nlohmann::json body;
    body["model"] = d.value("model", std::string());
    body["max_tokens"] = 4096;
    body["stream"] = true;
    if (const std::string system = d.value("system", std::string()); !system.empty())
        body["system"] = system;

    // messages：抽象对话 → /v1/messages 格式（合并相邻同 role）
    nlohmann::json msgs = nlohmann::json::array();
    if (d.contains("messages") && d["messages"].is_array()) {
        for (const nlohmann::json& m : d["messages"]) {
            const std::string role = m.at("role");
            nlohmann::json blocks = nlohmann::json::array();
            if (m.contains("content") && m["content"].is_array()) {
                for (const nlohmann::json& b : m["content"]) {
                    const std::string bt = b.at("type");
                    nlohmann::json out_block = nlohmann::json::object();
                    if (bt == "text") {
                        out_block["type"] = "text";
                        out_block["text"] = b.at("text");
                    } else if (bt == "tool_use") {
                        out_block["type"] = "tool_use";
                        out_block["id"] = b.at("id");
                        out_block["name"] = b.at("name");
                        out_block["input"] = b.value("input", nlohmann::json::object());
                    } else if (bt == "tool_result") {
                        out_block["type"] = "tool_result";
                        out_block["tool_use_id"] = b.at("tool_use_id");
                        out_block["content"] = b.at("content");
                        if (b.value("is_error", false)) out_block["is_error"] = true;
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
            if (!msgs.empty() && msgs.back()["role"] == role) {
                nlohmann::json& last_blocks = msgs.back()["content"];
                for (nlohmann::json& blk : blocks) last_blocks.push_back(blk);
            } else {
                nlohmann::json mout;
                mout["role"] = role;
                mout["content"] = blocks;
                msgs.push_back(mout);
            }
        }
    }
    body["messages"] = msgs;

    if (d.contains("tools") && d["tools"].is_array()) {
        nlohmann::json tools = nlohmann::json::array();
        for (const nlohmann::json& t : d["tools"]) {
            nlohmann::json tool;
            tool["name"] = t.at("name");
            if (t.contains("description")) tool["description"] = t.at("description");
            tool["input_schema"] = t.value("input_schema", nlohmann::json::object());
            tools.push_back(tool);
        }
        body["tools"] = tools;
        body["tool_choice"] = nlohmann::json{{"type", "auto"}};
    }

    HttpRequest req;
    req.url = cfg.get("base_url") + "/v1/messages";
    if (const std::string key = cfg.get("api_key"); !key.empty()) {
        req.headers.push_back("x-api-key: " + key);
        req.headers.push_back("Authorization: Bearer " + key);
    }
    req.headers.push_back("Content-Type: application/json");
    req.headers.push_back("anthropic-version: 2023-06-01");
    req.body = body.dump();
    return req;
}

} // namespace realagent
