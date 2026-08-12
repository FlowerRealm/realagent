#include "agent/agent.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cstdio>

namespace realagent {

Agent::Agent(CoreContext& ctx, Executor& exe) : ctx_(ctx), exe_(exe) {
    messages_ = json::array();
}

void Agent::reset() {
    messages_ = json::array();
    run_usage_ = Usage{};
    abort_.store(false);
}

void Agent::interrupt() {
    abort_.store(true);
}

void Agent::broadcast(const std::string& type, const json& payload) {
    if (ctx_.emit_fn) ctx_.emit_fn(type, payload.dump());
}

/* 找到协议链入口：最外层协议插件（未被其他协议插件依赖者）。
 * 嵌套链（ADR-0004）：deepseek 壳声明 deps: v1-messages → deepseek 是入口，
 * agent 只调最外层，链内包裹由插件自身经 get_dependency 完成。 */
static Plugin* find_protocol_plugin(CoreContext& ctx) {
    Plugin* fallback = nullptr;
    for (auto* p : ctx.all_plugins) {
        if (!p || !p->api || p->api->type != PLUGIN_TYPE_PROTOCOL ||
            !p->api->build_request || !p->api->parse_feed)
            continue;
        if (!fallback) fallback = p;
        bool depended = false;
        for (auto* q : ctx.all_plugins) {
            if (!q || q == p || !q->api || q->api->type != PLUGIN_TYPE_PROTOCOL) continue;
            if (std::find(q->deps.begin(), q->deps.end(), p->name) != q->deps.end()) {
                depended = true;
                break;
            }
        }
        if (!depended) return p; // 无插件依赖它 = 链的入口
    }
    return fallback; // 无嵌套：退化为第一个协议插件
}

/* parse_feed 事件接收器（协议插件 → agent）——前向声明 */
static void feed_sink(void* sink_ctx, const char* type, const char* payload);

/* —— curl 写回调：把响应体 chunk feed 给协议插件 parse_feed —— */

struct CurlSink {
    Plugin* proto;
    LlmOutcome* out;
    CoreContext* ctx;
    const Usage* base;
    const std::atomic<bool>* abort;
};

static int curl_progress_cb(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* abort = static_cast<const std::atomic<bool>*>(clientp);
    return (abort && abort->load()) ? 1 : 0;
}

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<CurlSink*>(userdata);
    if (s->abort && s->abort->load()) return 0;
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
    } else if (t == "thinking_start") {
        out->thinking_signature = ev["signature"].as_string().value_or("");
        // 实时广播（推送流 → TUI 思考块生命周期：开始）
        if (s->ctx->emit_fn) s->ctx->emit_fn("thinking_start", payload);
    } else if (t == "thinking_update") {
        out->thinking += ev["delta"].as_string().value_or("");
        // 实时广播增量（推送流 → TUI 思考块打字效果）
        if (s->ctx->emit_fn) s->ctx->emit_fn("thinking_update", payload);
    } else if (t == "thinking_stop") {
        // 思考块结束：thinking 已累积；广播收尾帧
        if (s->ctx->emit_fn) s->ctx->emit_fn("thinking_stop", payload);
    } else if (t == "tool_use") {
        LlmOutcome::ToolUse tu;
        tu.id = ev["id"].as_string().value_or("");
        tu.name = ev["name"].as_string().value_or("");
        tu.input = ev["input"].is_null() ? "{}" : json(ev["input"]).dump();
        out->tool_uses.push_back(std::move(tu));
    } else if (t == "usage") {
        // 插件给的是本次调用的绝对值（后到覆盖先到），累加只在 Agent 跨 turn 做一次。
        out->usage.input = ev["input"].as_int64().value_or(0);
        out->usage.output = ev["output"].as_int64().value_or(0);
        out->usage.cache_read = ev["cache_read"].as_int64().value_or(0);
        out->usage.cache_write = ev["cache_write"].as_int64().value_or(0);
        // 推送流里的 usage 帧一律是"本次 run 累计"：已完成 turn + 当前 turn
        if (s->ctx->emit_fn) {
            const Usage total = *s->base + out->usage;
            s->ctx->emit_fn("usage", json{{"input", total.input},
                                          {"output", total.output},
                                          {"cache_read", total.cache_read},
                                          {"cache_write", total.cache_write}}
                                         .dump());
        }
    } else if (t == "stop") {
        out->stop_reason = ev["reason"].as_string().value_or("stop");
    }
}

