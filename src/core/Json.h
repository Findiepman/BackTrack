#pragma once

// A small JSON reader and writer for Backtrack's own files (library.json). Only what those need:
// objects keep their key order so a saved file diffs cleanly, numbers are doubles, strings are
// UTF-8. The reader never throws and never recurses deeper than kMaxDepth, so a damaged file is
// "could not read", never a crash.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace bt {

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;

    Json() = default;  // null
    Json(std::nullptr_t) {}
    Json(bool value) : value_(value) {}
    Json(double value) : value_(value) {}
    Json(int value) : value_(static_cast<double>(value)) {}
    Json(std::int64_t value) : value_(static_cast<double>(value)) {}
    Json(std::string value) : value_(std::move(value)) {}
    Json(std::string_view value) : value_(std::string(value)) {}
    Json(const char* value) : value_(std::string(value)) {}
    Json(Array value) : value_(std::move(value)) {}
    Json(Object value) : value_(std::move(value)) {}

    bool is_null() const { return std::holds_alternative<std::monostate>(value_); }
    bool is_bool() const { return std::holds_alternative<bool>(value_); }
    bool is_number() const { return std::holds_alternative<double>(value_); }
    bool is_string() const { return std::holds_alternative<std::string>(value_); }
    bool is_array() const { return std::holds_alternative<Array>(value_); }
    bool is_object() const { return std::holds_alternative<Object>(value_); }

    // Typed reads with a fallback for a missing or wrongly typed value (an old or hand-edited file).
    bool as_bool(bool fallback = false) const;
    double as_number(double fallback = 0.0) const;
    std::int64_t as_int(std::int64_t fallback = 0) const;  // clamped to the int64 range, NaN -> fallback
    std::string as_string(std::string_view fallback = {}) const;
    const Array& as_array() const;  // empty when not an array
    const Object& as_object() const;  // empty when not an object

    // Object member, or a null Json when absent (first match if a key repeats).
    const Json& operator[](std::string_view key) const;

    // Appends a member to an object (turns a null into an empty object first).
    void set(std::string key, Json value);
    // Appends to an array (turns a null into an empty array first).
    void push(Json value);

    // Compact when indent is 0, otherwise pretty-printed with that many spaces per level.
    std::string dump(int indent = 0) const;

private:
    void dump_to(std::string& out, int indent, int level) const;

    std::variant<std::monostate, bool, double, std::string, Array, Object> value_;
};

struct JsonParseResult {
    std::optional<Json> value;
    std::string error;  // "line 3: expected ':'" when value is empty
};

inline constexpr int kJsonMaxDepth = 64;

JsonParseResult parse_json(std::string_view text);

}  // namespace bt
