#include "agent/agent.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <cstdlib>

namespace realagent {

Agent::Agent(CoreContext& ctx, Executor& exe) : ctx_(ctx), exe_(exe) {
    messages_ = json::array();
    // 模型数据表读一次。读不动就报出来，之后照常跑——没有表只是不算钱，
    // 不是"不能对话"，为它拒绝启动是把一个次要功能提成了必需品
    std::string err;
    pricing_ = Pricing::load(*ctx_.config, &err);
    if (!err.empty()) fprintf(stderr, "[llm] %s（本次运行不计价）\n", err.c_str());
}

void Agent::reset() {
    messages_ = json::array();
    session_ = Session(); // 换个新会话文件；旧的留在盘上，它是记录不是缓存
    run_cost_ = 0;
    abort_.store(false);
}

bool Agent::resume(const std::string& id) {
    json loaded;
    Session s;
    if (!s.resume(id, loaded)) return false; // 先在副本上试，成了才动自己
    session_ = std::move(s);
    messages_ = std::move(loaded);
    run_cost_ = 0;
    abort_.store(false);
    return true;
}

void Agent::record(json msg) {
    session_.append(msg); // 先落盘：进了内存却没进文件，恢复时就是一条凭空消失的消息
    messages_.push_back(std::move(msg));
}

void Agent::interrupt() {
    abort_.store(true);
    // 统一信号（PLAN.md R8）：一个动作同时停住 LLM 流与在跑的工具。
    // 从事件循环线程进来——agent 线程此刻多半正卡在某个工具里，指望不上它自己检查
    exe_.interrupt();
}

void Agent::broadcast(const std::string& type, const json& payload) {
    if (ctx_.emit_fn) ctx_.emit_fn(type, payload.dump());
}

/* 一次 LLM 调用期间的流式状态：解析器 + 落点。
 * 解析器按调用建、按调用扔——上一轮的半截 SSE 缓冲绝不该漏进下一轮。 */
struct StreamCtx {
    Agent* self;
    CURL* curl = nullptr;   // 写回调里问状态码要用（头已经收完了，问得到）
    SseParser parser;       // 协议决定怎么解，一次调用一个实例
    LlmOutcome* out;
    std::string model;      // 本次调用的模型名（计价按它查单价）
    bool parse_failed = false; // 解析报错：与用户中断区分开
    long status = 0;         // 首次拿到响应体时问一次 HTTP 状态码
    std::string error_body;  // 状态码不是 2xx 时，响应体不是流，攒起来给人看
};

static int curl_progress_cb(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* abort = static_cast<const std::atomic<bool>*>(clientp);
    return (abort && abort->load()) ? 1 : 0;
}

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<StreamCtx*>(userdata);
    const size_t n = size * nmemb;

    /* 先看状态码，再谈解析（ADR-0017）。curl 对 401/500 一律返回 CURLE_OK——
     * HTTP 层的失败不是传输层的失败。错误体不是 SSE：喂给解析器切不出事件块，
     * 最后攒出一个"成功但空"的回答，用户看不出发生了什么，会话里还留下一条空消息。 */
    if (s->status == 0) curl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &s->status);
    if (s->status >= 400) {
        // 攒错误体给人看，别喂解析器。攒够 8KB 就够说明问题了
        if (s->error_body.size() < 8192) s->error_body.append(ptr, n);
        return n;
    }

    const bool ok = s->parser.feed(std::string_view(ptr, n), [s](std::string_view t, const json& ev) {
        s->self->on_llm_event(t, ev, *s->out, s->model);
    });
    // 解析失败：立刻中止传输。继续读下去只会攒出一个"成功但空"的回答，
    // 那比报错更糟——用户看不出发生了什么。（返回 < n 即令 curl 报 CURLE_WRITE_ERROR）
    if (!ok) {
        s->parse_failed = true;
        return 0;
    }
    return n;
}

