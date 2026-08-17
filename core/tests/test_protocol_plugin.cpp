/*
 * test_protocol_plugin.cpp — 协议链插件单元测试
 *
 * 不依赖真实 API：dlopen 两个容器后按管线逐段直调（ADR-0012），验证：
 *   - v1-messages 的 request.build：抽象对话 → 粗请求 JSON（无端点/模型/凭证默认值）
 *   - v1-messages 的 response.parse：SSE → 事件（含 thinking 三帧、usage 合并）
 *   - deepseek 的 request.refine：粗请求 → 精请求（补端点/模型/凭证；claude-* 不做映射）
 *   - deepseek 的 usage.meter：token 用量 → 钱；model.list 不报单价
 * 两个容器互不认识：测试里也是分别取能力、分别调用，没有谁包谁
 */
#include <dlfcn.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <realagent/agent_caps.h>

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

static const char* fake_get_config(realugin_host_t*, const char* key) {
    if (std::strcmp(key, "api_key") == 0) return g_cfg.api_key;
    if (std::strcmp(key, "base_url") == 0) return g_cfg.base_url;
    if (std::strcmp(key, "model") == 0) return g_cfg.model;
    if (std::strcmp(key, "models_path") == 0) return g_models_path.c_str();
    return "";
}
static realugin_status_t fake_emit(realugin_host_t*, const char*, const char*) { return REALUGIN_OK; }
static void fake_log(realugin_host_t*, int, const char*) {}
/* 跨边界内存（ADR-0012）：转移类由 core 分配、core 释放。测试里就是 malloc/free */
static void* fake_alloc(realugin_host_t*, size_t n) { return std::malloc(n); }
static void fake_release(realugin_host_t*, void* p) { std::free(p); }

/* providers：假装 v1-messages 提供 request.build（deepseek 的 init 会问这一句） */
static const char* const k_builders[] = {"v1-messages"};
static size_t fake_providers(realugin_host_t*, const char* cap, const char* const** out) {
    if (std::strcmp(cap, REALAGENT_CAP_REQUEST_BUILD) != 0) return 0;
    *out = k_builders;
    return 1;
}
static realugin_fn_t fake_import(realugin_host_t*, const char*, const char*, realugin_plugin_t**) {
    return nullptr; // 管线上的容器互不取用，本测试不需要
}

static const realugin_host_api_t k_fake_core_api = {
    .emit = fake_emit,
    .log = fake_log,
    .get_config = fake_get_config,
    .alloc = fake_alloc,
    .release = fake_release,
    .providers = fake_providers,
    .import = fake_import,
};

/* —— 工具：dlopen + create ——
 * 容器从哪儿来：环境变量 REALAGENT_PLUGIN_DIR 优先，否则用编译期填进来的
 * RA_PLUGIN_DIR（CMake 变量 REALAGENT_PLUGIN_DIR，默认指向并排的 realagent-plugins/build）。
 * 两个仓库不必并排摆放 —— 摆哪儿都行，告诉它一声即可。 */
static void* dl(const char* name) {
    const char* env = std::getenv("REALAGENT_PLUGIN_DIR");
    const std::string dir = env && *env ? env : RA_PLUGIN_DIR;
    const std::string path = dir.empty() ? name : dir + "/" + name;
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) fprintf(stderr, "dlopen %s 失败: %s\n", path.c_str(), dlerror());
    return h;
}
static realugin_plugin_t* create_inst(void* h, const realugin_plugin_api_t** api) {
    auto fn = (realugin_create_fn)dlsym(h, REALUGIN_CREATE_SYM);
    if (!fn) return nullptr;
    realugin_plugin_t* inst = fn(api);
    return inst;
}
/* 按名取一个能力（core 侧提取器的测试版：查表 + 转型） */
static realugin_fn_t cap(const realugin_plugin_api_t* api, realugin_plugin_t* inst, const char* name) {
    const realugin_capability_t* caps = nullptr;
    const size_t n = api->capabilities ? api->capabilities(inst, &caps) : 0;
    for (size_t i = 0; i < n; ++i)
        if (caps[i].name && std::strcmp(caps[i].name, name) == 0) return caps[i].fn;
    return nullptr;
}

