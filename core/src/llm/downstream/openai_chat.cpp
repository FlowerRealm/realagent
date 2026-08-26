/*
 * downstream/openai_chat.cpp — /chat/completions 的 SSE 帧 → 事件
 *
 * 与 anthropic_messages 的实质差异：
 *   1. 流的终点是一行字面量 `data: [DONE]`，不是一个带类型的帧
 *   2. 工具调用按 index 增量到达（id 与 name 只在首帧给，arguments 分片累积），
 *      所以要按 index 攒；攒好的在收工那一帧一次性发出，顺序按首次出现
 *   3. usage 只在最后一个 choices 为空的帧里给（还得请求时开 stream_options）
 *   4. token 字段叫 prompt_tokens / completion_tokens，缓存命中藏在
 *      prompt_tokens_details.cached_tokens 里——换算在这儿做完，出门一律叫
 *      input / output / cache_read
 *   5. 思考内容叫 reasoning_content（DeepSeek）或 reasoning，没有 signature，
 *      也没有"块结束"帧——thinking_stop 由本文件在正文开始或收工时补
 */
#include <boost/json.hpp>

#include <cstdio>
#include <exception>

#include "llm/llm.hpp"

namespace realagent {
namespace bj = boost::json;

namespace {

long long num_of(const bj::object& o, const char* key) {
    if (!o.contains(key)) return 0;
    const bj::value& v = o.at(key);
    return v.is_int64() ? v.as_int64() : (v.is_uint64() ? (long long)v.as_uint64() : 0);
}

/* 取字符串字段；缺失或不是字符串返回空串（各家给的 delta 字段并不齐全） */
std::string str_of(const bj::object& o, const char* key) {
    if (!o.contains(key) || !o.at(key).is_string()) return {};
    return bj::value_to<std::string>(o.at(key));
}

void close_thinking(OpenAiChatState& st, const EventSink& sink) {
    if (!st.reasoning_open) return;
    st.reasoning_open = false;
    if (sink) sink("thinking_stop", json{});
}

/* 收工：先把攒好的工具调用发出去，再发 stop。顺序不能反——
 * 上层收到 stop 就当本轮结束了，之后来的 tool_use 没人接。 */
void finish(OpenAiChatState& st, const EventSink& sink) {
    close_thinking(st, sink);
    if (sink) {
        for (const long long idx : st.tool_order) {
            const auto it = st.tools.find(idx);
            if (it == st.tools.end()) continue;
            json ev;
            ev["id"] = it->second.id;
            ev["name"] = it->second.name;
            boost::system::error_code ec;
            bj::value in = bj::parse(it->second.args.empty() ? "{}" : it->second.args, ec);
            ev["input"] = ec ? bj::value(bj::object{}) : in;
            sink("tool_use", ev);
        }
        json ev;
        // 本协议的 tool_calls 与 anthropic 的 tool_use 是同一件事，收工理由也归一
        ev["reason"] = st.finish_reason == "tool_calls" ? "tool_use"
                       : st.finish_reason.empty()       ? "stop"
                                                        : st.finish_reason;
        sink("stop", ev);
    }
    st.tools.clear();
    st.tool_order.clear();
    st.finish_reason.clear();
}

} // namespace

bool feed_block(protocol::OpenAiChat, OpenAiChatState& st, std::string_view block,
                const EventSink& sink) {
    const SseBlock sb = split_sse_block(block);
    if (sb.data.empty()) return true;
    if (sb.data == "[DONE]") {
        // 有些端点只给 [DONE] 不给 finish_reason：收工帧照发，不让上层空等
        if (!st.tool_order.empty() || !st.finish_reason.empty() || st.reasoning_open)
            finish(st, sink);
        return true;
    }
    try {
        boost::system::error_code ec;
        const bj::value v = bj::parse(sb.data, ec);
        if (ec) return true; // 不是 JSON 的 data 行：忽略，不是错
        const bj::object& o = v.as_object();

        // 端点把错误塞进流里（HTTP 200 + 一帧 error）：这不是内容，是失败
        if (o.contains("error")) {
            fprintf(stderr, "[llm] openai-chat 流内错误: %.200s\n", sb.data.c_str());
            return false;
        }

        if (o.contains("usage") && o.at("usage").is_object()) {
            const auto& u = o.at("usage").as_object();
            if (const long long n = num_of(u, "prompt_tokens"); n > 0) st.usage.input = n;
            if (const long long n = num_of(u, "completion_tokens"); n > 0) st.usage.output = n;
            if (u.contains("prompt_tokens_details") && u.at("prompt_tokens_details").is_object()) {
                const auto& d = u.at("prompt_tokens_details").as_object();
                if (const long long n = num_of(d, "cached_tokens"); n > 0) st.usage.cache_read = n;
            }
            emit_usage(st.usage, sink);
        }

        if (!o.contains("choices") || !o.at("choices").is_array()) return true;
        const bj::array& choices = o.at("choices").as_array();
        if (choices.empty()) return true; // usage-only 帧，上面已处理
        const bj::object& c0 = choices.at(0).as_object();

        if (c0.contains("delta") && c0.at("delta").is_object()) {
            const bj::object& d = c0.at("delta").as_object();

            // 思考内容：两个字段名都认，同一件事
            std::string reasoning = str_of(d, "reasoning_content");
            if (reasoning.empty()) reasoning = str_of(d, "reasoning");
            if (!reasoning.empty() && sink) {
                if (!st.reasoning_open) {
                    st.reasoning_open = true;
                    json ev;
                    ev["signature"] = ""; // 本协议没有 signature，字段留着让上层一视同仁
                    sink("thinking_start", ev);
                }
                json ev;
                ev["delta"] = reasoning;
                sink("thinking_update", ev);
            }

            if (const std::string text = str_of(d, "content"); !text.empty()) {
                close_thinking(st, sink); // 正文开始 = 思考结束，本协议不另发结束帧
                if (sink) {
                    json ev;
                    ev["delta"] = text;
                    sink("message_update", ev);
                }
            }

            if (d.contains("tool_calls") && d.at("tool_calls").is_array()) {
                close_thinking(st, sink);
                for (const auto& tcv : d.at("tool_calls").as_array()) {
                    const bj::object& tc = tcv.as_object();
                    const long long idx = num_of(tc, "index");
                    auto [it, fresh] = st.tools.try_emplace(idx);
                    if (fresh) st.tool_order.push_back(idx);
                    if (const std::string id = str_of(tc, "id"); !id.empty()) it->second.id = id;
                    if (tc.contains("function") && tc.at("function").is_object()) {
                        const bj::object& fn = tc.at("function").as_object();
                        if (const std::string n = str_of(fn, "name"); !n.empty())
                            it->second.name = n;
                        it->second.args += str_of(fn, "arguments");
                    }
                }
            }
        }

        if (const std::string fr = str_of(c0, "finish_reason"); !fr.empty()) {
            st.finish_reason = fr;
            finish(st, sink);
        }
        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "[llm] openai-chat 帧不合规: %s | data=%.200s\n", e.what(),
                sb.data.c_str());
    } catch (...) {
        fprintf(stderr, "[llm] openai-chat 帧不合规（未知异常）| data=%.200s\n", sb.data.c_str());
    }
    return false;
}

} // namespace realagent
