/*
 * test_protocol_plugin.cpp — 协议链插件单元测试
 *
 * 不依赖真实 API：dlopen v1-messages（协议层）+ deepseek（壳）后直接调
 * build_request / parse_feed，验证：
 *   - v1-messages：抽象对话 → /v1/messages 请求体（供应商无关，无默认端点/模型）
 *   - v1-messages：SSE → 事件（含 thinking 三帧）
 *   - deepseek 壳：兜底 DeepSeek 默认端点/模型；已配置值原样透传；claude-* 模型不做映射
 *   - deepseek 壳：parse_feed 透传（thinking 事件经壳流出）
 *   - deepseek 壳：拦 usage 换 cost（ADR-0009），token 不再上传；list_models 不报单价
 */
#include <dlfcn.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <plugin_api.h>

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

namespace bj = boost::json;

static int failures = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (cond) {                                                             \
            printf("  ok: %s\n", msg);                                          \
        } else {                                                                \
            printf("  FAIL: %s\n", msg);                                        \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

/* —— fake core（get_config 可改写：同一实例链不同配置分支测试） —— */
static struct { const char* base_url; const char* api_key; const char* model; } g_cfg = {
    "", "test-key-123", ""};
/* 模型数据表路径（core 给的，见 ADR-0009）：测试用临时表，单价取整数好算 */
static std::string g_models_path;

static const char* fake_get_config(plugin_core_t*, const char* key) {
    if (std::strcmp(key, "api_key") == 0) return g_cfg.api_key;
    if (std::strcmp(key, "base_url") == 0) return g_cfg.base_url;
    if (std::strcmp(key, "model") == 0) return g_cfg.model;
    if (std::strcmp(key, "models_path") == 0) return g_models_path.c_str();
    return "";
}
static plugin_status_t fake_register_tool(plugin_core_t*, const plugin_tool_t*) { return PLUGIN_OK; }
static plugin_status_t fake_register_command(plugin_core_t*, const plugin_command_t*) { return PLUGIN_OK; }
static plugin_status_t fake_emit(plugin_core_t*, const char*, const char*) { return PLUGIN_OK; }
static void fake_log(plugin_core_t*, int, const char*) {}
static plugin_status_t fake_depends(plugin_core_t*, const char*) { return PLUGIN_OK; }

/* get_dependency：返回已注册的内层（v1-messages） */
static const plugin_api_t* g_dep_api = nullptr;
static plugin_t* g_dep_inst = nullptr;
static plugin_status_t fake_get_dependency(plugin_core_t*, const char* name,
                                           const plugin_api_t** out_api, plugin_t** out_inst) {
    if (std::strcmp(name, "v1-messages") != 0 || !g_dep_api || !g_dep_inst) return PLUGIN_ERR;
    *out_api = g_dep_api;
    *out_inst = g_dep_inst;
    return PLUGIN_OK;
}

static const plugin_core_api_t k_fake_core_api = {
    .register_tool = fake_register_tool,
    .register_command = fake_register_command,
    .emit = fake_emit,
    .log = fake_log,
    .get_config = fake_get_config,
    .depends_on = fake_depends,
    .get_dependency = fake_get_dependency,
};

/* —— 工具：dlopen + create —— */
static void* dl(const char* name) {
    void* h = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen((std::string("build/") + name).c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) fprintf(stderr, "dlopen %s 失败: %s\n", name, dlerror());
    return h;
}
static plugin_t* create_inst(void* h, const plugin_api_t** api) {
    auto fn = (plugin_create_fn)dlsym(h, PLUGIN_CREATE_SYM);
    if (!fn) return nullptr;
    plugin_t* inst = fn(api);
    return inst;
}
static void free_request(const plugin_api_t* api, plugin_t* inst, const plugin_request_t& r) {
    if (api->free) {
        api->free(inst, const_cast<char*>(r.url));
        api->free(inst, const_cast<char*>(r.headers));
        api->free(inst, const_cast<char*>(r.body));
    }
}

/* 测试 1：v1-messages build_request（配置了端点：协议层直连；thinking 历史回传） */
static void test_v1_build_request(const plugin_api_t* api, plugin_t* inst) {
    printf("[v1-messages build_request]\n");
    const char* dialog = R"({
        "model": "deepseek-v4-flash",
        "system": "You are a coding agent.",
        "messages": [
            {"role":"user","content":[{"type":"text","text":"hello"}]},
            {"role":"assistant","content":[
                {"type":"thinking","thinking":"先分析问题","signature":"sig-abc"},
                {"type":"text","text":"world"}
            ]}
        ],
        "tools": [
            {"name":"read","description":"读文件","input_schema":{"type":"object","properties":{"file_path":{"type":"string"}}}}
        ]
    })";
    plugin_request_t req{};
    CHECK(api->build_request(inst, dialog, &req) == PLUGIN_OK, "build_request 返回 OK");
    if (req.url) {
        CHECK(std::string(req.url).find("/v1/messages") != std::string::npos, "url 含 /v1/messages");
        CHECK(std::string(req.url).find("http://127.0.0.1:18080") != std::string::npos,
              "url 用配置 base_url（协议层直连）");
    }
    if (req.headers) {
        CHECK(std::string(req.headers).find("Bearer test-key-123") != std::string::npos,
              "headers 含 Bearer api_key");
        CHECK(std::string(req.headers).find("anthropic-version") != std::string::npos,
              "headers 含 anthropic-version（协议固有）");
    }
    if (req.body) {
        boost::system::error_code ec;
        const bj::value body = bj::parse(req.body, ec);
        CHECK(!ec, "body 是合法 JSON");
        if (!ec) {
            const auto& o = body.as_object();
            CHECK(o.at("stream").as_bool() == true, "stream=true");
            CHECK(bj::value_to<std::string>(o.at("model")) == "deepseek-v4-flash",
                  "model 从对话透传");
            const auto& msgs = o.at("messages").as_array();
            const bj::object& asst = msgs.at(1).as_object();
            const auto& blocks = asst.at("content").as_array();
            CHECK(bj::value_to<std::string>(blocks.at(0).as_object().at("type")) == "thinking",
                  "assistant[0].type=thinking");
            CHECK(bj::value_to<std::string>(blocks.at(0).as_object().at("thinking")) == "先分析问题",
                  "thinking 内容原样回传");
            CHECK(bj::value_to<std::string>(blocks.at(0).as_object().at("signature")) == "sig-abc",
                  "thinking signature 原样回传");
            CHECK(bj::value_to<std::string>(blocks.at(1).as_object().at("type")) == "text",
                  "assistant[1].type=text（thinking 块在正文前）");
        }
    }
    free_request(api, inst, req);
}

