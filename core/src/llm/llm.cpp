/*
 * llm.cpp — 协议派发 + SSE 切块 + 计价
 *
 * 这里只放三套协议共有的东西：怎么按空行切 SSE 块、怎么把配置里的协议名解成标签、
 * 怎么把 usage 换成钱。协议自己的知识全在 upstream/ 与 downstream/ 各自的文件里，
 * 本文件一句 if (是不是 anthropic) 都不该有。
 */
#include "llm/llm.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace realagent {
namespace fs = std::filesystem;

/* ==================== 协议身份 ==================== */

std::optional<Protocol> protocol_from(std::string_view name)
{
    if (name == "anthropic-messages") return Protocol::AnthropicMessages;
    if (name == "openai-chat") return Protocol::OpenAiChat;
    if (name == "openai-responses") return Protocol::OpenAiResponses;
    return std::nullopt;
}

std::string_view protocol_name(Protocol p)
{
    switch (p)
    {
        case Protocol::AnthropicMessages:
            return "anthropic-messages";
        case Protocol::OpenAiChat:
            return "openai-chat";
        case Protocol::OpenAiResponses:
            return "openai-responses";
    }
    return "";
}

/* ==================== 端点配置校验（ADR-0017）==================== */

std::string endpoint_config_error(const Config &cfg)
{
    std::vector<std::string> missing;
    if (cfg.get("protocol").empty()) missing.push_back("protocol");
    if (cfg.get("base_url").empty()) missing.push_back("base_url");
    if (cfg.get("model").empty()) missing.push_back("model");

    // 协议名写错与协议名没写是两种错，分开说——"你写的这个我不认识"比
    // "你没写"多一条信息：他确实写了，只是拼错或记岔了
    std::string bad_protocol;
    if (missing.empty() && !protocol_from(cfg.get("protocol")))
        bad_protocol = cfg.get("protocol");

    if (missing.empty() && bad_protocol.empty()) return {};

    std::string out;
    if (!missing.empty())
    {
        out = "配置缺少必填键：";
        for (std::size_t i = 0; i < missing.size(); ++i)
        {
            if (i) out += "、";
            out += missing[i];
        }
        out += "。这三个键没有默认值——填错产生的报错最难诊断，所以宁可现在拦住你。";
    }
    else
    {
        out = "配置里的 protocol=\"" + bad_protocol + "\" 不认识。";
    }
    out += "\nprotocol 只有三个值：anthropic-messages / openai-chat / openai-responses。";
    out += "\n往 ~/.realagent/settings.json 里写（照抄改值即可）：\n";
    out += R"({
  "protocol": "anthropic-messages",
  "base_url": "https://api.deepseek.com/anthropic",
  "model": "deepseek-v4-flash",
  "api_key": "sk-你的密钥"
})";
    return out;
}

std::string http_status_error(long status, const std::string &body)
{
    // 0 = 压根没拿到响应（连不上、被中断掐断在响应头之前）。那是传输层的事，
    // 由 CURLcode 去解释——在这儿说"HTTP 0"是拿一个不存在的状态码糊弄人
    if (status == 0) return {};
    if (status >= 200 && status < 300) return {};
    std::string msg;
    // 各家的错误体形状不同，就近捞一层 message：捞得到就说人话，捞不到就把原文给他，
    // 绝不因为"没读懂错误体"而把一次失败说成成功
    if (const nlohmann::json j = nlohmann::json::parse(body, nullptr, false); j.is_object())
    {
        msg = j.value("/message"_json_pointer, std::string());
        if (msg.empty()) msg = j.value("/error/message"_json_pointer, std::string());
    }
    if (msg.empty()) msg = body.substr(0, 400);
    return "端点返回 HTTP " + std::to_string(status) + (msg.empty() ? "" : "：" + msg);
}

/* ==================== 上行派发 ==================== */

HttpRequest build_request(const Config &cfg, const nlohmann::json &dialog)
{
    // 协议缺失/写错在 Config::missing_required 那一关就拦下了（ADR-0017），
    // 到这里一定解得出来。解不出来只可能是那一关漏了，宁可炸响也不要静默走某个默认
    const auto p = protocol_from(cfg.get("protocol"));
    switch (p.value())
    {
        case Protocol::AnthropicMessages:
            return build_request(protocol::AnthropicMessages{}, cfg, dialog);
        case Protocol::OpenAiChat:
            return build_request(protocol::OpenAiChat{}, cfg, dialog);
        case Protocol::OpenAiResponses:
            return build_request(protocol::OpenAiResponses{}, cfg, dialog);
    }
    return {};
}

/* ==================== 下行：切块 + 派发 ==================== */

SseBlock split_sse_block(std::string_view block)
{
    SseBlock out;
    for (size_t i = 0; i < block.size();)
    {
        const size_t nl = block.find('\n', i);
        std::string_view line =
            nl == std::string_view::npos ? block.substr(i) : block.substr(i, nl - i);
        i = nl == std::string_view::npos ? block.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        const auto after = [&line](size_t n) {
            std::string_view v = line.substr(n);
            if (!v.empty() && v.front() == ' ') v.remove_prefix(1);
            return v;
        };
        if (line.starts_with("event:"))
        {
            out.event = after(6);
        }
        else if (line.starts_with("data:"))
        {
            // 多行 data 按 SSE 规范用 \n 相接（openai-responses 的大载荷会分行）
            if (!out.data.empty()) out.data += '\n';
            out.data += after(5);
        }
    }
    return out;
}