/* 跑一遍管线的前两段：生成请求 →（可选）改请求。返回精请求 JSON（已接管所有权） */
static bj::value run_request(const realugin_plugin_api_t* v1_api, realugin_plugin_t* v1, const char* dialog,
                             const realugin_plugin_api_t* ds_api = nullptr, realugin_plugin_t* ds = nullptr) {
    auto build = (realagent_request_build_fn)cap(v1_api, v1, REALAGENT_CAP_REQUEST_BUILD);
    const char* raw = build(v1, dialog);
    std::string text(raw ? raw : "");
    std::free(const_cast<char*>(raw));
    if (ds_api) {
        auto refine = (realagent_request_refine_fn)cap(ds_api, ds, REALAGENT_CAP_REQUEST_REFINE);
        const char* fine = refine(ds, text.c_str());
        text.assign(fine ? fine : "");
        std::free(const_cast<char*>(fine));
    }
    boost::system::error_code ec;
    bj::value v = bj::parse(text, ec);
    return ec ? bj::value{} : v;
}

/* 测试 1：v1-messages build_request（配置了端点：协议层直连；thinking 历史回传） */
static void test_v1_build_request(const realugin_plugin_api_t* api, realugin_plugin_t* inst) {
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
    const bj::value req = run_request(api, inst, dialog);
    CHECK(req.is_object(), "粗请求是合法 JSON 对象");
    if (req.is_object()) {
        const auto& r = req.as_object();
        const std::string url = bj::value_to<std::string>(r.at("url"));
        CHECK(url.find("/v1/messages") != std::string::npos, "url 含 /v1/messages");
        CHECK(url.find("http://127.0.0.1:18080") != std::string::npos,
              "url 用配置 base_url（协议层直连）");
        const std::string hdrs = bj::serialize(r.at("headers"));
        CHECK(hdrs.find("Bearer test-key-123") != std::string::npos, "headers 含 Bearer api_key");
        CHECK(hdrs.find("anthropic-version") != std::string::npos,
              "headers 含 anthropic-version（协议固有）");
        {
            const auto& o = r.at("body").as_object();
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
}

/* 测试 2：v1-messages parse_feed（text + tool_use + thinking 事件） */
static void test_v1_parse_feed(const realugin_plugin_api_t* api, realugin_plugin_t* inst) {
    printf("[v1-messages response.parse]\n");
    auto parse = (realagent_response_parse_fn)cap(api, inst, REALAGENT_CAP_RESPONSE_PARSE);
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
    parse(inst, chunk1, cb, &sink);
    parse(inst, chunk2, cb, &sink);
    parse(inst, nullptr, cb, &sink); // flush

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
    parse(inst, chunk3, cb, &sink);
    parse(inst, nullptr, cb, &sink);
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
    parse(inst, chunk4, cb, &sink);
    parse(inst, chunk5, cb, &sink);
    parse(inst, nullptr, cb, &sink);
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
static void test_deepseek_refine(const realugin_plugin_api_t* v1_api, realugin_plugin_t* v1,
                                 const realugin_plugin_api_t* ds_api, realugin_plugin_t* ds,
                                 const char* want_url) {
    printf("[deepseek request.refine] want_url=%s\n", want_url);
    // 对话不带 model：粗请求里 model 为空，改请求那一段填供应商默认
    const char* dialog_nomodel = R"({
        "model": "",
        "system": "You are a coding agent.",
        "messages": [{"role":"user","content":[{"type":"text","text":"hi"}]}]
    })";
    const bj::value req = run_request(v1_api, v1, dialog_nomodel, ds_api, ds);
    CHECK(req.is_object(), "精请求是合法 JSON 对象");
    if (req.is_object()) {
        const auto& r = req.as_object();
        CHECK(bj::value_to<std::string>(r.at("url")) == want_url, "url = 供应商端点 + 协议路径");
        const std::string hdrs = bj::serialize(r.at("headers"));
        CHECK(hdrs.find("Bearer test-key-123") != std::string::npos, "Authorization 已补上");
        CHECK(hdrs.find("anthropic-version") != std::string::npos, "anthropic-version 保留");
        CHECK(bj::value_to<std::string>(r.at("body").as_object().at("model")) == "deepseek-v4-flash",
              "model 留空 → 改请求填供应商默认 deepseek-v4-flash");
    }

    // 对话带 claude 模型：不做映射，原样透传（无供应商特殊逻辑）
    const char* dialog_claude = R"({
        "model": "claude-sonnet-4-5",
        "messages": [{"role":"user","content":[{"type":"text","text":"hi"}]}]
    })";
    const bj::value req2 = run_request(v1_api, v1, dialog_claude, ds_api, ds);
    CHECK(req2.is_object(), "精请求（claude）是合法 JSON 对象");
    if (req2.is_object())
        CHECK(bj::value_to<std::string>(req2.as_object().at("body").as_object().at("model")) ==
                  "claude-sonnet-4-5",
              "claude-* 模型不做映射，原样透传");
}