/* 测试 2：v1-messages parse_feed（text + tool_use + thinking 事件） */
static void test_v1_parse_feed(const plugin_api_t* api, plugin_t* inst) {
    printf("[v1-messages parse_feed]\n");
    struct Sink {
        std::vector<std::string> updates;
        std::vector<std::string> tools;
        std::string stop_reason;
        std::string thinking;        // thinking_update 增量拼接
        std::string thinking_start;  // thinking_start 载荷（signature）
        int thinking_stops = 0;
    } sink;
    const auto cb = [](void* ctx, const char* type, const char* payload) {
        auto* s = static_cast<Sink*>(ctx);
        const std::string t(type ? type : "");
        if (t == "message_update") s->updates.emplace_back(payload);
        else if (t == "tool_use") s->tools.emplace_back(payload);
        else if (t == "stop") s->stop_reason = payload;
        else if (t == "thinking_start") s->thinking_start = payload;
        else if (t == "thinking_update") s->thinking += payload;
        else if (t == "thinking_stop") ++s->thinking_stops;
    };

    // 分两段喂（模拟 curl chunk 边界切在事件中间）
    const char* chunk1 =
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"你好\"}}\n\n";
    const char* chunk2 =
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"世界\"}}\n\n"
        "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
        "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n";
    api->parse_feed(inst, chunk1, cb, &sink);
    api->parse_feed(inst, chunk2, cb, &sink);
    api->parse_feed(inst, nullptr, cb, &sink); // flush

    CHECK(sink.updates.size() == 2, "产出 2 个 message_update");
    CHECK(sink.updates.size() >= 2 && sink.updates[0].find("你好") != std::string::npos,
          "message_update[0] 文本=你好");
    CHECK(sink.updates.size() >= 2 && sink.updates[1].find("世界") != std::string::npos,
          "message_update[1] 文本=世界");
    CHECK(sink.stop_reason.find("end_turn") != std::string::npos, "stop reason=end_turn");

    // tool_use block（input_json_delta 累积 → JSON）
    const char* chunk3 =
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"read\",\"input\":{}}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"file_path\\\":\"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"\\\"a.txt\\\"}\"}}\n\n"
        "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
        "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"}}\n\n";
    sink.updates.clear();
    sink.tools.clear();
    sink.stop_reason.clear();
    api->parse_feed(inst, chunk3, cb, &sink);
    api->parse_feed(inst, nullptr, cb, &sink);
    CHECK(sink.tools.size() == 1, "产出 1 个 tool_use");
    if (!sink.tools.empty()) {
        const std::string& tu = sink.tools[0];
        boost::system::error_code ec;
        const bj::value v = bj::parse(tu, ec);
        CHECK(!ec, "tool_use 是合法 JSON");
        if (!ec) {
            const auto& o = v.as_object();
            CHECK(bj::value_to<std::string>(o.at("name")) == "read", "tool_use.name=read");
            CHECK(bj::value_to<std::string>(o.at("input").as_object().at("file_path")) == "a.txt",
                  "tool_use.input.file_path=a.txt（partial_json 累积）");
        }
    }
    CHECK(sink.stop_reason.find("tool_use") != std::string::npos, "stop reason=tool_use");

    // thinking block（协议固有：thinking_start/thinking_update/thinking_stop）
    const char* chunk4 =
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"thinking\",\"thinking\":\"初始想法\",\"signature\":\"sig-1\"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"我需要\"}}\n\n";
    const char* chunk5 =
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"先读文件\"}}\n\n"
        "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":1,"
        "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"答案\"}}\n\n"
        "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n";
    sink.thinking.clear();
    sink.thinking_start.clear();
    sink.thinking_stops = 0;
    sink.updates.clear();
    sink.stop_reason.clear();
    api->parse_feed(inst, chunk4, cb, &sink);
    api->parse_feed(inst, chunk5, cb, &sink);
    api->parse_feed(inst, nullptr, cb, &sink);
    CHECK(sink.thinking_start.find("sig-1") != std::string::npos, "thinking_start 带 signature");
    CHECK(sink.thinking.find("初始想法") != std::string::npos && sink.thinking.find("我需要") != std::string::npos &&
              sink.thinking.find("先读文件") != std::string::npos,
          "thinking_update 增量拼接（起始文本 + 跨 chunk delta）");
    CHECK(sink.thinking_stops == 1, "thinking_stop 产出 1 次");
    CHECK(!sink.updates.empty() && sink.updates[0].find("答案") != std::string::npos,
          "thinking 块结束后 text 块正常解析");
    CHECK(sink.stop_reason.find("end_turn") != std::string::npos, "stop reason=end_turn");
}

