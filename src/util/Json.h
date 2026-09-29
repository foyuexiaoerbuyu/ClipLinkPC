#pragma once

// ---------------------------------------------------------------------------
// 轻量 JSON：构建 + 解析 + UTF-8 / UTF-16 转换
// - 序列化保持键顺序（配置文件字段顺序稳定）
// - int64 精确表示（serverSeq / 毫秒时间戳不丢精度）
// - 宽字符转换供 Windows API（UTF-16）与协议（UTF-8）互转
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cliplink {
namespace util {

// ---- UTF 转换（CP_UTF8 <-> UTF-16）-------------------------------------
std::string wideToUtf8(const std::wstring& wide);
std::wstring utf8ToWide(const std::string& utf8);

// ---- JSON 值 -------------------------------------------------------------
class JsonValue {
public:
    enum class Type {
        Null,
        Bool,
        Number,  // 内部区分 int64 / double
        String,
        Array,
        Object
    };

    JsonValue();
    JsonValue(bool v);
    JsonValue(int v);
    JsonValue(int64_t v);
    JsonValue(double v);
    JsonValue(const char* v);
    JsonValue(const std::string& v);

    static JsonValue makeArray();
    static JsonValue makeObject();

    Type type() const { return type_; }
    bool isNull()   const { return type_ == Type::Null; }
    bool isBool()   const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray()  const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    // 对象操作：set 不存在则追加，存在则覆盖
    JsonValue& set(const std::string& key, const JsonValue& value);
    bool has(const std::string& key) const;
    // 键不存在时返回静态 null 值（不抛异常）
    const JsonValue& get(const std::string& key) const;

    // 数组操作
    void push(const JsonValue& value);
    size_t size() const;
    const JsonValue& at(size_t index) const;

    // 取值（类型不匹配时返回默认值）
    std::string asString(const std::string& def = std::string()) const;
    int64_t     asInt64(int64_t def = 0) const;
    int         asInt(int def = 0) const;
    double      asDouble(double def = 0.0) const;
    bool        asBool(bool def = false) const;

    // 序列化；indent < 0 为紧凑单行，>= 0 为缩进美化
    std::string dump(int indent = -1) const;

    // 解析；失败返回 false 并给出 error 说明
    static bool parse(const std::string& text, JsonValue& out,
                      std::string* error = nullptr);

private:
    struct Parser;

    void dumpTo(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool boolValue_ = false;
    bool isInt_ = false;
    int64_t intValue_ = 0;
    double  doubleValue_ = 0.0;
    std::string stringValue_;
    std::vector<JsonValue> array_;
    // pair 而非 map：保留插入顺序
    std::vector<std::pair<std::string, JsonValue>> object_;
};

}  // namespace util
}  // namespace cliplink
