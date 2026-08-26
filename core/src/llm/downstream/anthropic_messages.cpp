/*
 * downstream/anthropic_messages.cpp — /v1/messages 的 SSE 帧 → 事件
 *
 * 帧类型在 data 的 type 字段里（event: 行与它永远一致，取两遍就得处理它们不一致时
 * 听谁的，那是凭空造出来的问题）。
 *
 * usage：各家在不同帧里给不同字段（input 在 message_start，output 在 message_delta），
 * 因此计数先落在状态上、合并后整体发出——下游永远收到完整一组，
 * 不必猜"0 是真的 0 还是这帧没给"。
 *
 * 按帧结构直取字段（.at / as_object / value_to 均会抛），帧一旦不合规就抛异常——
 * 就地兜住转成 false。异常绝不能穿出去：本函数由 libcurl 的写回调调进来，
 * 中间隔着 C 栈帧，异常穿过去是未定义行为。
 */
#include <boost/json.hpp>

#include <cstdio>
#include <exception>

#include "llm/llm.hpp"

namespace realagent {
namespace bj = boost::json;

namespace {

/* usage 对象里的整数字段，缺失/类型不符按 0——各家实现给的字段并不齐全 */
long long usage_num(const bj::object& u, const char* key) {
    if (!u.contains(key)) return 0;
    const bj::value& v = u.at(key);
    return v.is_int64() ? v.as_int64() : (v.is_uint64() ? (long long)v.as_uint64() : 0);
}

/* 合并一帧 usage 并发出。绝对值：后到覆盖先到，丢帧不造成永久偏差 */
void merge_usage(const bj::object& u, UsageCounts& c, const EventSink& sink) {
    if (usage_num(u, "input_tokens") > 0) c.input = usage_num(u, "input_tokens");
    if (usage_num(u, "output_tokens") > 0) c.output = usage_num(u, "output_tokens");
    if (usage_num(u, "cache_read_input_tokens") > 0)
        c.cache_read = usage_num(u, "cache_read_input_tokens");
    if (usage_num(u, "cache_creation_input_tokens") > 0)
        c.cache_write = usage_num(u, "cache_creation_input_tokens");
    emit_usage(c, sink);
}

} // namespace

bool feed_block(protocol::AnthropicMessages, AnthropicState& st, std::string_view block,
                const EventSink& sink) {
    const SseBlock sb = split_sse_block(block);
    if (sb.data.empty()) return true;
    try {
        boost::system::error_code ec;
        const bj::value v = bj::parse(sb.data, ec);
        if (ec) return true; // 不是 JSON 的 data 行（如 [DONE]）：忽略，不是错
        const bj::object& o = v.as_object();
        const std::string t = bj::value_to<std::string>(o.at("type"));

        if (t == "message_start") {
            // 新 message：计数清零，再合并首帧 usage（input_tokens 在这里给）
            st.usage = UsageCounts{};
            if (o.contains("message") && o.at("message").is_object()) {
                const auto& msg = o.at("message").as_object();
                if (msg.contains("usage") && msg.at("usage").is_object())
                    merge_usage(msg.at("usage").as_object(), st.usage, sink);
            }
        } else if (t == "content_block_start") {
            const auto& cb = o.at("content_block").as_object();
            const std::string cbt = bj::value_to<std::string>(cb.at("type"));
            st.block_type_ = cbt;
            if (cbt == "tool_use") {
                st.tool_id_ = bj::value_to<std::string>(cb.at("id"));
                st.tool_name_ = bj::value_to<std::string>(cb.at("name"));
                st.tool_input_.clear();
            } else if (cbt == "thinking") {
                // 思考块开始：先发 signature，再发起始文本（部分端点起始块自带文本）
                if (cb.contains("signature"))
                    st.thinking_sig_ = bj::value_to<std::string>(cb.at("signature"));
                if (sink) {
                    json ev;
                    ev["signature"] = st.thinking_sig_;
                    sink("thinking_start", ev);
                }
                if (cb.contains("thinking")) {
                    const std::string init = bj::value_to<std::string>(cb.at("thinking"));
                    if (!init.empty() && sink) {
                        json ev;
                        ev["delta"] = init;
                        sink("thinking_update", ev);
                    }
                }
            }
        } else if (t == "content_block_delta") {
            const auto& delta = o.at("delta").as_object();
            const std::string dt = bj::value_to<std::string>(delta.at("type"));
            if (dt == "text_delta" && sink) {
                json ev;
                ev["delta"] = bj::value_to<std::string>(delta.at("text"));
                sink("message_update", ev);
            } else if (dt == "thinking_delta" && st.block_type_ == "thinking" && sink) {
                json ev;
                ev["delta"] = bj::value_to<std::string>(delta.at("thinking"));
                sink("thinking_update", ev);
            } else if (dt == "input_json_delta" && st.block_type_ == "tool_use") {
                st.tool_input_ += bj::value_to<std::string>(delta.at("partial_json"));
            }
        } else if (t == "content_block_stop") {
            if (st.block_type_ == "tool_use" && sink) {
                json ev;
                ev["id"] = st.tool_id_;
                ev["name"] = st.tool_name_;
                boost::system::error_code iec;
                bj::value in = bj::parse(st.tool_input_, iec);
                ev["input"] = iec ? bj::value(bj::object{}) : in;
                sink("tool_use", ev);
                st.block_type_.clear();
            } else if (st.block_type_ == "thinking") {
                if (sink) sink("thinking_stop", json{});
                st.block_type_.clear();
                st.thinking_sig_.clear();
            }
        } else if (t == "message_delta") {
            // output_tokens 在这里给终值：先发 usage 再发 stop，保证下游收工时数字已定
            if (o.contains("usage") && o.at("usage").is_object())
                merge_usage(o.at("usage").as_object(), st.usage, sink);
            if (sink) {
                // delta 缺失/非对象不是丢帧的理由：stop 照发，reason 退回默认值
                json ev;
                ev["reason"] = "stop";
                if (o.contains("delta") && o.at("delta").is_object()) {
                    const auto& dd = o.at("delta").as_object();
                    if (dd.contains("stop_reason")) ev["reason"] = dd.at("stop_reason");
                }
                sink("stop", ev);
            }
        }
        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "[llm] anthropic-messages 帧不合规: %s | data=%.200s\n", e.what(),
                sb.data.c_str());
    } catch (...) {
        fprintf(stderr, "[llm] anthropic-messages 帧不合规（未知异常）| data=%.200s\n",
                sb.data.c_str());
    }
    return false;
}

} // namespace realagent
