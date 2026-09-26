#ifndef SMARTGRID_JSON_HPP
#define SMARTGRID_JSON_HPP

// A deliberately small JSON value type used by the native Smart Grid
// adapter.  It has no dependencies beyond the C++17 standard library and is
// intended for the application's bounded API payloads rather than as a
// general purpose JSON implementation.

#include <cstddef>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace sg {

class Json {
public:
    using array_type = std::vector<Json>;
    using object_type = std::map<std::string, Json>;

    Json() noexcept;
    explicit Json(std::nullptr_t) noexcept;
    explicit Json(bool value) noexcept;
    explicit Json(int value) noexcept;
    explicit Json(long value) noexcept;
    explicit Json(long long value) noexcept;
    explicit Json(std::size_t value) noexcept;
    explicit Json(double value);
    explicit Json(const std::string& value);
    explicit Json(std::string&& value) noexcept;
    explicit Json(const char* value);
    explicit Json(const array_type& value);
    explicit Json(array_type&& value) noexcept;
    explicit Json(const object_type& value);
    explicit Json(object_type&& value) noexcept;

    static Json object();
    static Json array();
    static Json parse(const std::string& text);

    std::string dump(int indent = -1) const;

    Json& operator[](const std::string& key);
    const Json& operator[](const std::string& key) const;
    Json& operator[](std::size_t index);
    const Json& operator[](std::size_t index) const;

    bool contains(const std::string& key) const noexcept;
    std::size_t size() const noexcept;
    void push_back(const Json& value);
    void push_back(Json&& value);

    bool is_null() const noexcept;
    bool is_boolean() const noexcept;
    bool is_number() const noexcept;
    bool is_string() const noexcept;
    bool is_array() const noexcept;
    bool is_object() const noexcept;

    double number(double default_value = 0.0) const noexcept;
    long long integer(long long default_value = 0) const noexcept;
    std::string string(const std::string& default_value = {}) const;
    bool boolean(bool default_value = false) const noexcept;

    const object_type& items() const;
    object_type& items();
    const array_type& elements() const;
    array_type& elements();

private:
    using value_type = std::variant<std::nullptr_t, bool, double, std::string,
                                    array_type, object_type>;
    value_type value_;

    explicit Json(value_type value) noexcept;
};

namespace json_detail {

inline void append_utf8(std::string& out, unsigned codepoint) {
    if (codepoint <= 0x7FU) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFU) {
        out.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
        out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else if (codepoint <= 0xFFFFU) {
        out.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    }
}

inline void append_escaped(std::string& out, const std::string& value) {
    static constexpr char hex[] = "0123456789abcdef";
    out.push_back('"');
    for (unsigned char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20U) {
                out += "\\u00";
                out.push_back(hex[(c >> 4U) & 0x0FU]);
                out.push_back(hex[c & 0x0FU]);
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    out.push_back('"');
}

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    Json parse_document() {
        skip_space();
        Json result = parse_value();
        skip_space();
        if (pos_ != text_.size()) {
            fail("unexpected trailing characters");
        }
        return result;
    }

private:
    const std::string& text_;
    std::size_t pos_ = 0;

    [[noreturn]] void fail(const std::string& message) const {
        throw std::runtime_error("invalid JSON at byte " + std::to_string(pos_) + ": " + message);
    }

    void skip_space() {
        while (pos_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[pos_]);
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++pos_;
        }
    }