SseParser::SseParser(Protocol p)
{
    switch (p)
    {
        case Protocol::AnthropicMessages:
            st_ = AnthropicState{};
            break;
        case Protocol::OpenAiChat:
            st_ = OpenAiChatState{};
            break;
        case Protocol::OpenAiResponses:
            st_ = OpenAiResponsesState{};
            break;
    }
}

bool SseParser::feed(std::string_view chunk, const EventSink &sink)
{
    buf_.append(chunk);
    // 按空行切事件块。\n\n 与 \r\n\r\n 都可能出现，取先到的那个边界。
    for (;;)
    {
        const size_t lf = buf_.find("\n\n");
        const size_t crlf = buf_.find("\r\n\r\n");
        size_t pos, sep_len;
        if (crlf != std::string::npos && (lf == std::string::npos || crlf < lf))
        {
            pos = crlf;
            sep_len = 4;
        }
        else if (lf != std::string::npos)
        {
            pos = lf;
            sep_len = 2;
        }
        else
        {
            break;
        }
        const std::string block = buf_.substr(0, pos);
        buf_.erase(0, pos + sep_len);

        // 状态的类型就是协议身份——每个 State 自带 tag，这里不必再问一次是谁
        const bool ok = std::visit(
            [&](auto &s) {
                using State = std::decay_t<decltype(s)>;
                return feed_block(typename State::tag{}, s, block, sink);
            },
            st_);
        if (!ok) return false;
    }
    return true;
}

bool SseParser::flush(const EventSink &)
{
    // 剩余缓冲凑不出完整事件块，丢弃即可
    buf_.clear();
    return true;
}

/* 各协议换算完的 usage 都从这儿出门。全零不发（无 usage 信息的端点保持静默） */
void emit_usage(const UsageCounts &u, const EventSink &sink)
{
    if (!sink) return;
    if (u.input == 0 && u.output == 0 && u.cache_read == 0 && u.cache_write == 0) return;
    sink("usage", nlohmann::json{{"input", u.input},
                                 {"output", u.output},
                                 {"cache_read", u.cache_read},
                                 {"cache_write", u.cache_write}});
}

/* ==================== 模型数据表 / 计价 ==================== */

/* 出厂表（ADR-0009）。编译进二进制——它只有几行，为它单开一个安装文件、
 * 再为那个文件单开一条"去哪儿找"的规矩，比表本身长得多。
 * 用户接管版在 ~/.realagent/models.json，存在即整表替换。 */
static constexpr const char *kFactoryModels = R"([
  {"name":"deepseek-v4-flash","owned_by":"deepseek","context":1048576,
   "pricing":{"input":0.14,"output":0.28,"cache_read":0.0028,"cache_write":0}},
  {"name":"deepseek-v4-pro","owned_by":"deepseek","context":1048576,
   "pricing":{"input":0.435,"output":0.87,"cache_read":0.003625,"cache_write":0}}
])";

Pricing Pricing::load(const Config &cfg, std::string *error)
{
    std::string text = kFactoryModels;
    const std::string path = cfg.models_path();
    if (std::error_code ec; fs::exists(path, ec))
    {
        std::ifstream f(path);
        if (!f)
        {
            if (error) *error = "模型数据表打不开: " + path;
            return {};
        }
        text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }

    const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
    if (!parsed.is_array())
    {
        if (error) *error = "模型数据表不是 JSON 数组: " + path;
        return {};
    }
    Pricing out;
    for (const nlohmann::json &m : parsed)
    {
        // 严格：字段缺一即失败，不跳过坏条目、不补默认值。半份表比没有表更难查。
        // 缺键要先用 find 问出来——const operator[] 撞上缺键是未定义行为
        const auto name = m.find("name");
        const auto owned_by = m.find("owned_by");
        const auto context = m.find("context");
        const auto pricing = m.find("pricing");
        if (name == m.end() || !name->is_string() || owned_by == m.end() ||
            !owned_by->is_string() || context == m.end() || !context->is_number_integer() ||
            pricing == m.end() || !pricing->is_object())
        {
            if (error)
                *error = "模型数据表条目缺字段（name/owned_by/context/pricing）: " + m.dump();
            return {};
        }
        out.pricing_[*name] = *pricing;
        out.models_.push_back(nlohmann::json{
            {"name", *name}, {"owned_by", *owned_by}, {"context", *context}});
    }
    return out;
}

double Pricing::cost(const std::string &model, const nlohmann::json &usage) const
{
    const auto it = pricing_.find(model);
    if (it == pricing_.end() || !usage.is_object()) return 0;
    double total = 0;
    // 键名两边同源（都是本表的口径），此处不认识具体是哪些键，也不需要认识
    for (const auto &[k, tokens] : usage.items())
    {
        const auto unit = it->second.find(k);
        if (unit == it->second.end() || !unit->is_number() || !tokens.is_number()) continue;
        total += tokens.get<double>() * unit->get<double>() / 1e6;
    }
    return total;
}

} // namespace realagent