/* 测试 3：deepseek 壳 build_request —— 未配置时兜底默认，已配置透传，claude 模型不做映射 */
static void test_deepseek_build_request(const plugin_api_t* api, plugin_t* inst,
                                        const char* want_url) {
    printf("[deepseek 壳 build_request] want_url=%s\n", want_url);
    // 对话不带 model：壳兜底默认模型
    const char* dialog_nomodel = R"({
        "model": "",
        "system": "You are a coding agent.",
        "messages": [{"role":"user","content":[{"type":"text","text":"hi"}]}]
    })";
    plugin_request_t req{};
    CHECK(api->build_request(inst, dialog_nomodel, &req) == PLUGIN_OK, "build_request 返回 OK");
    if (req.url) {
        CHECK(std::string(req.url) == want_url, "url = 壳的端点（默认或配置）");
    }
    if (req.headers) {
        CHECK(std::string(req.headers).find("Bearer test-key-123") != std::string::npos,
              "Authorization 由协议层/壳补上");
        CHECK(std::string(req.headers).find("anthropic-version") != std::string::npos,
              "anthropic-version 保留");
    }
    if (req.body) {
        boost::system::error_code ec;
        const bj::value body = bj::parse(req.body, ec);
        CHECK(!ec, "body 是合法 JSON");
        if (!ec) {
            CHECK(bj::value_to<std::string>(body.as_object().at("model")) == "deepseek-v4-flash",
                  "model 留空 → 壳兜底默认 deepseek-v4-flash");
        }
    }
    free_request(api, inst, req);

    // 对话带 claude 模型：壳不做映射，原样透传（无供应商特殊逻辑）
    const char* dialog_claude = R"({
        "model": "claude-sonnet-4-5",
        "messages": [{"role":"user","content":[{"type":"text","text":"hi"}]}]
    })";
    plugin_request_t req2{};
    CHECK(api->build_request(inst, dialog_claude, &req2) == PLUGIN_OK, "build_request(claude) 返回 OK");
    if (req2.body) {
        boost::system::error_code ec;
        const bj::value body = bj::parse(req2.body, ec);
        CHECK(!ec, "body 是合法 JSON");
        if (!ec) {
            CHECK(bj::value_to<std::string>(body.as_object().at("model")) == "claude-sonnet-4-5",
                  "claude-* 模型不做映射，原样透传");
        }
    }
    free_request(api, inst, req2);
}