    bool consume(char expected) {
        if (pos_ < text_.size() && text_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    void require(char expected) {
        if (!consume(expected)) {
            fail(std::string("expected '") + expected + "'");
        }
    }

    Json parse_value() {
        skip_space();
        if (pos_ >= text_.size()) {
            fail("expected a value");
        }
        switch (text_[pos_]) {
        case 'n': return parse_literal("null", Json(nullptr));
        case 't': return parse_literal("true", Json(true));
        case 'f': return parse_literal("false", Json(false));
        case '"': return Json(parse_string());
        case '[': return parse_array();
        case '{': return parse_object();
        default:
            if (text_[pos_] == '-' || (text_[pos_] >= '0' && text_[pos_] <= '9')) {
                return parse_number();
            }
            fail("unexpected value");
        }
    }

    Json parse_literal(const char* literal, Json value) {
        const std::size_t length = std::char_traits<char>::length(literal);
        if (text_.compare(pos_, length, literal) != 0) {
            fail(std::string("expected ") + literal);
        }
        pos_ += length;
        return value;
    }

    unsigned parse_hex4() {
        if (text_.size() - pos_ < 4) {
            fail("truncated unicode escape");
        }
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            value <<= 4U;
            if (c >= '0' && c <= '9') value |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned>(c - 'A' + 10);
            else fail("invalid unicode escape");
        }
        return value;
    }

    std::string parse_string() {
        require('"');
        std::string result;
        while (pos_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') {
                return result;
            }
            if (c < 0x20U) {
                fail("control character in string");
            }
            if (c != '\\') {
                result.push_back(static_cast<char>(c));
                continue;
            }
            if (pos_ >= text_.size()) {
                fail("truncated escape");
            }
            switch (text_[pos_++]) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                unsigned codepoint = parse_hex4();
                if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
                    if (text_.size() - pos_ < 6 || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
                        fail("high surrogate without low surrogate");
                    }
                    pos_ += 2;
                    const unsigned low = parse_hex4();
                    if (low < 0xDC00U || low > 0xDFFFU) {
                        fail("invalid low surrogate");
                    }
                    codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) + (low - 0xDC00U);
                } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
                    fail("unpaired low surrogate");
                }
                append_utf8(result, codepoint);
                break;
            }
            default: fail("unknown string escape");
            }
        }
        fail("unterminated string");
    }

    Json parse_number() {
        const std::size_t start = pos_;
        consume('-');
        if (pos_ >= text_.size()) fail("incomplete number");
        if (text_[pos_] == '0') {
            ++pos_;
            if (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                fail("leading zero in number");
            }
        } else {
            if (text_[pos_] < '1' || text_[pos_] > '9') fail("invalid number");
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        if (consume('.')) {
            const std::size_t fraction_start = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            if (fraction_start == pos_) fail("digits required after decimal point");
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            consume('+');
            consume('-');
            const std::size_t exponent_start = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            if (exponent_start == pos_) fail("digits required in exponent");
        }
        const std::string token = text_.substr(start, pos_ - start);
        char* end = nullptr;
        const double number = std::strtod(token.c_str(), &end);
        if (end == token.c_str() || *end != '\0' || !std::isfinite(number)) {
            fail("number is outside the supported finite range");
        }
        return Json(number);
    }

    Json parse_array() {
        require('[');
        Json result = Json::array();
        skip_space();
        if (consume(']')) return result;
        while (true) {
            result.push_back(parse_value());
            skip_space();
            if (consume(']')) return result;
            require(',');
            skip_space();
        }
    }

    Json parse_object() {
        require('{');
        Json result = Json::object();
        skip_space();
        if (consume('}')) return result;
        while (true) {
            skip_space();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                fail("object key must be a string");
            }
            const std::string key = parse_string();
            skip_space();
            require(':');
            result[key] = parse_value();
            skip_space();
            if (consume('}')) return result;
            require(',');
            skip_space();
        }
    }
};

inline void append_indent(std::string& out, int depth, int indent) {
    if (indent > 0) out.append(static_cast<std::size_t>(depth * indent), ' ');
}

} // namespace json_detail

inline Json::Json() noexcept : value_(nullptr) {}

inline Json::Json(std::nullptr_t) noexcept : value_(nullptr) {}

inline Json::Json(bool value) noexcept : value_(value) {}

inline Json::Json(int value) noexcept : value_(static_cast<double>(value)) {}

inline Json::Json(long value) noexcept : value_(static_cast<double>(value)) {}

