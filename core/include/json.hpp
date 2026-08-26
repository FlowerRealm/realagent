/*
 * json.hpp — boost::json 封装
 *
 * 设计要点：
 *  - 默认 `{}`：空对象起步，消除未初始化 vs null 的分支
 *  - 链式 operator[]：读缺键返回 null 不抛异常；写自动构造对象/数组
 *  - 宽容 parse：全文解析失败后截取首尾 {} 之间重试（LLM 输出带额外文本）
 *  - optional 提取器：as_string/as_int64 等返回 optional 不抛异常
 *  - struct → JSON：BOOST_DESCRIBE_STRUCT 描述过的类型走 to_json，
 *    字段表由 describe 生成，core 不手写逐字段搬运
 */
#pragma once

#include <boost/describe.hpp>
#include <boost/json.hpp>

#include <cstddef>
#include <cstdint>
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

/* —— struct → JSON（BOOST_DESCRIBE_STRUCT 描述过的类型） ——
 *
 * 只有出站一个方向。入站的 strict_from（严格解构、缺字段一次报全）随 plugin.json
 * 一起删除（ADR-0016）——它唯一的读者是那份人手写的插件清单。 */

/* 字段名与顺序取自 describe 的字段表，与结构体声明逐字一致。 */
template <class T>
json to_json(const T& v) {
    return json(boost::json::value_from(v));
}

} // namespace realagent