/* 测试 4：deepseek 壳 parse_feed——事件透传 + usage 拦截换 cost（ADR-0009） */
static void test_deepseek_parse_feed(const plugin_api_t* api, plugin_t* inst) {
    printf("[deepseek 壳 parse_feed 透传 + 计价]\n");
    // 先构造一次请求，把本次生效的模型名钉死（算钱按它查单价）
    const char* dialog = R"({
        "model": "deepseek-v4-flash",
        "system": "s",
        "messages": [{"role":"user","content":[{"type":"text","text":"hi"}]}],
        "tools": []
    })";
    plugin_request_t req{};
    api->build_request(inst, dialog, &req);
    free_request(api, inst, req);

    struct Sink {
        std::string thinking_start;
        std::string thinking;
        int thinking_stops = 0;
        std::string text;
        std::string usage;
        std::string status_update;
    } sink;
    const auto cb = [](void* ctx, const char* type, const char* payload) {
        auto* s = static_cast<Sink*>(ctx);
        const std::string t(type ? type : "");
        if (t == "thinking_start") s->thinking_start = payload;
        else if (t == "thinking_update") s->thinking += payload;
        else if (t == "thinking_stop") ++s->thinking_stops;
        else if (t == "message_update") s->text += payload;
        else if (t == "usage") s->usage = payload;
        else if (t == "status_update") s->status_update = payload;
    };
    const char* chunk =
        "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"m1\","
        "\"usage\":{\"input_tokens\":77}}}\n\n"
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"sig-9\"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"想一下\"}}\n\n"
        "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":1,"
        "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"答案\"}}\n\n"
        "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n";
    api->parse_feed(inst, chunk, cb, &sink);
    api->parse_feed(inst, nullptr, cb, &sink);
    CHECK(sink.thinking_start.find("sig-9") != std::string::npos, "thinking_start 经壳透传");
    CHECK(sink.thinking.find("想一下") != std::string::npos, "thinking_update 经壳透传");
    CHECK(sink.thinking_stops == 1, "thinking_stop 经壳透传");
    CHECK(sink.text.find("答案") != std::string::npos, "message_update 经壳透传");
    // token 到壳为止：core 只该看见钱
    CHECK(sink.usage.empty(), "usage 被壳吞掉（token 不上传）");
    CHECK(!sink.status_update.empty(), "壳报出 status_update");
    if (!sink.status_update.empty()) {
        boost::system::error_code ec;
        const bj::value v = bj::parse(sink.status_update, ec);
        CHECK(!ec && v.is_object() && v.as_object().contains("cost"), "status_update 带 cost");
        if (!ec && v.is_object() && v.as_object().contains("cost")) {
            // 表里 input 单价 1000/1M，77 token → 0.077
            const double cost = v.as_object().at("cost").to_number<double>();
            CHECK(cost > 0.0769 && cost < 0.0771, "cost = input 77 × 1000/1M = 0.077");
        }
    }
}

