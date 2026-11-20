#pragma once

#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loomwright_test {

struct TestCase {
    std::string name;
    void (*function)();
};

std::vector<TestCase>& registry();

struct Registrar {
    Registrar(const char* name, void (*function)()) { registry().push_back(TestCase{name, function}); }
};

[[noreturn]] void fail(const char* file, int line, std::string_view message);
[[noreturn]] void fail_equal(const char* file, int line, std::string_view message,
                             const std::string& actual, const std::string& expected);

template <typename T>
std::string to_text(const T& value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

inline std::string to_text(bool value) { return value ? "true" : "false"; }
inline const std::string& to_text(const std::string& value) { return value; }

inline std::string to_text(const std::vector<std::string>& values) {
    std::string out;
    for (const auto& value : values) {
        if (!out.empty()) out += ", ";
        out += "\"" + value + "\"";
    }
    return "[" + out + "]";
}

// Keep formatting and string temporaries off the assertion sites, including
// when LTO can see the reporting code.
template <typename Actual, typename Expected>
[[noreturn, gnu::cold, gnu::noinline]] void report_equal_failure(
    const char* file, int line, std::string_view message, const Actual& actual, const Expected& expected) {
    fail_equal(file, line, message, to_text(actual), to_text(expected));
}

/// stdout/stderr captured while `fn` ran.
struct CapturedOutput {
    int code = 0;
    std::string out;
    std::string err;
};

/// Run `fn` with stdout and stderr redirected to pipes and return what it printed.
CapturedOutput capture_output(const std::function<int()>& fn);

/// Trim trailing newlines so CHECK_EQ reads naturally.
inline std::string strip_trailing_newlines(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

int run_all(const std::string& filter = std::string());

}  // namespace loomwright_test

#define TEST_CASE(name)                                              \
    static void name();                                              \
    static ::loomwright_test::Registrar registrar_##name(#name, &name); \
    static void name()

#define CHECK_TRUE(expression)                                              \
    do {                                                                    \
        if (!(expression)) {                                                \
            ::loomwright_test::fail(__FILE__, __LINE__,                       \
                                 "CHECK_TRUE(" #expression ")");       \
        }                                                                   \
    } while (false)

#define CHECK_EQ(actual, expected)                                                              \
    do {                                                                                        \
        const auto& actual_value_ = (actual);                                                   \
        const auto& expected_value_ = (expected);                                               \
        if (!(actual_value_ == expected_value_)) {                                              \
            ::loomwright_test::report_equal_failure(__FILE__, __LINE__,                          \
                "CHECK_EQ(" #actual ", " #expected ")", actual_value_, expected_value_);         \
        }                                                                                       \
    } while (false)

#define CHECK_THROWS(expression, exception_type)                                    \
    do {                                                                           \
        bool caught_ = false;                                                      \
        try {                                                                      \
            (void)(expression);                                                    \
        } catch (const exception_type&) {                                          \
            caught_ = true;                                                        \
        } catch (...) {                                                            \
        }                                                                          \
        if (!caught_) {                                                            \
            ::loomwright_test::fail(__FILE__, __LINE__,                             \
                                 "expected " #exception_type " from " #expression); \
        }                                                                          \
    } while (false)
