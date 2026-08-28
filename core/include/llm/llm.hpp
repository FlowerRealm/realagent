/*
 * llm.hpp — 一次 LLM 调用的三件事：造请求、解响应、算钱
 *
 * 三套协议，两个方向（ADR-0017）：
 *
 *                    upstream/                     downstream/
 *   对话 ──build_request──▶ HttpRequest ─[curl]─▶ 响应流 ──feed_block──▶ 事件
 *                                                                        │
 *                                        Pricing::cost(model, usage) ◀────┘
 *
 * 上行三个文件、下行三个文件，一个协议在一个方向上的全部知识住在一个文件里：
 * 认证头、URL 路径、请求体形状、帧结构、token 字段名——这些是共变的，
 * 拆成独立开关就能配出无效组合（Bearer 头配 OpenAI 体配 /v1/messages 路径）。
 * 绑成一束，无效组合直接不可表达。
 *
 * 协议由用户选，没有默认值、也不从 base_url 猜（ADR-0017）：
 * 猜错产生的报错（端点 404、体形状不认、解析出空）恰是最难自己诊断的一类。
 *
 * 事件词汇表只有一套：thinking_start / thinking_update / thinking_stop /
 * message_update / tool_use / usage / stop。协议是三套，上层不该跟着分三份。
 *
 * 计价（ADR-0009）：token 用量 × 模型单价 = 钱。usage 事件本身不上传——
 * 上层不认识 token，只认识钱。算不出返回 0（不发 cost，不发 0）。
 * 各家 token 字段名不同（prompt_tokens / input_tokens），换算在各自的下行文件里，
 * 出了那个文件一律叫 input / output / cache_read / cache_write。
 */
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "config.hpp"
#include "json.hpp"