/* 测试 4：解析段产出 usage，计价段把它换成钱——两段分属两个容器，
 * 串起来的是 core（ADR-0012）。此处照 core 的做法手工串一次。 */
static void test_meter(const realugin_plugin_api_t* v1_api, realugin_plugin_t* v1, const realugin_plugin_api_t* ds_api,
                       realugin_plugin_t* ds) {
    printf("[usage.meter 计价]\n");
    // 先跑一次管线前两段：改请求那一步会把本次生效的模型名钉死，计价按它查单价
    const char* dialog = R"({
        "model": "deepseek-v4-flash",
        "messages": [{"role":"user","content":[{"type":"text","text":"hi"}]}]
    })";
    run_request(v1_api, v1, dialog, ds_api, ds);

    struct Sink {
        std::string usage;
        std::string text;
    } sink;
    const auto cb = [](void* ctx, const char* type, const char* payload) {
        auto* s = static_cast<Sink*>(ctx);
        const std::string t(type ? type : "");
        if (t == "usage") s->usage = payload;
        else if (t == "message_update") s->text += payload;
    };
    auto parse = (realagent_response_parse_fn)cap(v1_api, v1, REALAGENT_CAP_RESPONSE_PARSE);
    const char* chunk =
        "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"m1\","
        "\"usage\":{\"input_tokens\":77}}}\n\n"
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"答案\"}}\n\n"
        "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n";
    parse(v1, chunk, cb, &sink);
    parse(v1, nullptr, cb, &sink);

    CHECK(sink.text.find("答案") != std::string::npos, "解析段照常产出正文");
    CHECK(!sink.usage.empty(), "解析段产出 usage 事件");
    auto meter = (realagent_usage_meter_fn)cap(ds_api, ds, REALAGENT_CAP_USAGE_METER);
    CHECK(meter != nullptr, "供应商容器提供计价能力");
    if (meter && !sink.usage.empty()) {
        // 表里 input 单价 1000/1M，77 token → 0.077
        const double cost = meter(ds, sink.usage.c_str());
        CHECK(cost > 0.0769 && cost < 0.0771, "cost = input 77 × 1000/1M = 0.077");
        CHECK(meter(ds, "{\"input\":0,\"output\":0}") == 0, "无用量则不产生费用");
    }
}

/* 测试 4b：model.list 只报公共字段，单价留在容器里（ADR-0009） */
static void test_deepseek_list_models(const realugin_plugin_api_t* api, realugin_plugin_t* inst) {
    printf("[deepseek model.list]\n");
    auto list = (realagent_model_list_fn)cap(api, inst, REALAGENT_CAP_MODEL_LIST);
    const char* text = list ? list(inst) : nullptr;
    CHECK(text != nullptr, "供应商容器报出模型清单");
    if (!text) return;
    boost::system::error_code ec;
    const bj::value v = bj::parse(text, ec); // 借阅：读完即用，不释放（ADR-0012）
    CHECK(!ec && v.is_array() && v.as_array().size() == 1, "清单是 JSON 数组（1 条）");
    if (ec || !v.is_array() || v.as_array().empty()) return;
    const auto& o = v.as_array()[0].as_object();
    CHECK(bj::value_to<std::string>(o.at("name")) == "deepseek-v4-flash", "name 报上来");
    CHECK(bj::value_to<std::string>(o.at("owned_by")) == "deepseek", "owned_by 报上来");
    CHECK(o.at("context").as_int64() == 131072, "context 报上来");
    CHECK(!o.contains("pricing"), "单价不报给 core");
}