/* 测试 4b：list_models 只报公共字段，单价留在壳里（ADR-0009） */
static void test_deepseek_list_models(const plugin_api_t* api, plugin_t* inst) {
    printf("[deepseek list_models]\n");
    const char* text = api->list_models ? api->list_models(inst) : nullptr;
    CHECK(text != nullptr, "壳报出模型清单");
    if (!text) return;
    boost::system::error_code ec;
    const bj::value v = bj::parse(text, ec);
    CHECK(!ec && v.is_array() && v.as_array().size() == 1, "清单是 JSON 数组（1 条）");
    if (ec || !v.is_array() || v.as_array().empty()) return;
    const auto& o = v.as_array()[0].as_object();
    CHECK(bj::value_to<std::string>(o.at("name")) == "deepseek-v4-flash", "name 报上来");
    CHECK(bj::value_to<std::string>(o.at("owned_by")) == "deepseek", "owned_by 报上来");
    CHECK(o.at("context").as_int64() == 131072, "context 报上来");
    CHECK(!o.contains("pricing"), "单价不报给 core");
}

/* 测试 5：v1-messages usage 事件（message_start 给 input，message_delta 给 output，合并上报） */
static void test_v1_usage(const plugin_api_t* api, plugin_t* inst) {
    printf("[v1-messages usage]\n");
    struct Sink {
        std::vector<std::string> usages;
        std::vector<std::string> order; // 事件顺序（验证 usage 先于 stop）
    } sink;
    const auto cb = [](void* ctx, const char* type, const char* payload) {
        auto* s = static_cast<Sink*>(ctx);
        const std::string t(type ? type : "");
        s->order.emplace_back(t);
        if (t == "usage") s->usages.emplace_back(payload);
    };

    const char* chunk =
        "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"m1\","
        "\"usage\":{\"input_tokens\":1200,\"output_tokens\":1,"
        "\"cache_read_input_tokens\":300,\"cache_creation_input_tokens\":0}}}\n\n"
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"答\"}}\n\n"
        "event: message_delta\ndata: {\"type\":\"message_delta\","
        "\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":842}}\n\n";
    api->parse_feed(inst, chunk, cb, &sink);
    api->parse_feed(inst, nullptr, cb, &sink);

    CHECK(sink.usages.size() == 2, "产出 2 个 usage 事件（message_start + message_delta）");
    if (sink.usages.size() == 2) {
        boost::system::error_code ec;
        const bj::value first = bj::parse(sink.usages[0], ec);
        const bj::value last = bj::parse(sink.usages[1], ec);
        CHECK(!ec, "usage 是合法 JSON");
        if (!ec) {
            const auto& f = first.as_object();
            CHECK(f.at("input").as_int64() == 1200, "首帧 input=1200");
            CHECK(f.at("cache_read").as_int64() == 300, "首帧 cache_read=300");
            const auto& l = last.as_object();
            CHECK(l.at("output").as_int64() == 842, "末帧 output=842");
            CHECK(l.at("input").as_int64() == 1200,
                  "末帧仍带 input=1200（缺字段的帧不清零，插件内合并为完整一组）");
        }
    }
    // usage 必须先于 stop（客户端收工时数字已定）
    const auto usage_pos = std::find(sink.order.begin(), sink.order.end(), "usage");
    const auto stop_pos = std::find(sink.order.begin(), sink.order.end(), "stop");
    CHECK(usage_pos < stop_pos, "usage 事件先于 stop 送出");

    // 端点不给 usage：一个 usage 事件都不发（客户端按"无数据"处理，不显示为 0）
    sink.usages.clear();
    sink.order.clear();
    const char* no_usage =
        "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"m2\"}}\n\n"
        "event: message_delta\ndata: {\"type\":\"message_delta\","
        "\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n";
    api->parse_feed(inst, no_usage, cb, &sink);
    api->parse_feed(inst, nullptr, cb, &sink);
    CHECK(sink.usages.empty(), "无 usage 字段时不发 usage 事件（且上条 message 的计数已清零）");
}

