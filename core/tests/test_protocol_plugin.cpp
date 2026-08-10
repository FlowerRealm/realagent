/*
 * test_protocol_plugin.cpp — deepseek-messages 协议插件单元测试
 *
 * 不依赖真实 API：dlopen 插件后直接调 build_request / parse_feed，
 * 验证抽象对话 → Anthropic 请求体、SSE → 事件 的转换正确性。
 */
#include <dlfcn.h>

#include <cstdio>
#include <cstring>
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

/* —— fake core（仅 get_config 被用到） —— */
static const char* fake_get_config(plugin_core_t*, const char* key) {
    static const std::string api_key = "test-key-123";
    static const std::string base_url = "https://api.deepseek.com/anthropic";
    static const std::string model = "deepseek-v4-flash";
    if (std::strcmp(key, "api_key") == 0) return api_key.c_str();
    if (std::strcmp(key, "base_url") == 0) return base_url.c_str();
    if (std::strcmp(key, "model") == 0) return model.c_str();
    return "";
}
static plugin_status_t fake_register_tool(plugin_core_t*, const plugin_tool_t*) { return PLUGIN_OK; }
static plugin_status_t fake_register_command(plugin_core_t*, const plugin_command_t*) { return PLUGIN_OK; }
static plugin_status_t fake_emit(plugin_core_t*, const char*, const char*) { return PLUGIN_OK; }
static void fake_log(plugin_core_t*, int, const char*) {}
static plugin_status_t fake_depends(plugin_core_t*, const char*) { return PLUGIN_OK; }

static const plugin_core_api_t k_fake_core_api = {
    .register_tool = fake_register_tool,
    .register_command = fake_register_command,
    .emit = fake_emit,
    .log = fake_log,
    .get_config = fake_get_config,
    .depends_on = fake_depends,
};

/* 测试 1：build_request 转换 */
static void test_build_request(plugin_t* inst, const plugin_api_t* api) {
    printf("[build_request]\n");
    const char* dialog = R"({
        "model": "deepseek-v4-flash",
        "system": "You are a coding agent.",
        "messages": [
            {"role":"user","content":[{"type":"text","text":"hello"}]}
        ],
        "tools": [
            {"name":"read","description":"读文件","input_schema":{"type":"object","properties":{"file_path":{"type":"string"}}}}
        ]
    })";
    plugin_request_t req{};
    CHECK(api->build_request(inst, dialog, &req) == PLUGIN_OK, "build_request 返回 OK");
    if (req.url) {
        CHECK(std::string(req.url).find("/v1/messages") != std::string::npos, "url 含 /v1/messages");
        CHECK(std::string(req.url).find("https://api.deepseek.com") != std::string::npos, "url 用配置 base_url");
    }
    if (req.headers) {
        CHECK(std::string(req.headers).find("Bearer test-key-123") != std::string::npos, "headers 含 Bearer api_key");
        CHECK(std::string(req.headers).find("Content-Type") != std::string::npos, "headers 含 Content-Type");
    }
    if (req.body) {
        boost::system::error_code ec;
        const bj::value body = bj::parse(req.body, ec);
        CHECK(!ec, "body 是合法 JSON");
        if (!ec) {
            const auto& o = body.as_object();
            CHECK(o.at("stream").as_bool() == true, "stream=true");
            CHECK(bj::value_to<std::string>(o.at("model")) == "deepseek-v4-flash", "model 正确");
            CHECK(o.at("messages").as_array().at(0).as_object().at("role").as_string() == "user",
                  "messages[0].role=user");
            CHECK(o.at("tools").as_array().at(0).as_object().at("name").as_string() == "read",
                  "tools[0].name=read");
        }
    }
    if (api->free) {
        api->free(inst, const_cast<char*>(req.url));
        api->free(inst, const_cast<char*>(req.headers));
        api->free(inst, const_cast<char*>(req.body));
    }
}

/* 测试 2：parse_feed SSE 解析 */
static void test_parse_feed(plugin_t* inst, const plugin_api_t* api) {
    printf("[parse_feed]\n");
    struct Sink {
        std::vector<std::string> updates;
        std::vector<std::string> tools;
        std::string stop_reason;
    } sink;
    const auto cb = [](void* ctx, const char* type, const char* payload) {
        auto* s = static_cast<Sink*>(ctx);
        const std::string t(type ? type : "");
        if (t == "message_update") s->updates.emplace_back(payload);
        else if (t == "tool_use") s->tools.emplace_back(payload);
        else if (t == "stop") s->stop_reason = payload;
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
            CHECK(bj::value_to<std::string>(o.at("id")) == "t1", "tool_use.id=t1");
            CHECK(bj::value_to<std::string>(o.at("input").as_object().at("file_path")) == "a.txt",
                  "tool_use.input.file_path=a.txt（partial_json 累积）");
        }
    }
    CHECK(sink.stop_reason.find("tool_use") != std::string::npos, "stop reason=tool_use");
}

int main() {
    void* h = dlopen("deepseek-messages.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("build/deepseek-messages.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "dlopen 失败: %s\n", dlerror());
        return 1;
    }
    auto create_fn = (plugin_create_fn)dlsym(h, PLUGIN_CREATE_SYM);
    if (!create_fn) { fprintf(stderr, "缺 %s\n", PLUGIN_CREATE_SYM); return 1; }
    const plugin_api_t* api = nullptr;
    plugin_t* inst = create_fn(&api);
    if (!inst || !api) { fprintf(stderr, "plugin_create 失败\n"); return 1; }

    // 构造 fake core 句柄并 init
    plugin_core_t core_handle{&k_fake_core_api, nullptr};
    if (api->init(inst, &core_handle) != PLUGIN_OK) {
        fprintf(stderr, "init 失败\n");
        return 1;
    }

    test_build_request(inst, api);
    test_parse_feed(inst, api);

    if (api->destroy) api->destroy(inst);
    dlclose(h);

    printf("\n%s: %d 失败\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