/* 测试 5：v1-messages usage 事件（message_start 给 input，message_delta 给 output，合并上报） */
static void test_v1_usage(const realugin_plugin_api_t* api, realugin_plugin_t* inst) {
    printf("[v1-messages usage]\n");
    auto parse = (realagent_response_parse_fn)cap(api, inst, REALAGENT_CAP_RESPONSE_PARSE);
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
    parse(inst, chunk, cb, &sink);
    parse(inst, nullptr, cb, &sink);

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
    parse(inst, no_usage, cb, &sink);
    parse(inst, nullptr, cb, &sink);
    CHECK(sink.usages.empty(), "无 usage 字段时不发 usage 事件（且上条 message 的计数已清零）");
}

int main() {
    void* h1 = dl("v1-messages.dylib");
    void* h2 = dl("deepseek.dylib");
    if (!h1 || !h2) return 1;

    const realugin_plugin_api_t* v1_api = nullptr;
    const realugin_plugin_api_t* ds_api = nullptr;

    // —— v1a：空配置（协议层不设供应商默认，那是"改请求"那一段的事） ——
    g_cfg = {"", "test-key-123", ""};
    realugin_plugin_t* v1a = create_inst(h1, &v1_api);
    realugin_host_t core_handle{&k_fake_core_api, nullptr};
    if (!v1a || !v1_api || v1_api->init(v1a, &core_handle) != REALUGIN_OK) {
        fprintf(stderr, "v1-messages init 失败\n");
        return 1;
    }
    // 模型数据表（ADR-0009）：壳自读自解析，单价取整数便于核对算出来的钱。
    // 单价键与 usage 事件的键同源（input / output / cache_read），壳只做同名键点积
    g_models_path = "test_models.json";
    {
        std::ofstream f(g_models_path);
        f << R"([{"name":"deepseek-v4-flash","owned_by":"deepseek","context":131072,)"
             R"("pricing":{"input":1000,"output":2000,"cache_read":0,"cache_write":0}}])";
    }

    // —— deepseek_default：空配置 → 改请求填 DeepSeek 默认端点/模型 ——
    realugin_plugin_t* ds_default = create_inst(h2, &ds_api);
    if (!ds_default || !ds_api || ds_api->init(ds_default, &core_handle) != REALUGIN_OK) {
        fprintf(stderr, "deepseek init 失败\n");
        return 1;
    }

    // —— v1b：配置了端点（协议层可用配置直连） ——
    g_cfg = {"http://127.0.0.1:18080", "test-key-123", ""};
    realugin_plugin_t* v1b = create_inst(h1, &v1_api);
    if (!v1b || v1_api->init(v1b, &core_handle) != REALUGIN_OK) {
        fprintf(stderr, "v1-messages(配置) init 失败\n");
        return 1;
    }

    // —— deepseek_configured：配置了端点 → 改请求用配置值 ——
    realugin_plugin_t* ds_cfg = create_inst(h2, &ds_api);
    if (!ds_cfg || ds_api->init(ds_cfg, &core_handle) != REALUGIN_OK) {
        fprintf(stderr, "deepseek(配置) init 失败\n");
        return 1;
    }

    test_v1_build_request(v1_api, v1b);
    test_v1_parse_feed(v1_api, v1a);
    // 生成请求的那一段用 v1a（空配置：粗请求里 url 只有路径），改请求补端点——
    // 这正是管线上真实发生的顺序
    test_deepseek_refine(v1_api, v1a, ds_api, ds_default,
                         "https://api.deepseek.com/anthropic/v1/messages");
    test_deepseek_refine(v1_api, v1a, ds_api, ds_cfg, "http://127.0.0.1:18080/v1/messages");
    test_meter(v1_api, v1a, ds_api, ds_default);
    test_deepseek_list_models(ds_api, ds_default);
    test_v1_usage(v1_api, v1a);

    if (v1_api->destroy) { v1_api->destroy(v1b); v1_api->destroy(v1a); }
    if (ds_api->destroy) { ds_api->destroy(ds_cfg); ds_api->destroy(ds_default); }
    dlclose(h1);
    dlclose(h2);

    printf("\n%s: %d 失败\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
