/*
 * upstream/openai_responses.cpp — 抽象对话 → /responses 请求
 *
 * 与 openai_chat 的差异（同一家公司的两套协议，不是同一套的两个版本）：
 *   1. 对话叫 input 不叫 messages，条目是强类型 item：message / function_call /
 *      function_call_output 三种，各有各的形状
 *   2. 文本块按方向分名：用户侧 input_text，助手侧 output_text
 *   3. 工具是平的（type/name/parameters 同层），不套一层 function
 *   4. system 走顶层 instructions
 *   5. 工具结果的关联键叫 call_id，不叫 tool_call_id
 */
#include <boost/json.hpp>

#include "llm/llm.hpp"

namespace realagent {
namespace bj = boost::json;

HttpRequest build_request(protocol::OpenAiResponses, const Config& cfg, const json& dialog) {
    const bj::object& d = dialog.as_object();

    bj::object body;
    body["model"] = d.contains("model") ? d.at("model") : bj::value("");
    body["stream"] = true;
    if (const std::string system =
            d.contains("system") ? bj::value_to<std::string>(d.at("system")) : std::string();
        !system.empty())
        body["instructions"] = system;

    bj::array input;
    if (d.contains("messages") && d.at("messages").is_array()) {
        for (const auto& m : d.at("messages").as_array()) {
            const bj::object& mo = m.as_object();
            const std::string role = bj::value_to<std::string>(mo.at("role"));
            const bool is_user = role == "user";
            const bj::array& blocks = mo.contains("content") && mo.at("content").is_array()
                                          ? mo.at("content").as_array()
                                          : bj::array{};

            bj::array text_parts;
            for (const auto& blk : blocks) {
                const bj::object& b = blk.as_object();
                const std::string bt = bj::value_to<std::string>(b.at("type"));
                if (bt == "text") {
                    bj::object part;
                    part["type"] = is_user ? "input_text" : "output_text";
                    part["text"] = b.at("text");
                    text_parts.push_back(part);
                } else if (bt == "tool_use") {
                    bj::object call;
                    call["type"] = "function_call";
                    call["call_id"] = b.at("id");
                    call["name"] = b.at("name");
                    call["arguments"] =
                        bj::serialize(b.contains("input") ? b.at("input") : bj::value(bj::object{}));
                    input.push_back(call);
                } else if (bt == "tool_result") {
                    bj::object out;
                    out["type"] = "function_call_output";
                    out["call_id"] = b.at("tool_use_id");
                    out["output"] = b.at("content");
                    input.push_back(out);
                }
            }
            if (!text_parts.empty()) {
                bj::object item;
                item["type"] = "message";
                item["role"] = role;
                item["content"] = text_parts;
                input.push_back(item);
            }
        }
    }
    body["input"] = input;

    if (d.contains("tools") && d.at("tools").is_array()) {
        bj::array tools;
        for (const auto& t : d.at("tools").as_array()) {
            const bj::object& to = t.as_object();
            bj::object tool;
            tool["type"] = "function";
            tool["name"] = to.at("name");
            if (to.contains("description")) tool["description"] = to.at("description");
            tool["parameters"] =
                to.contains("input_schema") ? to.at("input_schema") : bj::value(bj::object{});
            tools.push_back(tool);
        }
        body["tools"] = tools;
        body["tool_choice"] = "auto";
    }

    HttpRequest req;
    req.url = cfg.get("base_url") + "/responses";
    if (const std::string key = cfg.get("api_key"); !key.empty())
        req.headers.push_back("Authorization: Bearer " + key);
    req.headers.push_back("Content-Type: application/json");
    req.body = bj::serialize(body);
    return req;
}

} // namespace realagent