void Agent::on_llm_event(std::string_view type, const json& ev, LlmOutcome& out,
                         const std::string& model) {
    if (type == "message_update") {
        out.text += ev["delta"].as_string().value_or("");
        broadcast("message_update", ev); // 实时增量 → TUI 打字效果
    } else if (type == "thinking_start") {
        out.thinking_signature = ev["signature"].as_string().value_or("");
        broadcast("thinking_start", ev);
    } else if (type == "thinking_update") {
        out.thinking += ev["delta"].as_string().value_or("");
        broadcast("thinking_update", ev);
    } else if (type == "thinking_stop") {
        broadcast("thinking_stop", ev);
    } else if (type == "usage") {
        // 计价（ADR-0009）：token 用量换成钱，**usage 事件本身不再上传**——
        // 客户端不认识 token。算不出（表里没这个模型）就什么都不发，不发 0。
        //
        // 本次用的是哪个模型，调用方自己知道（就是 dialog["model"]）——
        // 从前这条信息要靠 provider 壳在改请求时偷偷记一笔，现在直接传进来
        const double cost = pricing_.cost(model, ev);
        if (cost <= 0) return;
        out.cost = cost;
        json fwd;
        fwd["cost"] = run_cost_ + cost; // 推送流里的花费一律是"本次 run 累计"
        broadcast("status_update", fwd);
    } else if (type == "tool_use") {
        LlmOutcome::ToolUse tu;
        tu.id = ev["id"].as_string().value_or("");
        tu.name = ev["name"].as_string().value_or("");
        tu.input = ev["input"].is_null() ? "{}" : json(ev["input"]).dump();
        out.tool_uses.push_back(std::move(tu));
    } else if (type == "stop") {
        out.stop_reason = ev["reason"].as_string().value_or("stop");
    }
}

