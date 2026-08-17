#include "agent/agent.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <cstdlib>
#include <algorithm>

namespace realagent {

Agent::Agent(CoreContext& ctx, PluginManager& plugins, Executor& exe)
    : ctx_(ctx), plugins_(plugins), exe_(exe) {
    messages_ = json::array();
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

/* parse_feed 事件接收器（协议插件 → agent）——前向声明 */
static void feed_sink(void* sink_ctx, const char* type, const char* payload);

/* —— curl 写回调：把响应体 chunk feed 给协议插件 parse_feed —— */

struct CurlSink {
    const CapabilitySlots* slots; // 管线的解析段与计价段
    LlmOutcome* out;
    CoreContext* ctx;
    const double* base; // 已完成 turn 的累计花费（本次调用的钱加在它上面才是 run 累计）
    const std::atomic<bool>* abort;
    bool parse_failed = false; // 协议插件报了 REALUGIN_ERR：与用户中断区分开
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
    const auto& parse = s->slots->parse;
    const realugin_status_t rc = parse.fn(parse.self, chunk, feed_sink, s);
    free(chunk);
    // 协议插件解析失败：立刻中止传输。继续读下去只会攒出一个"成功但空"的回答，
    // 那比报错更糟——用户看不出发生了什么。（返回 < n 即令 curl 报 CURLE_WRITE_ERROR）
    if (rc != REALUGIN_OK) {
        s->parse_failed = true;
        return 0;
    }
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
    } else if (t == "usage") {
        // 计价段（ADR-0009/0012）：token 用量换成钱，**usage 事件本身不再上传**——
        // core 不认识 token。算不出（无计价段 / 表里没这个模型）就什么都不发，不发 0
        const auto& meter = s->slots->meter;
        if (!meter) return;
        const double cost = meter.fn(meter.self, payload);
        if (cost <= 0) return;
        out->cost = cost;
        json fwd;
        fwd["cost"] = *s->base + cost; // 推送流里的花费一律是"本次 run 累计"
        if (s->ctx->emit_fn) s->ctx->emit_fn("status_update", fwd.dump());
    } else if (t == "tool_use") {
        LlmOutcome::ToolUse tu;
        tu.id = ev["id"].as_string().value_or("");
        tu.name = ev["name"].as_string().value_or("");
        tu.input = ev["input"].is_null() ? "{}" : json(ev["input"]).dump();
        out->tool_uses.push_back(std::move(tu));
    } else if (t == "status_update") {
        // 运行态帧（ADR-0009）：开放键集，core 只认识 cost（要跨 turn 累加），
        // 其余键原样转发——插件报什么客户端渲染什么，core 不解释、不校验。
        json fwd = ev;
        if (ev.contains("cost")) {
            // 插件给的是本次调用的绝对值（后到覆盖先到），累加只在 Agent 跨 turn 做一次
            out->cost = ev["cost"].as_double().value_or(0);
            // 推送流里的花费一律是"本次 run 累计"：已完成 turn + 当前 turn
            fwd["cost"] = *s->base + out->cost;
        }
        if (s->ctx->emit_fn) s->ctx->emit_fn("status_update", fwd.dump());
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
    // 管线（ADR-0012）：生成请求 → 改请求 →（core 发出）→ 解析响应 → 计价。
    // 每段一个函数，插件互不认识；agent 只知道"有个东西能干这一段"。
    const auto& build = ctx_.slots.build;
    const auto& parse = ctx_.slots.parse;
    if (!build || !parse) {
        fprintf(stderr, "[agent] 管线不完整（生成请求 / 解析响应 缺一），无法调用 LLM\n");
        return false;
    }

    // 第一段：对话 → 粗请求（转移：core 释放）
    const std::string dialog_json = dialog.dump();
    const char* raw = build.fn(build.self, dialog_json.c_str());
    if (!raw) {
        fprintf(stderr, "[agent] 生成请求失败\n");
        return false;
    }
    std::string request(raw);
    std::free(const_cast<char*>(raw));

    // 第二段：粗请求 → 精请求（补端点/模型/凭证）。没有 provider 就原样发出——
    // 那是"裸协议直连"，端点与凭证得用户自己在配置里写全
    if (const auto& refine = ctx_.slots.refine) {
        const char* fine = refine.fn(refine.self, request.c_str());
        if (!fine) {
            fprintf(stderr, "[agent] 改请求失败（不拿粗请求硬发）\n");
            return false;
        }
        request.assign(fine);
        std::free(const_cast<char*>(fine));
    }

    const auto req = json::parse(request).value_or(json{});
    const std::string url = req["url"].as_string().value_or("");
    const std::string body = req["body"].is_null() ? "{}" : json(req["body"]).dump();

    // 第三段：core 发出（网络归 core：超时、中断、证书都只有这一处）
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    CurlSink sink{&ctx_.slots, &out, &ctx_, &run_cost_, &abort_};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &abort_);
    struct curl_slist* hdrs = nullptr;
    for (const auto& [k, v] : req["headers"].as_object()) {
        const std::string line = std::string(k) + ": " + json(v).as_string().value_or("");
        hdrs = curl_slist_append(hdrs, line.c_str());
    }
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode rc = curl_easy_perform(curl);
    // 通知解析段流结束（flush）——sink_ctx 必须与 curl 回调一致
    if (parse.fn(parse.self, nullptr, feed_sink, &sink) != REALUGIN_OK) sink.parse_failed = true;
    curl_easy_cleanup(curl);
    if (hdrs) curl_slist_free_all(hdrs);

    // 解析失败优先报：curl 的 CURLE_WRITE_ERROR 只是我们主动中止的副作用，不是真因
    if (sink.parse_failed) {
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
    // model 不设供应商默认值——外层 provider 壳插件负责兜底
    dialog["model"] = ctx_.config->model(tier);
    dialog["system"] = "You are a helpful coding agent.";
    // 工具定义：现问现答（ADR-0012），core 不存表。
    // 视图里的 name 就是对外名字（带命名空间前缀或短名）——
    // LLM 见到的名字与 executor 查视图用的名字必须是同一个
    json tools = json::array();
    for (const auto& t : tools_of(plugins_)) {
        json tool;
        tool["name"] = t.name;
        tool["description"] = t.def->description;
        if (const auto schema = json::parse(t.def->parameters ? t.def->parameters : "{}")) {
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

        json am;
        am["role"] = "assistant";
        am["content"] = json::array();
        append_thinking(am["content"], out);
        json b;
        b["type"] = "text";
        b["text"] = out.text;
        am["content"].push_back(b);
        record(am);
        broadcast("turn_end", json{{"stop_reason", out.stop_reason}});
        break;
    }
}

} // namespace realagent
