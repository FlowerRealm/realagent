#include "agent/agent.hpp"

#include <curl/curl.h>

#include <cstdio>

namespace realagent {

Agent::Agent(CoreContext& ctx, Executor& exe) : ctx_(ctx), exe_(exe) {
    messages_ = json::array();
}

void Agent::reset() {
    messages_ = json::array();
}

void Agent::broadcast(const std::string& type, const json& payload) {
    if (ctx_.emit_fn) ctx_.emit_fn(type, payload.dump());
}

/* 找到协议插件（首版 deepseek-messages） */
static Plugin* find_protocol_plugin(CoreContext& ctx) {
    for (auto* p : ctx.all_plugins)
        if (p && p->api && p->api->type == PLUGIN_TYPE_PROTOCOL && p->api->build_request &&
            p->api->parse_feed)
            return p;
    return nullptr;
}

/* parse_feed 事件接收器（协议插件 → agent）——前向声明 */
static void feed_sink(void* sink_ctx, const char* type, const char* payload);

/* —— curl 写回调：把响应体 chunk feed 给协议插件 parse_feed —— */

struct CurlSink {
    Plugin* proto;
    LlmOutcome* out;
    CoreContext* ctx; // 实时广播 message_update 到推送流（流式打字）
};

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<CurlSink*>(userdata);
    const size_t n = size * nmemb;
    char* chunk = static_cast<char*>(malloc(n + 1));
    if (!chunk) return 0;
    memcpy(chunk, ptr, n);
    chunk[n] = '\0';
    s->proto->api->parse_feed(s->proto->instance, chunk, feed_sink, s);
    free(chunk);
    return n;
}

/* parse_feed 事件接收器（协议插件 → agent） */
static void feed_sink(void* sink_ctx, const char* type, const char* payload) {
    auto* s = static_cast<CurlSink*>(sink_ctx);
    auto* out = s->out;
    const json ev = json::parse(payload).value_or(json{});
    const std::string t(type ? type : "");
    if (t == "message_update") {
        out->text += ev["delta"].as_string().value_or("");
        // 实时广播增量（推送流 → TUI 打字效果）
        if (s->ctx->emit_fn) s->ctx->emit_fn("message_update", payload);
    } else if (t == "tool_use") {
        LlmOutcome::ToolUse tu;
        tu.id = ev["id"].as_string().value_or("");
        tu.name = ev["name"].as_string().value_or("");
        tu.input = ev["input"].is_null() ? "{}" : json(ev["input"]).dump();
        out->tool_uses.push_back(std::move(tu));
    } else if (t == "stop") {
        out->stop_reason = ev["reason"].as_string().value_or("stop");
    }
}