inline Json::Json(long long value) noexcept : value_(static_cast<double>(value)) {}

inline Json::Json(std::size_t value) noexcept : value_(static_cast<double>(value)) {}

inline Json::Json(double value) : value_(value) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument("JSON numbers must be finite");
    }
}

inline Json::Json(const std::string& value) : value_(value) {}

inline Json::Json(std::string&& value) noexcept : value_(std::move(value)) {}

inline Json::Json(const char* value) {
    if (value == nullptr) {
        throw std::invalid_argument("JSON string cannot be null");
    }
    value_ = std::string(value);
}

inline Json::Json(const array_type& value) : value_(value) {}

inline Json::Json(array_type&& value) noexcept : value_(std::move(value)) {}

inline Json::Json(const object_type& value) : value_(value) {}

inline Json::Json(object_type&& value) noexcept : value_(std::move(value)) {}

inline Json::Json(value_type value) noexcept : value_(std::move(value)) {}

inline Json Json::object() { return Json(object_type{}); }

inline Json Json::array() { return Json(array_type{}); }

inline Json Json::parse(const std::string& text) {
    return json_detail::Parser(text).parse_document();
}

inline std::string Json::dump(int indent) const {
    std::string out;
    const int pretty_indent = indent < 0 ? -1 : indent;
    std::function<void(const Json&, int)> emit = [&](const Json& node, int depth) {
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) {
                out += "null";
            } else if constexpr (std::is_same_v<T, bool>) {
                out += value ? "true" : "false";
            } else if constexpr (std::is_same_v<T, double>) {
                if (!std::isfinite(value)) throw std::runtime_error("cannot serialize non-finite JSON number");
                std::ostringstream stream;
                stream.imbue(std::locale::classic());
                stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
                out += stream.str();
            } else if constexpr (std::is_same_v<T, std::string>) {
                json_detail::append_escaped(out, value);
            } else if constexpr (std::is_same_v<T, array_type>) {
                if (value.empty()) {
                    out += "[]";
                    return;
                }
                out.push_back('[');
                if (pretty_indent >= 0) out.push_back('\n');
                for (std::size_t i = 0; i < value.size(); ++i) {
                    if (pretty_indent >= 0) json_detail::append_indent(out, depth + 1, pretty_indent);
                    emit(value[i], depth + 1);
                    if (i + 1 < value.size()) out.push_back(',');
                    if (pretty_indent >= 0) out.push_back('\n');
                }
                if (pretty_indent >= 0) json_detail::append_indent(out, depth, pretty_indent);
                out.push_back(']');
            } else if constexpr (std::is_same_v<T, object_type>) {
                if (value.empty()) {
                    out += "{}";
                    return;
                }
                out.push_back('{');
                if (pretty_indent >= 0) out.push_back('\n');
                std::size_t i = 0;
                for (const auto& entry : value) {
                    if (pretty_indent >= 0) json_detail::append_indent(out, depth + 1, pretty_indent);
                    json_detail::append_escaped(out, entry.first);
                    out.push_back(':');
                    if (pretty_indent >= 0) out.push_back(' ');
                    emit(entry.second, depth + 1);
                    if (++i < value.size()) out.push_back(',');
                    if (pretty_indent >= 0) out.push_back('\n');
                }
                if (pretty_indent >= 0) json_detail::append_indent(out, depth, pretty_indent);
                out.push_back('}');
            }
        }, node.value_);
    };
    emit(*this, 0);
    return out;
}

inline Json& Json::operator[](const std::string& key) {
    if (std::holds_alternative<std::nullptr_t>(value_)) value_ = object_type{};
    if (!std::holds_alternative<object_type>(value_)) {
        throw std::runtime_error("JSON value is not an object");
    }
    return std::get<object_type>(value_)[key];
}

inline const Json& Json::operator[](const std::string& key) const {
    static const Json null_value;
    const auto* object = std::get_if<object_type>(&value_);
    if (object == nullptr) return null_value;
    const auto it = object->find(key);
    return it == object->end() ? null_value : it->second;
}