/* 把 thinking 块追加进 assistant content（thinking + signature）。
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
    const HttpRequest req = build_request(*ctx_.config, dialog);
    if (req.url.rfind("http", 0) != 0) {
        // base_url 没配全，拼出来的是个相对路径。libcurl 会报一句难懂的错，
        // 不如在这儿说人话
        out.error = "base_url 未配置（当前请求 URL: " + req.url + "）";
        fprintf(stderr, "[agent] %s\n", out.error.c_str());
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) return false;
    // 协议在这一层就定下来：解析器建出来那一刻就知道自己要解哪套帧
    StreamCtx s{.self = this,
                .curl = curl,
                .parser = SseParser(*protocol_from(ctx_.config->get("protocol"))),
                .out = &out,
                .model = dialog["model"].as_string().value_or("")};
    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &abort_);
    struct curl_slist* hdrs = nullptr;
    for (const auto& h : req.headers) hdrs = curl_slist_append(hdrs, h.c_str());
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    const CURLcode rc = curl_easy_perform(curl);
    s.parser.flush([](std::string_view, const json&) {});
    curl_easy_cleanup(curl);
    if (hdrs) curl_slist_free_all(hdrs);

    // HTTP 状态码优先报：它是最上游的真因。401 的错误体不是流，
    // 解析器切不出事件也算"没解析失败"——只看 parse_failed 会把认证失败说成成功
    if (const std::string he = http_status_error(s.status, s.error_body); !he.empty()) {
        out.error = he;
        fprintf(stderr, "[agent] %s\n", out.error.c_str());
        return false;
    }
    // 解析失败次之：curl 的 CURLE_WRITE_ERROR 只是我们主动中止的副作用，不是真因
    if (s.parse_failed) {
        out.error = "解析响应失败（见 core 日志）";
        fprintf(stderr, "[agent] %s\n", out.error.c_str());
        return false;
    }
    if (rc != CURLE_OK) {
        out.error = std::string("curl 失败: ") + curl_easy_strerror(rc);
        fprintf(stderr, "[agent] %s\n", out.error.c_str());
        return false;
    }
    return true;
}

json Agent::build_dialog(ModelTier tier) const {
    json dialog;
    dialog["model"] = ctx_.config->model(tier);
    dialog["system"] = "You are a helpful coding agent.";
    // 工具定义：静态表，LLM 见到的名字与 executor 查表用的名字是同一个
    json tools = json::array();
    for (const auto& t : tool_defs()) {
        json tool;
        tool["name"] = t.name;
        tool["description"] = t.description;
        tool["input_schema"] = json::parse(t.parameters).value_or(json{});
        tools.push_back(tool);
    }
    dialog["tools"] = tools;
    dialog["messages"] = messages_;
    return dialog;
}

void Agent::run(const std::string& user_input) {
    abort_.store(false);
    exe_.reset(); // 中止痕迹与 abort_ 同一个生命周期，一起清
    run_cost_ = 0;
    json um;
    um["role"] = "user";
    um["content"] = json::array();
    json ublock;
    ublock["type"] = "text";
    ublock["text"] = user_input;
    um["content"].push_back(ublock);
    record(um);
    broadcast("message_start", json{{"role", "user"}});

    // 不设轮数上限：一个数字定不出"多少轮算跑飞了"——同一个任务，改个错字一轮，
    // 重构一个模块几十轮，两者都正常。真正让循环停下来的是下面四处 abort_ 检查点
    // （用户按中断）、llm_call 失败、以及模型不再要工具，条条都是有据可依的收工理由。
    // 上限只会在最需要它继续的时候把长任务砍断，还不给用户任何解释。
    for (;;) {
        if (abort_.load()) {
            broadcast("interrupted", json{});
            break;
        }
        broadcast("turn_start", json{});
        LlmOutcome out;
        // 对话主链路走主模型；小模型档留给后续杂活调用点（标题/摘要）
        if (!llm_call(build_dialog(ModelTier::Main), out)) {
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
                    record(am);
                }
                broadcast("interrupted", json{});
            } else {
                broadcast("turn_end",
                          json{{"error", out.error.empty() ? "llm_call failed" : out.error}});
            }
            break;
        }
        run_cost_ += out.cost;

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
            record(am);

            size_t executed = 0;
            for (const auto& tu : out.tool_uses) {
                if (abort_.load()) break;
                broadcast("tool_execution_start", json{{"name", tu.name}, {"id", tu.id}});
                const auto r = exe_.execute(tu.id, tu.name, tu.input);
                broadcast("tool_execution_end", json{{"name", tu.name},
                                                     {"id", tu.id},
                                                     {"status", r.status},
                                                     {"interrupted", r.interrupted}});
                json tr;
                tr["role"] = "user";
                tr["content"] = json::array();
                json tb;
                tb["type"] = "tool_result";
                tb["tool_use_id"] = tu.id;
                // 被中断的结果照实说，别混进"命令失败了"里——模型据此判断该不该重试，
                // 这两件事它的反应完全不同。手上那截输出仍然给它，那是真跑出来的
                tb["content"] = r.interrupted
                    ? (r.messages.empty() ? std::string("interrupted by user")
                                          : r.messages + "\n[interrupted by user]")
                    : r.messages;
                if (r.status != 0 || r.interrupted) tb["is_error"] = true;
                tr["content"].push_back(tb);
                record(tr);
                ++executed;
                if (r.interrupted) break;
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
                    record(tr);
                }
                broadcast("interrupted", json{});
                break;
            }
            broadcast("turn_end", json{{"tool_uses", (int)out.tool_uses.size()}});
            continue;
        }

        // 一个字都没有：这不是一次"内容为空的成功"。空 text 块写进会话就是一块砖——
        // 下一轮原样回传，而端点拒收空 text 块，那个会话从此每轮都 400。
        // 报错、不落盘（ADR-0017）
        if (out.text.empty() && out.thinking.empty()) {
            broadcast("turn_end", json{{"error", "端点没有返回任何内容（HTTP 2xx 但流里一个事件都没有）"}});
            fprintf(stderr, "[agent] 空回答：不写入会话\n");
            break;
        }
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
        record(am);
        broadcast("turn_end", json{{"stop_reason", out.stop_reason}});
        break;
    }
}

} // namespace realagent
