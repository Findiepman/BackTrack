#pragma once

// Minimal test helpers: plain main() test programs, no framework (§4 of the build prompt).

#include <cstdio>
#include <format>
#include <string>
#include <string_view>

namespace bt_test {

inline int g_checks = 0;
inline int g_failures = 0;

inline void report_failure(const char* file, int line, const std::string& what) {
    ++g_failures;
    std::printf("%s:%d: FAILED: %s\n", file, line, what.c_str());
}

inline int finish(const char* suite) {
    std::printf("%s: %d checks, %d failed\n", suite, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

inline bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

}  // namespace bt_test

#define CHECK(cond)                                                     \
    do {                                                                \
        ++bt_test::g_checks;                                            \
        if (!(cond)) bt_test::report_failure(__FILE__, __LINE__, #cond); \
    } while (0)

#define CHECK_EQ(actual, expected)                                                                         \
    do {                                                                                                   \
        ++bt_test::g_checks;                                                                               \
        const auto& bt_a = (actual);                                                                       \
        const auto& bt_e = (expected);                                                                     \
        if (!(bt_a == bt_e))                                                                               \
            bt_test::report_failure(__FILE__, __LINE__,                                                    \
                                    std::format("{} == {} (got '{}', expected '{}')", #actual, #expected, \
                                                bt_a, bt_e));                                              \
    } while (0)