inline Json& Json::operator[](std::size_t index) {
    if (std::holds_alternative<std::nullptr_t>(value_)) value_ = array_type{};
    if (!std::holds_alternative<array_type>(value_)) {
        throw std::runtime_error("JSON value is not an array");
    }
    auto& array = std::get<array_type>(value_);
    if (index >= array.size()) array.resize(index + 1);
    return array[index];
}

inline const Json& Json::operator[](std::size_t index) const {
    static const Json null_value;
    const auto* array = std::get_if<array_type>(&value_);
    if (array == nullptr || index >= array->size()) return null_value;
    return (*array)[index];
}

inline bool Json::contains(const std::string& key) const noexcept {
    const auto* object = std::get_if<object_type>(&value_);
    return object != nullptr && object->find(key) != object->end();
}

inline std::size_t Json::size() const noexcept {
    if (const auto* object = std::get_if<object_type>(&value_)) return object->size();
    if (const auto* array = std::get_if<array_type>(&value_)) return array->size();
    return 0;
}

inline void Json::push_back(const Json& value) {
    if (std::holds_alternative<std::nullptr_t>(value_)) value_ = array_type{};
    if (!std::holds_alternative<array_type>(value_)) throw std::runtime_error("JSON value is not an array");
    std::get<array_type>(value_).push_back(value);
}

inline void Json::push_back(Json&& value) {
    if (std::holds_alternative<std::nullptr_t>(value_)) value_ = array_type{};
    if (!std::holds_alternative<array_type>(value_)) throw std::runtime_error("JSON value is not an array");
    std::get<array_type>(value_).push_back(std::move(value));
}

inline bool Json::is_null() const noexcept { return std::holds_alternative<std::nullptr_t>(value_); }

inline bool Json::is_boolean() const noexcept { return std::holds_alternative<bool>(value_); }

inline bool Json::is_number() const noexcept { return std::holds_alternative<double>(value_); }

inline bool Json::is_string() const noexcept { return std::holds_alternative<std::string>(value_); }

inline bool Json::is_array() const noexcept { return std::holds_alternative<array_type>(value_); }

inline bool Json::is_object() const noexcept { return std::holds_alternative<object_type>(value_); }

inline double Json::number(double default_value) const noexcept {
    const auto* number = std::get_if<double>(&value_);
    return number == nullptr ? default_value : *number;
}

inline long long Json::integer(long long default_value) const noexcept {
    const auto* number = std::get_if<double>(&value_);
    if (number == nullptr || !std::isfinite(*number) ||
        *number < static_cast<double>(std::numeric_limits<long long>::min()) ||
        *number >= static_cast<double>(std::numeric_limits<long long>::max())) {
        return default_value;
    }
    return static_cast<long long>(*number);
}

inline std::string Json::string(const std::string& default_value) const {
    const auto* value = std::get_if<std::string>(&value_);
    return value == nullptr ? default_value : *value;
}

inline bool Json::boolean(bool default_value) const noexcept {
    const auto* value = std::get_if<bool>(&value_);
    return value == nullptr ? default_value : *value;
}

inline const Json::object_type& Json::items() const {
    static const object_type empty;
    const auto* object = std::get_if<object_type>(&value_);
    return object == nullptr ? empty : *object;
}

inline Json::object_type& Json::items() {
    if (!std::holds_alternative<object_type>(value_)) value_ = object_type{};
    return std::get<object_type>(value_);
}

inline const Json::array_type& Json::elements() const {
    static const array_type empty;
    const auto* array = std::get_if<array_type>(&value_);
    return array == nullptr ? empty : *array;
}

inline Json::array_type& Json::elements() {
    if (!std::holds_alternative<array_type>(value_)) value_ = array_type{};
    return std::get<array_type>(value_);
}

} // namespace sg

#endif // SMARTGRID_JSON_HPP
