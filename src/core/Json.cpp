#include "core/Json.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace bt {
namespace {

const Json kNull;
const Json::Array kEmptyArray;
const Json::Object kEmptyObject;

constexpr unsigned kHighSurrogateFirst = 0xD800;
constexpr unsigned kLowSurrogateFirst = 0xDC00;
constexpr unsigned kSurrogateEnd = 0xE000;
constexpr unsigned kSurrogateBits = 10;
constexpr unsigned kSupplementaryBase = 0x10000;

void append_utf8(std::string& out, unsigned code_point) {
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

void dump_string(std::string& out, const std::string& text) {
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (byte < 0x20) {
                    out += "\\u00";
                    out.push_back(kHex[byte >> 4]);
                    out.push_back(kHex[byte & 0xF]);
                } else {
                    out.push_back(c);  // UTF-8 passes through unchanged
                }
        }
    }
    out.push_back('"');
}

void dump_number(std::string& out, double value) {
    if (!std::isfinite(value)) {
        out += "null";  // JSON has no NaN or infinity
        return;
    }
    char buffer[32];
    // Whole numbers (times, sizes) as plain integers: the shortest form of 1700000000.0 is "1.7e+09".
    constexpr double kExactIntegers = 9007199254740992.0;  // 2^53
    if (value == std::trunc(value) && std::fabs(value) < kExactIntegers) {
        const auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), static_cast<long long>(value));
        out.append(buffer, ec == std::errc{} ? end : buffer);
        return;
    }
    const auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, ec == std::errc{} ? end : buffer);
}

void newline(std::string& out, int indent, int level) {
    if (indent > 0) {
        out.push_back('\n');
        out.append(static_cast<std::size_t>(indent) * static_cast<std::size_t>(level), ' ');
    }
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    JsonParseResult run() {
        JsonParseResult result;
        skip_space();
        Json value;
        if (!parse_value(value, 0)) {
            result.error = "line " + std::to_string(line_) + ": " + error_;
            return result;
        }
        skip_space();
        if (pos_ != text_.size()) {
            result.error = "line " + std::to_string(line_) + ": unexpected text after the end";
            return result;
        }
        result.value = std::move(value);
        return result;
    }

private:
    bool fail(const char* message) {
        if (error_.empty()) {
            error_ = message;
        }
        return false;
    }

    void skip_space() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == '\n') {
                ++line_;
            } else if (c != ' ' && c != '\t' && c != '\r') {
                break;
            }
            ++pos_;
        }
    }

    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) {
            return fail("unknown word");
        }
        pos_ += word.size();
        return true;
    }

    bool parse_value(Json& out, int depth) {
        if (depth > kJsonMaxDepth) {
            return fail("nested too deeply");
        }
        if (pos_ >= text_.size()) {
            return fail("unexpected end of file");
        }
        switch (text_[pos_]) {
            case '{': return parse_object(out, depth);
            case '[': return parse_array(out, depth);
            case '"': {
                std::string text;
                if (!parse_string(text)) return false;
                out = Json(std::move(text));
                return true;
            }
            case 't': out = Json(true); return literal("true");
            case 'f': out = Json(false); return literal("false");
            case 'n': out = Json(); return literal("null");
            default: return parse_number(out);
        }
    }

    bool parse_number(Json& out) {
        const std::size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        const auto digits = [this] {
            const std::size_t from = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            return pos_ > from;
        };
        if (!digits()) return fail("expected a value");
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (!digits()) return fail("expected digits after '.'");
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            if (!digits()) return fail("expected digits in the exponent");
        }
        double value = 0.0;
        const auto [end, ec] = std::from_chars(text_.data() + start, text_.data() + pos_, value);
        if (ec == std::errc::result_out_of_range) {
            // Too big (1e999) or too small (1e-400) for a double. strtod rounds both the usual way:
            // infinity, clamped here to the largest double, or zero. The text is plain ASCII digits,
            // so the C locale's decimal point is the one that applies.
            value = std::strtod(std::string(text_.substr(start, pos_ - start)).c_str(), nullptr);
            if (std::isinf(value)) {
                value = value < 0 ? std::numeric_limits<double>::lowest() : std::numeric_limits<double>::max();
            }
        } else if (ec != std::errc{} || end != text_.data() + pos_) {
            return fail("bad number");
        }
        out = Json(value);
        return true;
    }

    bool parse_hex4(unsigned& out) {
        if (text_.size() - pos_ < 4) return fail("short \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }

    bool parse_string(std::string& out) {
        ++pos_;  // opening quote
        while (true) {
            if (pos_ >= text_.size()) return fail("unterminated string");
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in a string");
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) return fail("unterminated string");
            const char e = text_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned code = 0;
                    if (!parse_hex4(code)) return false;
                    if (code >= kHighSurrogateFirst && code < kLowSurrogateFirst) {
                        unsigned low = 0;
                        const std::size_t after_high = pos_;
                        if (text_.substr(pos_, 2) == "\\u") {
                            pos_ += 2;
                            if (!parse_hex4(low)) return false;
                        }
                        if (low < kLowSurrogateFirst || low >= kSurrogateEnd) {
                            // A lone half of a pair: replacement character. Whatever followed is
                            // read on its own (it may be an ordinary \\u escape).
                            code = 0xFFFD;
                            pos_ = after_high;
                        } else {
                            code = kSupplementaryBase + ((code - kHighSurrogateFirst) << kSurrogateBits) +
                                   (low - kLowSurrogateFirst);
                        }
                    } else if (code >= kLowSurrogateFirst && code < kSurrogateEnd) {
                        code = 0xFFFD;
                    }
                    append_utf8(out, code);
                    break;
                }
                default: return fail("bad escape in a string");
            }
        }
    }

    bool parse_array(Json& out, int depth) {
        ++pos_;
        Json::Array items;
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            out = Json(std::move(items));
            return true;
        }
        while (true) {
            skip_space();
            Json item;
            if (!parse_value(item, depth + 1)) return false;
            items.push_back(std::move(item));
            skip_space();
            if (pos_ >= text_.size()) return fail("unterminated array");
            const char c = text_[pos_++];
            if (c == ']') break;
            if (c != ',') return fail("expected ',' or ']'");
        }
        out = Json(std::move(items));
        return true;
    }

    bool parse_object(Json& out, int depth) {
        ++pos_;
        Json::Object members;
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            out = Json(std::move(members));
            return true;
        }
        while (true) {
            skip_space();
            if (pos_ >= text_.size() || text_[pos_] != '"') return fail("expected a key");
            std::string key;
            if (!parse_string(key)) return false;
            skip_space();
            if (pos_ >= text_.size() || text_[pos_] != ':') return fail("expected ':'");
            ++pos_;
            skip_space();
            Json value;
            if (!parse_value(value, depth + 1)) return false;
            members.emplace_back(std::move(key), std::move(value));
            skip_space();
            if (pos_ >= text_.size()) return fail("unterminated object");
            const char c = text_[pos_++];
            if (c == '}') break;
            if (c != ',') return fail("expected ',' or '}'");
        }
        out = Json(std::move(members));
        return true;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    int line_ = 1;
    std::string error_;
};

}  // namespace