bool Agent::llm_call(const json& dialog, LlmOutcome& out) {
    Plugin* proto = find_protocol_plugin(ctx_);
    if (!proto) {
        fprintf(stderr, "[agent] 未找到协议插件（protocol），无法调用 LLM\n");
        return false;
    }
    // 协议插件构造请求（url + body）
    plugin_request_t req{};
    const std::string dialog_json = dialog.dump();
    if (proto->api->build_request(proto->instance, dialog_json.c_str(), &req) != PLUGIN_OK) {
        fprintf(stderr, "[agent] build_request 失败\n");
        return false;
    }

    // libcurl 流式 POST
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    CurlSink sink{proto, &out, &ctx_};
    curl_easy_setopt(curl, CURLOPT_URL, req.url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    // 请求头：解析协议插件返回的 headers JSON → curl_slist
    struct curl_slist* hdrs = nullptr;
    if (req.headers) {
        if (auto h = json::parse(req.headers)) {
            for (const auto& [k, v] : h->as_object()) {
                std::string line = std::string(k) + ": " +
                                   json(v).as_string().value_or("");
                hdrs = curl_slist_append(hdrs, line.c_str());
            }
        }
    }
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode rc = curl_easy_perform(curl);
    // 通知插件流结束（flush）
    proto->api->parse_feed(proto->instance, nullptr, feed_sink, &out);
    curl_easy_cleanup(curl);
    if (hdrs) curl_slist_free_all(hdrs);
    // 释放协议插件分配的请求内存（api->free = delete[]）
    if (proto->api->free) {
        proto->api->free(proto->instance, const_cast<char*>(req.url));
        proto->api->free(proto->instance, const_cast<char*>(req.headers));
        proto->api->free(proto->instance, const_cast<char*>(req.body));
    }
    if (rc != CURLE_OK) {
        fprintf(stderr, "[agent] curl 失败: %s\n", curl_easy_strerror(rc));
        return false;
    }
    return true;
}

json Agent::build_dialog() const {
    json dialog;
    dialog["model"] = ctx_.config->get("model", "deepseek-v4-flash");
    dialog["system"] = "You are a helpful coding agent.";
    // 工具定义（注册表 → schema）
    json tools = json::array();
    for (const auto& [name, t] : ctx_.tools) {
        json tool;
        tool["name"] = t.def.name;
        tool["description"] = t.def.description;
        if (const auto schema = json::parse(t.def.parameters ? t.def.parameters : "{}")) {
            tool["input_schema"] = *schema;
        } else {
            tool["input_schema"] = json{};
        }
        tools.push_back(tool);
    }
    dialog["tools"] = tools;
    dialog["messages"] = messages_;
    return dialog;
}

void Agent::run(const std::string& user_input) {
    // 用户消息入会话
    json um;
    um["role"] = "user";
    um["content"] = json::array();
    json ublock;
    ublock["type"] = "text";
    ublock["text"] = user_input;
    um["content"].push_back(ublock);
    messages_.push_back(um);
    broadcast("message_start", json{{"role", "user"}});

    for (int turn = 0; turn < 50; ++turn) { // 上限防死循环
        broadcast("turn_start", json{});
        LlmOutcome out;
        if (!llm_call(build_dialog(), out)) {
            broadcast("turn_end", json{{"error", "llm_call failed"}});
            break;
        }

        if (!out.tool_uses.empty()) {
            // assistant 消息（tool_use blocks）入会话
            json am;
            am["role"] = "assistant";
            am["content"] = json::array();
            for (const auto& tu : out.tool_uses) {
                json b;
                b["type"] = "tool_use";
                b["id"] = tu.id;
                b["name"] = tu.name;
                b["input"] = json::parse(tu.input).value_or(json{});
                am["content"].push_back(b);
            }
            messages_.push_back(am);

            // 顺序执行工具（单 agent 严格顺序，ADR-0002）
            for (const auto& tu : out.tool_uses) {
                broadcast("tool_execution_start", json{{"name", tu.name}, {"id", tu.id}});
                const auto r = exe_.execute(tu.name, tu.input);
                broadcast("tool_execution_end",
                          json{{"name", tu.name}, {"id", tu.id}, {"status", r.status}});
                // tool_result 入会话
                json tr;
                tr["role"] = "user";
                tr["content"] = json::array();
                json tb;
                tb["type"] = "tool_result";
                tb["tool_use_id"] = tu.id;
                tb["content"] = r.messages;
                if (r.status != 0) tb["is_error"] = true;
                tr["content"].push_back(tb);
                messages_.push_back(tr);
            }
            broadcast("turn_end", json{{"tool_uses", (int)out.tool_uses.size()}});
            continue; // 工具结果回传 → 下一 Turn
        }

        // 无工具调用：assistant 文本入会话，结束
        json am;
        am["role"] = "assistant";
        am["content"] = json::array();
        json b;
        b["type"] = "text";
        b["text"] = out.text;
        am["content"].push_back(b);
        messages_.push_back(am);
        broadcast("turn_end", json{{"stop_reason", out.stop_reason}});
        break;
    }
}

} // namespace realagent