namespace realagent {

/* —— 协议标签：重载的判别参数，不是运行期数据 —— */
namespace protocol {
struct AnthropicMessages {};
struct OpenAiChat {};
struct OpenAiResponses {};
} // namespace protocol

/* 运行期的协议身份（配置里那个字符串解出来的） */
enum class Protocol { AnthropicMessages, OpenAiChat, OpenAiResponses };

/* 配置字符串 → 协议。认不出返回 nullopt（调用方报错，不猜、不兜底） */
std::optional<Protocol> protocol_from(std::string_view name);
/* 协议 → 配置字符串（报错信息里列可选值用） */
std::string_view protocol_name(Protocol p);

/* 端点那一束（protocol / base_url / model）配齐了没有。配齐返回空串，
 * 否则返回一段人话：缺哪个说哪个、一次说全、附一段能直接抄的 settings.json。
 *
 * 为什么这个检查住在 llm 而不是 config：这三个键是"打一次 LLM 调用需要什么"，
 * 那是本模块的知识。config 只管键的读写，不该认识 anthropic-messages 是什么。 */
std::string endpoint_config_error(const Config& cfg);

/* HTTP 状态码不是 2xx 时的人话错误（body 里的 message 能捞就捞出来）。
 * 三套协议共用一条规矩：先看状态码，再谈解析——4xx/5xx 的响应体不是流，
 * 拿去喂解析器只会解出一个"成功但空"的回答（ADR-0017）。 */
std::string http_status_error(long status, const std::string& body);

/* 一个 HTTP 请求：给 libcurl 用的三样东西，不多不少。
 * 不摆成 JSON 再解回来——中间那一趟序列化没有第二个读者。 */
struct HttpRequest {
    std::string url;
    std::vector<std::string> headers; // "Key: value"，直接进 curl_slist
    std::string body;                 // JSON 文本
};

/* —— 上行：抽象对话 {model, system, messages, tools} → 具体协议的请求 ——
 * 端点取 cfg.base_url，凭证取 cfg.api_key，模型名由调用方放进 dialog["model"]。
 * 认证头归各协议自己：Anthropic 原厂认 x-api-key，OpenAI 系认 Authorization: Bearer。 */
HttpRequest build_request(protocol::AnthropicMessages, const Config& cfg, const nlohmann::json& dialog);
HttpRequest build_request(protocol::OpenAiChat, const Config& cfg, const nlohmann::json& dialog);
HttpRequest build_request(protocol::OpenAiResponses, const Config& cfg, const nlohmann::json& dialog);
/* 按配置里的协议派发（认不出的协议由 Config 那一关拦下，到不了这里） */
HttpRequest build_request(const Config& cfg, const nlohmann::json& dialog);

/* 解析出的事件同步交给它（payload 只在回调内有效） */
using EventSink = std::function<void(std::string_view type, const nlohmann::json& payload)>;

/* —— 下行：每个协议一份解析状态 ——
 * 一次 LLM 调用一份，用完就扔：上一轮的半截缓冲绝不该漏进下一轮。
 * usage 计数是绝对值（后到覆盖先到），丢帧不造成永久偏差。 */

struct UsageCounts {
    long long input = 0;
    long long output = 0;
    long long cache_read = 0;
    long long cache_write = 0;
};

struct AnthropicState {
    using tag = protocol::AnthropicMessages;
    std::string block_type_; // 当前 content block 类型（text / thinking / tool_use）
    std::string tool_id_;
    std::string tool_name_;
    std::string tool_input_; // 累积 partial_json
    std::string thinking_sig_;
    UsageCounts usage;
};

struct OpenAiChatState {
    using tag = protocol::OpenAiChat;
    /* tool_calls 按下标增量到达，index → {id, name, 累积 arguments} */
    struct PendingTool {
        std::string id;
        std::string name;
        std::string args;
    };
    std::unordered_map<long long, PendingTool> tools;
    std::vector<long long> tool_order; // 按首次出现排，回传顺序与模型给的一致
    bool reasoning_open = false;       // 已发过 thinking_start
    std::string finish_reason;
    UsageCounts usage;
};

struct OpenAiResponsesState {
    using tag = protocol::OpenAiResponses;
    /* Responses 的事件类型在 SSE 的 event: 行上，data 里是强类型载荷 */
    std::string tool_id_;
    std::string tool_name_;
    std::string tool_input_;
    bool reasoning_open = false;
    std::string finish_reason;
    UsageCounts usage;
};

/* 一个 SSE 事件块拆出来的两样东西。三套协议都用得上：
 * anthropic / openai_chat 的类型在 data 的字段里，openai_responses 的在 event: 行上。
 * 多行 data 按 SSE 规范用 \n 相接。 */
struct SseBlock {
    std::string event;
    std::string data;
};
SseBlock split_sse_block(std::string_view block);

/* 喂一个 SSE 事件块（已按空行切好，含 event: 与 data: 各行）。
 * false = 帧不合规，本次调用应当中止。
 *
 * 畸形帧一律返回 false，绝不静默跳过：跳过 = 正文悄悄消失，上层收到一个
 * "成功但空"的回答，用户看不出发生了什么，那比报错更糟。 */
bool feed_block(protocol::AnthropicMessages, AnthropicState&, std::string_view block,
                const EventSink&);
bool feed_block(protocol::OpenAiChat, OpenAiChatState&, std::string_view block, const EventSink&);
bool feed_block(protocol::OpenAiResponses, OpenAiResponsesState&, std::string_view block,
                const EventSink&);

/* SSE 解析器：按空行切块是三套协议共有的（都是 text/event-stream），
 * 切完交给协议自己的 feed_block。有状态，一次调用一个实例。 */
class SseParser {
public:
    explicit SseParser(Protocol p);

    /* 喂一段响应体。false = 帧不合规，本次调用应当中止 */
    bool feed(std::string_view chunk, const EventSink& sink);
    /* 流结束：剩余缓冲不再有完整事件，此处只为对称保留 */
    bool flush(const EventSink& sink);

private:
    std::string buf_; // 未凑齐一个事件块的残料
    std::variant<AnthropicState, OpenAiChatState, OpenAiResponsesState> st_;
};

/* 下行文件共用的小工具（各协议的 usage 字段名不同，换算完都走这里） */
void emit_usage(const UsageCounts& u, const EventSink& sink);

/* 模型数据表（ADR-0009）：单价 + 公开元数据。协议无关——钱就是钱。
 *
 * 两个来源，不合并：~/.realagent/models.json 存在就是它，否则用编译进来的出厂表。
 * 用户想改一个模型的单价，就得连表一起接管——半份表比没有表更难查。
 */
class Pricing {
public:
    /* 读表。文件在但读不动/字段缺 → 报错（error 非空），不跳过坏条目、不补默认值 */
    static Pricing load(const Config& cfg, std::string* error = nullptr);

    /* token 用量 × 该模型单价，同名键点积 / 1M。表里没这个模型/没这个键 → 该维度不计。
     * 算不出返回 0。 */
    double cost(const std::string& model, const nlohmann::json& usage) const;

    /* 公开清单 [{name, owned_by, context}]——单价不在里面，那是本表自己的事 */
    const nlohmann::json& models() const { return models_; }

private:
    std::unordered_map<std::string, nlohmann::json> pricing_; // 模型名 → 单价对象
    nlohmann::json models_ = nlohmann::json::array();
};

} // namespace realagent
