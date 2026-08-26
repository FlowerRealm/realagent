/*
 * upstream/openai_chat.cpp — 抽象对话 → /chat/completions 请求
 *
 * 与 anthropic_messages 的三处实质差异（不是风格差异，是协议差异）：
 *   1. system 是 messages 里的一条（role=system），不是顶层字段
 *   2. 一条 message 的 content 是字符串，不是块数组；工具调用另开 tool_calls 字段，
 *      工具结果另开一条 role=tool 的 message
 *   3. 工具参数是 JSON **字符串**，不是对象
 *
 * thinking 块不回传：本协议没有"把上一轮的思考原样交回去"这回事，
 * reasoning 内容是只读的产出。硬塞进 content 会被当成用户可见正文，那是另一种谎。
 */
#include <boost/json.hpp>

#include "llm/llm.hpp"

namespace realagent {
namespace bj = boost::json;

namespace {

/* 块数组 → 一段文本（本协议的 content 只有字符串这一种形状） */
std::string join_text(const bj::array& blocks) {
    std::string out;
    for (const auto& blk : blocks) {
        const bj::object& b = blk.as_object();
        if (bj::value_to<std::string>(b.at("type")) != "text") continue;
        out += bj::value_to<std::string>(b.at("text"));
    }
    return out;
}

} // namespace

HttpRequest build_request(protocol::OpenAiChat, const Config& cfg, const json& dialog) {
    const bj::object& d = dialog.as_object();

    bj::object body;
    body["model"] = d.contains("model") ? d.at("model") : bj::value("");
    body["max_tokens"] = 4096;
    body["stream"] = true;
    // 不问就不给：本协议默认的流不带 usage，计价拿不到数
    bj::object so;
    so["include_usage"] = true;
    body["stream_options"] = so;

    bj::array msgs;
    if (const std::string system =
            d.contains("system") ? bj::value_to<std::string>(d.at("system")) : std::string();
        !system.empty()) {
        bj::object sys;
        sys["role"] = "system";
        sys["content"] = system;
        msgs.push_back(sys);
    }

    if (d.contains("messages") && d.at("messages").is_array()) {
        for (const auto& m : d.at("messages").as_array()) {
            const bj::object& mo = m.as_object();
            const std::string role = bj::value_to<std::string>(mo.at("role"));
            const bj::array& blocks = mo.contains("content") && mo.at("content").is_array()
                                          ? mo.at("content").as_array()
                                          : bj::array{};

            // tool_result 各自单独成一条 role=tool 的 message，先把它们摘出去
            bj::array tool_msgs;
            bj::array tool_calls;
            for (const auto& blk : blocks) {
                const bj::object& b = blk.as_object();
                const std::string bt = bj::value_to<std::string>(b.at("type"));
                if (bt == "tool_result") {
                    bj::object tm;
                    tm["role"] = "tool";
                    tm["tool_call_id"] = b.at("tool_use_id");
                    tm["content"] = b.at("content");
                    tool_msgs.push_back(tm);
                } else if (bt == "tool_use") {
                    bj::object fn;
                    fn["name"] = b.at("name");
                    // 参数是 JSON 字符串，不是对象——本协议如此
                    fn["arguments"] =
                        bj::serialize(b.contains("input") ? b.at("input") : bj::value(bj::object{}));
                    bj::object tc;
                    tc["id"] = b.at("id");
                    tc["type"] = "function";
                    tc["function"] = fn;
                    tool_calls.push_back(tc);
                }
            }

            const std::string text = join_text(blocks);
            if (!text.empty() || !tool_calls.empty()) {
                bj::object mout;
                mout["role"] = role == "user" ? "user" : "assistant";
                mout["content"] = text;
                if (!tool_calls.empty()) mout["tool_calls"] = tool_calls;
                msgs.push_back(mout);
            }
            for (auto& tm : tool_msgs) msgs.push_back(tm);
        }
    }
    body["messages"] = msgs;

    if (d.contains("tools") && d.at("tools").is_array()) {
        bj::array tools;
        for (const auto& t : d.at("tools").as_array()) {
            const bj::object& to = t.as_object();
            bj::object fn;
            fn["name"] = to.at("name");
            if (to.contains("description")) fn["description"] = to.at("description");
            fn["parameters"] =
                to.contains("input_schema") ? to.at("input_schema") : bj::value(bj::object{});
            bj::object tool;
            tool["type"] = "function";
            tool["function"] = fn;
            tools.push_back(tool);
        }
        body["tools"] = tools;
        body["tool_choice"] = "auto";
    }

    HttpRequest req;
    req.url = cfg.get("base_url") + "/chat/completions";
    if (const std::string key = cfg.get("api_key"); !key.empty())
        req.headers.push_back("Authorization: Bearer " + key);
    req.headers.push_back("Content-Type: application/json");
    req.body = bj::serialize(body);
    return req;
}

} // namespace realagent