/* 把 thinking 块追加进 assistant content（Anthropic 格式：thinking + signature）。
 * 思考块恒在正文/tool_use 块之前（协议约定顺序）。 */
static void append_thinking(json& content, const LlmOutcome& out) {
    if (out.thinking.empty()) return;
    json b;
    b["type"] = "thinking";
    b["thinking"] = out.thinking;
    if (!out.thinking_signature.empty()) b["signature"] = out.thinking_signature;
    content.push_back(b);
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
    CurlSink sink{proto, &out, &ctx_, &run_usage_, &abort_};
    curl_easy_setopt(curl, CURLOPT_URL, req.url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &abort_);
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
    // 通知插件流结束（flush）——sink_ctx 必须与 curl 回调一致（feed_sink 按 CurlSink* 解引用）
    proto->api->parse_feed(proto->instance, nullptr, feed_sink, &sink);
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
    // model 不设供应商默认值——外层 provider 壳插件负责兜底
    dialog["model"] = ctx_.config->get("model");
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
    abort_.store(false);
    run_usage_ = Usage{};
    json um;
    um["role"] = "user";
    um["content"] = json::array();
    json ublock;
    ublock["type"] = "text";
    ublock["text"] = user_input;
    um["content"].push_back(ublock);
    messages_.push_back(um);
    broadcast("message_start", json{{"role", "user"}});

    for (int turn = 0; turn < 50; ++turn) {
        if (abort_.load()) {
            broadcast("interrupted", json{});
            break;
        }
        broadcast("turn_start", json{});
        LlmOutcome out;
        if (!llm_call(build_dialog(), out)) {
            if (abort_.load()) {
                if (!out.text.empty() || !out.thinking.empty()) {
                    json am;
                    am["role"] = "assistant";
                    am["content"] = json::array();
                    append_thinking(am["content"], out);
                    if (!out.text.empty()) {
                        json b;
                        b["type"] = "text";
                        b["text"] = out.text;
                        am["content"].push_back(b);
                    }
                    messages_.push_back(am);
                }
                broadcast("interrupted", json{});
            } else {
                broadcast("turn_end", json{{"error", "llm_call failed"}});
            }
            break;
        }
        run_usage_ += out.usage;

        if (!out.tool_uses.empty()) {
            json am;
            am["role"] = "assistant";
            am["content"] = json::array();
            append_thinking(am["content"], out);
            for (const auto& tu : out.tool_uses) {
                json b;
                b["type"] = "tool_use";
                b["id"] = tu.id;
                b["name"] = tu.name;
                b["input"] = json::parse(tu.input).value_or(json{});
                am["content"].push_back(b);
            }
            messages_.push_back(am);

            size_t executed = 0;
            for (const auto& tu : out.tool_uses) {
                if (abort_.load()) break;
                broadcast("tool_execution_start", json{{"name", tu.name}, {"id", tu.id}});
                const auto r = exe_.execute(tu.name, tu.input);
                broadcast("tool_execution_end",
                          json{{"name", tu.name}, {"id", tu.id}, {"status", r.status}});
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
                ++executed;
            }
            if (abort_.load()) {
                for (size_t i = executed; i < out.tool_uses.size(); ++i) {
                    json tr;
                    tr["role"] = "user";
                    tr["content"] = json::array();
                    json tb;
                    tb["type"] = "tool_result";
                    tb["tool_use_id"] = out.tool_uses[i].id;
                    tb["content"] = "interrupted by user";
                    tb["is_error"] = true;
                    tr["content"].push_back(tb);
                    messages_.push_back(tr);
                }
                broadcast("interrupted", json{});
                break;
            }
            broadcast("turn_end", json{{"tool_uses", (int)out.tool_uses.size()}});
            continue;
        }

        json am;
        am["role"] = "assistant";
        am["content"] = json::array();
        append_thinking(am["content"], out);
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
