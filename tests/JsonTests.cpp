#include "core/Json.h"

#include "Check.h"

#include <cmath>
#include <limits>

using namespace bt;

namespace {

Json parse_ok(std::string_view text) {
    JsonParseResult r = parse_json(text);
    CHECK(r.value.has_value());
    if (!r.value) std::printf("    parse error: %s\n", r.error.c_str());
    return r.value ? *r.value : Json();
}

bool parse_fails(std::string_view text) {
    return !parse_json(text).value.has_value();
}

void values() {
    CHECK(parse_ok("null").is_null());
    CHECK_EQ(parse_ok("true").as_bool(), true);
    CHECK_EQ(parse_ok(" false ").as_bool(true), false);
    CHECK_EQ(parse_ok("42").as_int(), std::int64_t{42});
    CHECK_EQ(parse_ok("-3.5e2").as_number(), -350.0);
    CHECK_EQ(parse_ok("0.25").as_number(), 0.25);
    CHECK_EQ(parse_ok("\"hi\"").as_string(), std::string("hi"));
    const Json obj = parse_ok(R"({"a": 1, "b": [true, null, "x"], "c": {"d": "e"}})");
    CHECK_EQ(obj["a"].as_int(), std::int64_t{1});
    CHECK_EQ(obj["b"].as_array().size(), std::size_t{3});
    CHECK_EQ(obj["c"]["d"].as_string(), std::string("e"));
    CHECK(obj["missing"].is_null());
    CHECK(obj["a"]["deeper"].is_null());  // indexing a number is a null, not a crash
    CHECK(parse_ok("[]").as_array().empty());
    CHECK(parse_ok("{}").is_object());
}

void fallbacks_for_wrong_types() {
    const Json obj = parse_ok(R"({"n": "12", "s": 5, "b": 1})");
    CHECK_EQ(obj["n"].as_int(7), std::int64_t{7});
    CHECK_EQ(obj["s"].as_string("x"), std::string("x"));
    CHECK_EQ(obj["b"].as_bool(true), true);
    CHECK(obj["n"].as_array().empty());
    CHECK_EQ(parse_ok("1e300").as_int(), std::numeric_limits<std::int64_t>::max());
    CHECK_EQ(parse_ok("-1e300").as_int(), std::numeric_limits<std::int64_t>::min());
    CHECK_EQ(parse_ok("1e999").as_number(), std::numeric_limits<double>::max());
    CHECK_EQ(parse_ok("-1e999").as_number(), std::numeric_limits<double>::lowest());
    CHECK_EQ(parse_ok("1e-400").as_number(), 0.0);  // too small: zero, not the largest double
}

void strings_and_unicode() {
    // Parsed outside the macros: MSVC's preprocessor re-reads a stringized raw string and chokes
    // on its backslashes and unicode escapes.
    const std::string escaped = parse_ok(R"("a\"b\\c\/d\n\t")").as_string();
    const std::string accent = parse_ok(R"("\u00e9")").as_string();
    const std::string pair = parse_ok(R"("\ud83c\udfae")").as_string();
    const std::string lone_high = parse_ok(R"("\ud83c")").as_string();
    const std::string lone_low = parse_ok(R"("\udfae")").as_string();
    CHECK(escaped == "a\"b\\c/d\n\t");
    CHECK(accent == "\xC3\xA9");  // e with an acute accent
    CHECK(pair == "\xF0\x9F\x8E\xAE");  // a game controller emoji
    CHECK(lone_high == "\xEF\xBF\xBD");  // half a pair: replacement character
    CHECK(lone_low == "\xEF\xBF\xBD");
    const std::string lone_then_letter = parse_ok(R"("\ud800\u0041")").as_string();
    CHECK(lone_then_letter == "\xEF\xBF\xBD" "A");  // the escape after a lone half is kept
    CHECK_EQ(parse_ok("\"caf\xC3\xA9 \xE6\x97\xA5\"").as_string(), std::string("caf\xC3\xA9 \xE6\x97\xA5"));
    CHECK_EQ(parse_ok("\xEF\xBB\xBF\"bom\"").as_string(), std::string("bom"));
}

void damaged_input_is_refused() {
    CHECK(parse_fails(""));
    CHECK(parse_fails("{"));
    CHECK(parse_fails("[1,]"));
    CHECK(parse_fails("{\"a\" 1}"));
    CHECK(parse_fails("{\"a\": 1,}"));
    CHECK(parse_fails("{a: 1}"));
    CHECK(parse_fails("\"unterminated"));
    CHECK(parse_fails("\"bad \\x escape\""));
    CHECK(parse_fails("\"\\u12\""));
    CHECK(parse_fails("\"tab\there\""));
    CHECK(parse_fails("tru"));
    CHECK(parse_fails("01x"));
    CHECK(parse_fails("1."));
    CHECK(parse_fails("-"));
    CHECK(parse_fails("{} extra"));
    CHECK(parse_fails(std::string(1000, '\0')));
    // Deep nesting is refused, not a stack overflow.
    CHECK(parse_fails(std::string(100000, '[')));
    CHECK(!parse_fails(std::string(kJsonMaxDepth, '[') + std::string(kJsonMaxDepth, ']')));
    const JsonParseResult r = parse_json("{\n\"a\":\n}");
    CHECK(bt_test::contains(r.error, "line 3"));
    // A file cut off mid-write (power loss) at every possible length never crashes.
    const std::string full = R"({"clips":[{"file":"a.mp4","tags":["x","y"],"duration":30.5}],"v":1})";
    for (std::size_t n = 0; n < full.size(); ++n) {
        CHECK(parse_fails(full.substr(0, n)));
    }
}

void round_trip() {
    Json root;
    root.set("name", Json("Clip \"one\"\n\x01"));
    root.set("n", Json(30.5));
    root.set("big", Json(std::int64_t{1730736000}));
    root.set("flag", Json(false));
    root.set("nothing", Json());
    Json tags{Json::Array{}};
    tags.push(Json("a"));
    tags.push(Json("\xE6\x97\xA5"));
    root.set("tags", std::move(tags));
    root.set("nan", Json(std::nan("")));
    for (const int indent : {0, 2}) {
        const std::string text = root.dump(indent);
        const Json back = parse_ok(text);
        CHECK_EQ(back["name"].as_string(), std::string("Clip \"one\"\n\x01"));
        CHECK_EQ(back["n"].as_number(), 30.5);
        CHECK_EQ(back["big"].as_int(), std::int64_t{1730736000});
        CHECK_EQ(back["flag"].as_bool(true), false);
        CHECK(back["nothing"].is_null());
        CHECK(back["nan"].is_null());
        CHECK_EQ(back["tags"].as_array()[1].as_string(), std::string("\xE6\x97\xA5"));
        CHECK_EQ(back.dump(indent), text);  // stable: saving twice gives the same file
    }
    CHECK_EQ(Json(Json::Array{}).dump(2), std::string("[]"));
    CHECK_EQ(Json(1700000000.0).dump(), std::string("1700000000"));  // not 1.7e+09
    CHECK_EQ(Json(-42).dump(), std::string("-42"));
    CHECK_EQ(Json(30.5).dump(), std::string("30.5"));
    CHECK_EQ(Json(1e300).dump(), std::string("1e+300"));
    CHECK_EQ(Json(Json::Object{}).dump(2), std::string("{}"));
    CHECK(bt_test::contains(root.dump(2), "\n  \"n\": 30.5"));
}

}  // namespace

int main() {
    values();
    fallbacks_for_wrong_types();
    strings_and_unicode();
    damaged_input_is_refused();
    round_trip();
    return bt_test::finish("JsonTests");
}
