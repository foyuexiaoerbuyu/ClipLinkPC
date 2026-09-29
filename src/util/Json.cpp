#include "Json.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cliplink {
namespace util {

// ---------------------------------------------------------------------------
// UTF 转换
// ---------------------------------------------------------------------------

std::string wideToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return std::string();
    }
    int need = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                   static_cast<int>(wide.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (need <= 0) {
        return std::string();
    }
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        &out[0], need, nullptr, nullptr);
    return out;
}

std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    int need = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                   static_cast<int>(utf8.size()),
                                   nullptr, 0);
    if (need <= 0) {
        return std::wstring();
    }
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        &out[0], need);
    return out;
}

// ---------------------------------------------------------------------------
// 构造 / 工厂
// ---------------------------------------------------------------------------

const JsonValue& jsonNullValue() {
    static const JsonValue nullValue;
    return nullValue;
}

JsonValue::JsonValue() : type_(Type::Null) {}

JsonValue::JsonValue(bool v) : type_(Type::Bool), boolValue_(v) {}

JsonValue::JsonValue(int v)
    : type_(Type::Number), isInt_(true), intValue_(v),
      doubleValue_(static_cast<double>(v)) {}

JsonValue::JsonValue(int64_t v)
    : type_(Type::Number), isInt_(true), intValue_(v),
      doubleValue_(static_cast<double>(v)) {}

JsonValue::JsonValue(double v)
    : type_(Type::Number), isInt_(false), intValue_(static_cast<int64_t>(v)),
      doubleValue_(v) {}

JsonValue::JsonValue(const char* v)
    : type_(Type::String), stringValue_(v ? v : "") {}

JsonValue::JsonValue(const std::string& v)
    : type_(Type::String), stringValue_(v) {}

JsonValue JsonValue::makeArray() {
    JsonValue v;
    v.type_ = Type::Array;
    return v;
}

JsonValue JsonValue::makeObject() {
    JsonValue v;
    v.type_ = Type::Object;
    return v;
}

// ---------------------------------------------------------------------------
// 对象 / 数组操作
// ---------------------------------------------------------------------------

JsonValue& JsonValue::set(const std::string& key, const JsonValue& value) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        object_.clear();
    }
    for (auto& kv : object_) {
        if (kv.first == key) {
            kv.second = value;
            return *this;
        }
    }
    object_.emplace_back(key, value);
    return *this;
}

bool JsonValue::has(const std::string& key) const {
    if (type_ != Type::Object) {
        return false;
    }
    for (const auto& kv : object_) {
        if (kv.first == key) {
            return true;
        }
    }
    return false;
}

const JsonValue& JsonValue::get(const std::string& key) const {
    if (type_ == Type::Object) {
        for (const auto& kv : object_) {
            if (kv.first == key) {
                return kv.second;
            }
        }
    }
    return jsonNullValue();
}

void JsonValue::push(const JsonValue& value) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        array_.clear();
    }
    array_.push_back(value);
}

size_t JsonValue::size() const {
    if (type_ == Type::Array) {
        return array_.size();
    }
    if (type_ == Type::Object) {
        return object_.size();
    }
    return 0;
}

const JsonValue& JsonValue::at(size_t index) const {
    if (type_ == Type::Array && index < array_.size()) {
        return array_[index];
    }
    return jsonNullValue();
}

// ---------------------------------------------------------------------------
// 取值
// ---------------------------------------------------------------------------