int main() {
    void* h1 = dl("v1-messages.dylib");
    void* h2 = dl("deepseek.dylib");
    if (!h1 || !h2) return 1;

    const plugin_api_t* v1_api = nullptr;
    const plugin_api_t* ds_api = nullptr;

    // —— v1a：空配置（协议层无供应商默认，壳负责兜底） ——
    g_cfg = {"", "test-key-123", ""};
    plugin_t* v1a = create_inst(h1, &v1_api);
    plugin_core_t core_handle{&k_fake_core_api, nullptr};
    if (!v1a || !v1_api || v1_api->init(v1a, &core_handle) != PLUGIN_OK) {
        fprintf(stderr, "v1-messages init 失败\n");
        return 1;
    }
    g_dep_api = v1_api; // get_dependency 目标：deepseek 壳包住 v1a
    g_dep_inst = v1a;

    // 模型数据表（ADR-0009）：壳自读自解析，单价取整数便于核对算出来的钱。
    // 峰谷两张表填同一组数——测试不能因为跑在几点而结果不同
    g_models_path = "test_models.json";
    {
        std::ofstream f(g_models_path);
        f << R"([{"name":"deepseek-v4-flash","owned_by":"deepseek","context":131072,)"
             R"("pricing":{"peak_hours_utc":[[1,4],[6,10]],)"
             R"("peak":{"input":1000,"output":2000,"cache_read":0},)"
             R"("off_peak":{"input":1000,"output":2000,"cache_read":0}}}])";
    }

    // —— deepseek_default：空配置 → 壳默认 DeepSeek 端点/模型 ——
    plugin_t* ds_default = create_inst(h2, &ds_api);
    if (!ds_default || !ds_api || ds_api->init(ds_default, &core_handle) != PLUGIN_OK) {
        fprintf(stderr, "deepseek init 失败\n");
        return 1;
    }

    // —— v1b：配置了端点（协议层可用配置直连） ——
    g_cfg = {"http://127.0.0.1:18080", "test-key-123", ""};
    plugin_t* v1b = create_inst(h1, &v1_api);
    if (!v1b || v1_api->init(v1b, &core_handle) != PLUGIN_OK) {
        fprintf(stderr, "v1-messages(配置) init 失败\n");
        return 1;
    }

    // —— deepseek_configured：配置了端点 → 壳用配置透传 ——
    plugin_t* ds_cfg = create_inst(h2, &ds_api);
    if (!ds_cfg || ds_api->init(ds_cfg, &core_handle) != PLUGIN_OK) {
        fprintf(stderr, "deepseek(配置) init 失败\n");
        return 1;
    }

    test_v1_build_request(v1_api, v1b);
    test_v1_parse_feed(v1_api, v1a);
    test_deepseek_build_request(ds_api, ds_default,
                                "https://api.deepseek.com/anthropic/v1/messages");
    test_deepseek_build_request(ds_api, ds_cfg, "http://127.0.0.1:18080/v1/messages");
    test_deepseek_parse_feed(ds_api, ds_default);
    test_deepseek_list_models(ds_api, ds_default);
    test_v1_usage(v1_api, v1a);

    if (v1_api->destroy) { v1_api->destroy(v1b); v1_api->destroy(v1a); }
    if (ds_api->destroy) { ds_api->destroy(ds_cfg); ds_api->destroy(ds_default); }
    dlclose(h1);
    dlclose(h2);

    printf("\n%s: %d 失败\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
