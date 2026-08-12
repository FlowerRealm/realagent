/*
 * json.hpp — boost::json 封装
 *
 * 设计要点：
 *  - 默认 `{}`：空对象起步，消除未初始化 vs null 的分支
 *  - 链式 operator[]：读缺键返回 null 不抛异常；写自动构造对象/数组
 *  - 宽容 parse：全文解析失败后截取首尾 {} 之间重试（LLM 输出带额外文本）
 *  - optional 提取器：as_string/as_int64 等返回 optional 不抛异常
 *  - struct ⇄ JSON：BOOST_DESCRIBE_STRUCT 描述过的类型走 to_json / strict_from，
 *    字段表由 describe 生成，core 不手写逐字段搬运
 */
#pragma once

#include <boost/describe.hpp>
#include <boost/json.hpp>
#include <boost/mp11.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace realagent {

class json : public boost::json::value {
public:
    json() : boost::json::value(boost::json::object{}) {}
    using boost::json::value::value;

    // value -> json 转换（防止 braced init 被 initializer_list 劫持）
    json(const boost::json::value& other) : boost::json::value(other) {}
    json(boost::json::value&& other) : boost::json::value(std::move(other)) {}
    json(std::nullptr_t) : boost::json::value(nullptr) {}
    json(std::string_view s) : boost::json::value(std::string{s}) {}
    json(const std::string& s) : boost::json::value(s) {}
    json(const char* s)
        : boost::json::value(s == nullptr ? boost::json::value(nullptr) : boost::json::value(s)) {}

    static json null() { return json(nullptr); }

    static json array(std::initializer_list<json> items = {}) {
        boost::json::array a;
        a.reserve(items.size());
        for (const auto& item : items) a.push_back(item);
        return json(boost::json::value(std::move(a)));
    }

    // 宽容 parse：先全文解析；失败则截取首尾 {} 之间重试
    static std::optional<json> parse(std::string_view text) {
        boost::system::error_code ec;
        boost::json::value v =
            boost::json::parse(boost::json::string_view{text.data(), text.size()}, ec);
        if (!ec) return json(std::move(v));
        const std::size_t start = text.find('{');
        const std::size_t end = text.rfind('}');
        if (start == std::string_view::npos || end == std::string_view::npos || end <= start)
            return std::nullopt;
        v = boost::json::parse(boost::json::string_view{text.data() + start, end - start + 1}, ec);
        if (ec) return std::nullopt;
        return json(std::move(v));
    }

    // —— 链式访问 ——
    json& operator[](std::string_view key) {
        if (!is_object()) *this = boost::json::object{};
        return static_cast<json&>(as_object()[std::string{key}]);
    }
    json& operator[](std::size_t index) {
        if (!is_array()) *this = boost::json::array{};
        auto& a = as_array();
        if (a.size() <= index) a.resize(index + 1);
        return static_cast<json&>(a[index]);
    }
    json operator[](std::string_view key) const {
        if (!is_object()) return null();
        const auto* v = as_object().if_contains(key);
        return v == nullptr ? null() : json(*v);
    }
    json operator[](std::size_t index) const {
        if (!is_array() || index >= as_array().size()) return null();
        return json(as_array()[index]);
    }

    bool contains(std::string_view key) const {
        return is_object() && as_object().if_contains(key) != nullptr;
    }
    std::size_t size() const {
        if (is_object()) return as_object().size();
        if (is_array()) return as_array().size();
        if (is_string()) return boost::json::value::as_string().size();
        return 0;
    }
    void erase(std::string_view key) {
        if (is_object()) as_object().erase(key);
    }
    void push_back(json v) {
        if (!is_array()) *this = boost::json::array{};
        as_array().push_back(std::move(v));
    }
    std::vector<std::string> keys() const {
        std::vector<std::string> out;
        if (!is_object()) return out;
        out.reserve(as_object().size());
        for (const auto& f : as_object()) out.emplace_back(f.key());
        return out;
    }
    std::string dump() const {
        return boost::json::serialize(static_cast<const boost::json::value&>(*this));
    }

    // —— optional 提取器（不抛异常） ——
    std::optional<long long> as_int64() const {
        if (is_int64()) return boost::json::value::as_int64();
        if (is_uint64()) {
            const auto u = boost::json::value::as_uint64();
            if (u > static_cast<std::uint64_t>(std::numeric_limits<long long>::max()))
                return std::nullopt;
            return static_cast<long long>(u);
        }
        return std::nullopt;
    }
    std::optional<std::string> as_string() const {
        if (!is_string()) return std::nullopt;
        const auto& s = boost::json::value::as_string();
        return std::string{s.data(), s.size()};
    }
    std::optional<bool> as_bool() const {
        if (!is_bool()) return std::nullopt;
        return boost::json::value::as_bool();
    }
    std::optional<double> as_double() const {
        if (is_double()) return boost::json::value::as_double();
        if (const auto i = as_int64()) return static_cast<double>(*i);
        return std::nullopt;
    }
};

inline std::string serialize(const json& v) { return v.dump(); }

/* —— struct ⇄ JSON（BOOST_DESCRIBE_STRUCT 描述过的类型） —— */

/* 出站：struct → JSON。字段名与顺序取自 describe 的字段表，与结构体声明逐字一致。 */
template <class T>
json to_json(const T& v) {
    return json(boost::json::value_from(v));
}

/* 入站：JSON → struct，严格解构——描述的字段缺一即失败，绝不静默补默认值。
 * 空串/0 混进结构体会一路传下去，等发现时早已离案发现场十万八千里。
 *
 * 用于人手写的数据（plugin.json 一类：手改、装一半、打错字都是常态）。
 * 插件在运行时吐出的事件不走这里——那是插件自己的契约，core 不替它把关。
 *
 * 失败以返回值给出（try_value_to 不抛）：调用方要拿错误原文报给用户，
 * 而不是让异常穿过一路 C ABI 边界（ADR-0001）。
 *
 * boost 原生错误只有一句 "source composite size does not match target size"，
 * 不说缺谁。字段表既然是现成的，就自己比一遍、一次报全（对齐 Config::load）。 */
template <class T>
std::expected<T, std::string> strict_from(const json& v, std::string_view what) {
    if (auto r = boost::json::try_value_to<T>(static_cast<const boost::json::value&>(v)))
        return std::move(*r);
    std::vector<std::string> missing;
    if (v.is_object()) {
        using members = boost::describe::describe_members<T, boost::describe::mod_public>;
        boost::mp11::mp_for_each<members>([&](auto D) {
            if (!v.as_object().if_contains(D.name)) missing.emplace_back(D.name);
        });
    }
    std::string msg(what);
    if (missing.empty()) return std::unexpected(msg + ": 字段类型不符 — " + v.dump());
    msg += ": 缺必需键 [";
    for (std::size_t i = 0; i < missing.size(); ++i) {
        if (i != 0) msg += ", ";
        msg += missing[i];
    }
    return std::unexpected(msg + "]");
}

} // namespace realagent