std::string JsonValue::asString(const std::string& def) const {
    if (type_ == Type::String) {
        return stringValue_;
    }
    if (type_ == Type::Number) {
        if (isInt_) {
            return std::to_string(intValue_);
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.17g", doubleValue_);
        return std::string(buf);
    }
    if (type_ == Type::Bool) {
        return boolValue_ ? "true" : "false";
    }
    return def;
}

int64_t JsonValue::asInt64(int64_t def) const {
    if (type_ == Type::Number) {
        return isInt_ ? intValue_ : static_cast<int64_t>(doubleValue_);
    }
    if (type_ == Type::Bool) {
        return boolValue_ ? 1 : 0;
    }
    return def;
}

int JsonValue::asInt(int def) const {
    return static_cast<int>(asInt64(def));
}

double JsonValue::asDouble(double def) const {
    if (type_ == Type::Number) {
        return isInt_ ? static_cast<double>(intValue_) : doubleValue_;
    }
    return def;
}

bool JsonValue::asBool(bool def) const {
    if (type_ == Type::Bool) {
        return boolValue_;
    }
    if (type_ == Type::Number) {
        return asInt64(0) != 0;
    }
    return def;
}

// ---------------------------------------------------------------------------
// 序列化
// ---------------------------------------------------------------------------

namespace {

void appendIndent(std::string& out, int indent, int depth) {
    if (indent < 0) {
        return;
    }
    out.push_back('\n');
    out.append(static_cast<size_t>(indent * depth), ' ');
}

void escapeString(const std::string& s, std::string& out) {
    out.push_back('"');
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    // UTF-8 多字节原样透传
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

}  // namespace

void JsonValue::dumpTo(std::string& out, int indent, int depth) const {
    switch (type_) {
        case Type::Null:
            out += "null";
            break;
        case Type::Bool:
            out += boolValue_ ? "true" : "false";
            break;
        case Type::Number:
            if (isInt_) {
                out += std::to_string(intValue_);
            } else {
                if (std::isnan(doubleValue_) || std::isinf(doubleValue_)) {
                    out += "null";
                } else {
                    char buf[40];
                    std::snprintf(buf, sizeof(buf), "%.17g", doubleValue_);
                    out += buf;
                }
            }
            break;
        case Type::String:
            escapeString(stringValue_, out);
            break;
        case Type::Array: {
            if (array_.empty()) {
                out += "[]";
                break;
            }
            out.push_back('[');
            for (size_t i = 0; i < array_.size(); ++i) {
                if (i > 0) {
                    out.push_back(',');
                }
                appendIndent(out, indent, depth + 1);
                array_[i].dumpTo(out, indent, depth + 1);
            }
            appendIndent(out, indent, depth);
            out.push_back(']');
            break;
        }
        case Type::Object: {
            if (object_.empty()) {
                out += "{}";
                break;
            }
            out.push_back('{');
            for (size_t i = 0; i < object_.size(); ++i) {
                if (i > 0) {
                    out.push_back(',');
                }
                appendIndent(out, indent, depth + 1);
                escapeString(object_[i].first, out);
                out.push_back(':');
                if (indent >= 0) {
                    out.push_back(' ');
                }
                object_[i].second.dumpTo(out, indent, depth + 1);
            }
            appendIndent(out, indent, depth);
            out.push_back('}');
            break;
        }
    }
}

std::string JsonValue::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

// ---------------------------------------------------------------------------
// 解析（递归下降）
// ---------------------------------------------------------------------------

struct JsonValue::Parser {
    const char* begin;
    const char* cur;
    const char* end;
    std::string* error;
    int depth = 0;

    void fail(const std::string& msg) {
        if (error != nullptr && error->empty()) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "offset=%td: ",
                          static_cast<std::ptrdiff_t>(cur - begin));
            *error = std::string(buf) + msg;
        }
    }

    void skipWs() {
        while (cur < end) {
            char c = *cur;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++cur;
            } else {
                break;
            }
        }
    }

    bool match(const char* lit) {
        size_t n = std::strlen(lit);
        if (static_cast<size_t>(end - cur) >= n && std::memcmp(cur, lit, n) == 0) {
            cur += n;
            return true;
        }
        return false;
    }

    // 将码点编码为 UTF-8 追加到 out
    static void appendCodePoint(uint32_t cp, std::string& out) {
        if (cp < 0x80u) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800u) {
            out.push_back(static_cast<char>(0xc0u | (cp >> 6)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
        } else if (cp < 0x10000u) {
            out.push_back(static_cast<char>(0xe0u | (cp >> 12)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
        } else {
            out.push_back(static_cast<char>(0xf0u | (cp >> 18)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
        }
    }

    bool parseHex4(uint32_t& out) {
        if (end - cur < 4) {
            fail("truncated \\u escape");
            return false;
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = cur[i];
            uint32_t d;
            if (c >= '0' && c <= '9') {
                d = static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                d = static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                d = static_cast<uint32_t>(c - 'A' + 10);
            } else {
                fail("invalid hex in \\u escape");
                return false;
            }
            out = (out << 4) | d;
        }
        cur += 4;
        return true;
    }

    bool parseStringValue(std::string& out) {
        if (cur >= end || *cur != '"') {
            fail("expected string");
            return false;
        }
        ++cur;
        while (cur < end) {
            char c = *cur;
            if (c == '"') {
                ++cur;
                return true;
            }
            if (c == '\\') {
                ++cur;
                if (cur >= end) {
                    fail("truncated escape");
                    return false;
                }
                char e = *cur++;
                switch (e) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp = 0;
                        if (!parseHex4(cp)) {
                            return false;
                        }
                        // 代理对
                        if (cp >= 0xD800u && cp <= 0xDBFFu) {
                            if (end - cur >= 2 && cur[0] == '\\' && cur[1] == 'u') {
                                cur += 2;
                                uint32_t low = 0;
                                if (!parseHex4(low)) {
                                    return false;
                                }
                                if (low >= 0xDC00u && low <= 0xDFFFu) {
                                    cp = 0x10000u + ((cp - 0xD800u) << 10) +
                                         (low - 0xDC00u);
                                } else {
                                    // 非法低代理：按替换字符处理
                                    cp = 0xFFFDu;
                                }
                            } else {
                                cp = 0xFFFDu;
                            }
                        } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                            cp = 0xFFFDu;
                        }
                        appendCodePoint(cp, out);
                        break;
                    }
                    default:
                        fail("unknown escape");
                        return false;
                }
                continue;
            }
            out.push_back(c);
            ++cur;
        }
        fail("unterminated string");
        return false;
    }

    bool parseNumberValue(JsonValue& out) {
        const char* start = cur;
        if (cur < end && (*cur == '-' || *cur == '+')) {
            ++cur;
        }
        bool isIntegral = true;
        while (cur < end) {
            char c = *cur;
            if (c >= '0' && c <= '9') {
                ++cur;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                if (c == '.' || c == 'e' || c == 'E') {
                    isIntegral = false;
                }
                ++cur;
            } else {
                break;
            }
        }
        if (cur == start) {
            fail("invalid number");
            return false;
        }
        std::string num(start, cur);
        if (isIntegral) {
            errno = 0;
            long long v = std::strtoll(num.c_str(), nullptr, 10);
            if (errno == 0) {
                out = JsonValue(static_cast<int64_t>(v));
                return true;
            }
        }
        out = JsonValue(std::strtod(num.c_str(), nullptr));
        return true;
    }

    bool parseValue(JsonValue& out) {
        if (++depth > 128) {
            fail("max nesting depth exceeded");
            return false;
        }
        struct DepthGuard {
            int& d;
            ~DepthGuard() { --d; }
        } guard{depth};

        skipWs();
        if (cur >= end) {
            fail("unexpected end of input");
            return false;
        }
        char c = *cur;
        if (c == 'n') {
            if (match("null")) {
                out = JsonValue();
                return true;
            }
            fail("invalid literal");
            return false;
        }
        if (c == 't') {
            if (match("true")) {
                out = JsonValue(true);
                return true;
            }
            fail("invalid literal");
            return false;
        }
        if (c == 'f') {
            if (match("false")) {
                out = JsonValue(false);
                return true;
            }
            fail("invalid literal");
            return false;
        }
        if (c == '"') {
            std::string s;
            if (!parseStringValue(s)) {
                return false;
            }
            out = JsonValue(s);
            return true;
        }
        if (c == '[') {
            ++cur;
            out = JsonValue::makeArray();
            skipWs();
            if (cur < end && *cur == ']') {
                ++cur;
                return true;
            }
            while (true) {
                JsonValue item;
                if (!parseValue(item)) {
                    return false;
                }
                out.push(item);
                skipWs();
                if (cur < end && *cur == ',') {
                    ++cur;
                    continue;
                }
                if (cur < end && *cur == ']') {
                    ++cur;
                    return true;
                }
                fail("expected ',' or ']'");
                return false;
            }
        }
        if (c == '{') {
            ++cur;
            out = JsonValue::makeObject();
            skipWs();
            if (cur < end && *cur == '}') {
                ++cur;
                return true;
            }
            while (true) {
                skipWs();
                std::string key;
                if (!parseStringValue(key)) {
                    return false;
                }
                skipWs();
                if (cur >= end || *cur != ':') {
                    fail("expected ':'");
                    return false;
                }
                ++cur;
                JsonValue val;
                if (!parseValue(val)) {
                    return false;
                }
                out.set(key, val);
                skipWs();
                if (cur < end && *cur == ',') {
                    ++cur;
                    continue;
                }
                if (cur < end && *cur == '}') {
                    ++cur;
                    return true;
                }
                fail("expected ',' or '}'");
                return false;
            }
        }
        if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
            return parseNumberValue(out);
        }
        fail("unexpected character");
        return false;
    }
};

bool JsonValue::parse(const std::string& text, JsonValue& out,
                      std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    Parser p;
    p.begin = text.data();
    p.cur   = text.data();
    p.end   = text.data() + text.size();
    p.error = error;

    JsonValue result;
    if (!p.parseValue(result)) {
        if (error != nullptr && error->empty()) {
            *error = "parse failed";
        }
        return false;
    }
    p.skipWs();
    if (p.cur != p.end) {
        if (error != nullptr && error->empty()) {
            *error = "trailing characters after JSON value";
        }
        return false;
    }
    out = result;
    return true;
}

}  // namespace util
}  // namespace cliplink