bool Json::as_bool(bool fallback) const {
    const bool* value = std::get_if<bool>(&value_);
    return value != nullptr ? *value : fallback;
}

double Json::as_number(double fallback) const {
    const double* value = std::get_if<double>(&value_);
    return value != nullptr ? *value : fallback;
}

std::int64_t Json::as_int(std::int64_t fallback) const {
    const double* value = std::get_if<double>(&value_);
    if (value == nullptr || std::isnan(*value)) {
        return fallback;
    }
    // 2^63 is exactly representable; anything at or beyond it would overflow the conversion.
    constexpr double kLimit = 9223372036854775808.0;
    if (*value >= kLimit) return std::numeric_limits<std::int64_t>::max();
    if (*value < -kLimit) return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(*value);
}

std::string Json::as_string(std::string_view fallback) const {
    const std::string* value = std::get_if<std::string>(&value_);
    return value != nullptr ? *value : std::string(fallback);
}

const Json::Array& Json::as_array() const {
    const Array* value = std::get_if<Array>(&value_);
    return value != nullptr ? *value : kEmptyArray;
}

const Json::Object& Json::as_object() const {
    const Object* value = std::get_if<Object>(&value_);
    return value != nullptr ? *value : kEmptyObject;
}

const Json& Json::operator[](std::string_view key) const {
    for (const auto& [name, value] : as_object()) {
        if (name == key) {
            return value;
        }
    }
    return kNull;
}

void Json::set(std::string key, Json value) {
    if (is_null()) {
        value_ = Object{};
    }
    if (Object* members = std::get_if<Object>(&value_)) {
        members->emplace_back(std::move(key), std::move(value));
    }
}

void Json::push(Json value) {
    if (is_null()) {
        value_ = Array{};
    }
    if (Array* items = std::get_if<Array>(&value_)) {
        items->push_back(std::move(value));
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dump_to(out, indent, 0);
    return out;
}

void Json::dump_to(std::string& out, int indent, int level) const {
    if (is_null()) {
        out += "null";
    } else if (const bool* b = std::get_if<bool>(&value_)) {
        out += *b ? "true" : "false";
    } else if (const double* d = std::get_if<double>(&value_)) {
        dump_number(out, *d);
    } else if (const std::string* s = std::get_if<std::string>(&value_)) {
        dump_string(out, *s);
    } else if (const Array* a = std::get_if<Array>(&value_)) {
        out.push_back('[');
        for (std::size_t i = 0; i < a->size(); ++i) {
            if (i > 0) out.push_back(',');
            newline(out, indent, level + 1);
            (*a)[i].dump_to(out, indent, level + 1);
        }
        if (!a->empty()) newline(out, indent, level);
        out.push_back(']');
    } else if (const Object* o = std::get_if<Object>(&value_)) {
        out.push_back('{');
        for (std::size_t i = 0; i < o->size(); ++i) {
            if (i > 0) out.push_back(',');
            newline(out, indent, level + 1);
            dump_string(out, (*o)[i].first);
            out += indent > 0 ? ": " : ":";
            (*o)[i].second.dump_to(out, indent, level + 1);
        }
        if (!o->empty()) newline(out, indent, level);
        out.push_back('}');
    }
}

JsonParseResult parse_json(std::string_view text) {
    // A UTF-8 byte order mark (Notepad) is not part of the document.
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }
    return Parser(text).run();
}

}  // namespace bt
