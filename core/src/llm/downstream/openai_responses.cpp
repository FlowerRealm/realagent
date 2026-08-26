/*
 * downstream/openai_responses.cpp — /responses 的 SSE 帧 → 事件
 *
 * 与另外两套的实质差异：
 *   1. 帧类型在 SSE 的 event: 行上，data 里是该类型自己的强类型载荷
 *      （另外两套都是 data 里带 type 字段）——这就是 split_sse_block 要把
 *      event: 行也留下来的原因
 *   2. 工具调用有明确的开始（response.output_item.added）与结束
 *      （response.output_item.done）帧，不必像 openai_chat 那样按 index 攒
 *   3. usage 在 response.completed 的 response.usage 里，字段是
 *      input_tokens / output_tokens，缓存命中在 input_tokens_details.cached_tokens
 *   4. 失败是一个显式的帧（response.failed / error），不是 HTTP 状态——
 *      它必须让本次调用失败，不能当成"没内容"
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

std::string str_of(const bj::object& o, const char* key) {
    if (!o.contains(key) || !o.at(key).is_string()) return {};
    return bj::value_to<std::string>(o.at(key));
}

void close_thinking(OpenAiResponsesState& st, const EventSink& sink) {
    if (!st.reasoning_open) return;
    st.reasoning_open = false;
    if (sink) sink("thinking_stop", json{});
}

} // namespace

bool feed_block(protocol::OpenAiResponses, OpenAiResponsesState& st, std::string_view block,
                const EventSink& sink) {
    const SseBlock sb = split_sse_block(block);
    if (sb.data.empty()) return true;
    try {
        boost::system::error_code ec;
        const bj::value v = bj::parse(sb.data, ec);
        if (ec) return true;
        const bj::object& o = v.as_object();
        // 类型优先取 event: 行；有些实现在 data 里也带一份 type，两处一致时取哪个都一样
        const std::string t = !sb.event.empty() ? sb.event : str_of(o, "type");

        if (t == "response.output_text.delta") {
            close_thinking(st, sink);
            if (sink) {
                json ev;
                ev["delta"] = str_of(o, "delta");
                sink("message_update", ev);
            }
        } else if (t == "response.reasoning_summary_text.delta" ||
                   t == "response.reasoning_text.delta") {
            if (sink) {
                if (!st.reasoning_open) {
                    st.reasoning_open = true;
                    json ev;
                    ev["signature"] = ""; // 本协议不给 signature，字段留着让上层一视同仁
                    sink("thinking_start", ev);
                }
                json ev;
                ev["delta"] = str_of(o, "delta");
                sink("thinking_update", ev);
            }
        } else if (t == "response.reasoning_summary_text.done" ||
                   t == "response.reasoning_text.done") {
            close_thinking(st, sink);
        } else if (t == "response.output_item.added") {
            if (o.contains("item") && o.at("item").is_object()) {
                const bj::object& item = o.at("item").as_object();
                if (str_of(item, "type") == "function_call") {
                    close_thinking(st, sink);
                    st.tool_id_ = str_of(item, "call_id");
                    st.tool_name_ = str_of(item, "name");
                    st.tool_input_.clear();
                }
            }
        } else if (t == "response.function_call_arguments.delta") {
            st.tool_input_ += str_of(o, "delta");
        } else if (t == "response.output_item.done") {
            if (!o.contains("item") || !o.at("item").is_object()) return true;
            const bj::object& item = o.at("item").as_object();
            if (str_of(item, "type") != "function_call") return true;
            // 结束帧带全量 arguments 就用它，只有增量就用攒的——两者应当一致
            const std::string args =
                !str_of(item, "arguments").empty() ? str_of(item, "arguments") : st.tool_input_;
            if (sink) {
                json ev;
                ev["id"] = st.tool_id_.empty() ? str_of(item, "call_id") : st.tool_id_;
                ev["name"] = st.tool_name_.empty() ? str_of(item, "name") : st.tool_name_;
                boost::system::error_code aec;
                bj::value in = bj::parse(args.empty() ? "{}" : args, aec);
                ev["input"] = aec ? bj::value(bj::object{}) : in;
                sink("tool_use", ev);
            }
            st.tool_id_.clear();
            st.tool_name_.clear();
            st.tool_input_.clear();
            st.finish_reason = "tool_use";
        } else if (t == "response.completed" || t == "response.incomplete") {
            close_thinking(st, sink);
            if (o.contains("response") && o.at("response").is_object()) {
                const bj::object& r = o.at("response").as_object();
                if (r.contains("usage") && r.at("usage").is_object()) {
                    const bj::object& u = r.at("usage").as_object();
                    if (const long long n = num_of(u, "input_tokens"); n > 0) st.usage.input = n;
                    if (const long long n = num_of(u, "output_tokens"); n > 0) st.usage.output = n;
                    if (u.contains("input_tokens_details") &&
                        u.at("input_tokens_details").is_object()) {
                        const bj::object& d = u.at("input_tokens_details").as_object();
                        if (const long long n = num_of(d, "cached_tokens"); n > 0)
                            st.usage.cache_read = n;
                    }
                    emit_usage(st.usage, sink);
                }
            }
            if (sink) {
                json ev;
                ev["reason"] = st.finish_reason.empty() ? "stop" : st.finish_reason;
                sink("stop", ev);
            }
            st.finish_reason.clear();
        } else if (t == "response.failed" || t == "error") {
            // 显式失败帧：HTTP 是 200，但这次调用没成。绝不能当成"没内容"
            fprintf(stderr, "[llm] openai-responses 失败帧: %.200s\n", sb.data.c_str());
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "[llm] openai-responses 帧不合规: %s | data=%.200s\n", e.what(),
                sb.data.c_str());
    } catch (...) {
        fprintf(stderr, "[llm] openai-responses 帧不合规（未知异常）| data=%.200s\n",
                sb.data.c_str());
    }
    return false;
}

} // namespace realagent
