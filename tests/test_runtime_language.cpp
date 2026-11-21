#include "harness.hpp"
#include "runtime_fixtures.hpp"

#include <memory>
#include <string>
#include <vector>

#include "parser.hpp"
#include "runtime.hpp"
#include "scheduler.hpp"
#include "screen_geometry.hpp"

using namespace loomwright;
using loomwright_test::eval_expression;
using loomwright_test::RecordingBackend;

namespace {

/// Evaluate one expression in a throwaway script and return the runtime's
/// error text, so error paths need no exception plumbing in each test.
std::string expression_error(const std::string& expression) {
    Script script = parse("loom \"error\" { when app starts { set result = " + expression + " } }");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    return runtime.runtime_error();
}

/// The variables and error text after running a sequence of statements.
struct StatementResult {
    VariableMap variables;
    std::string error;
};

StatementResult run_parsed(const Script& script) {
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    return StatementResult{runtime.variables(), runtime.runtime_error()};
}

StatementResult run_statements(const std::string& body) {
    return run_parsed(parse("loom \"statements\" { when app starts { " + body + " } }"));
}

}  // namespace


TEST_CASE(evaluates_language_expressions) {
    CHECK_EQ(eval_expression("5-2").to_string(), std::string("3"));
    CHECK_EQ(eval_expression("1 / 2").to_string(), std::string("0.5"));
    CHECK_EQ(eval_expression("7 % 3").to_string(), std::string("1"));
    CHECK_EQ(eval_expression("-7 % 3").to_string(), std::string("2"));
    CHECK_EQ(eval_expression("true + 2").to_string(), std::string("3"));
    CHECK_EQ(eval_expression("-7 % true").to_string(), std::string("0"));
    CHECK_EQ(eval_expression("true * 2.5").to_string(), std::string("2.5"));
    CHECK_EQ(eval_expression("\"ab\" + \"cd\"").to_string(), std::string("abcd"));
    CHECK_EQ(eval_expression("clamp(12, 0, 10)").to_string(), std::string("10"));
    CHECK_EQ(eval_expression("round(2.5)").to_string(), std::string("2"));
    CHECK_EQ(eval_expression("round(3.5)").to_string(), std::string("4"));
    CHECK_EQ(eval_expression("str(1.0)").to_string(), std::string("1.0"));
    CHECK_EQ(eval_expression("min(3, 1, 2)").to_string(), std::string("1"));
    CHECK_EQ(eval_expression("max(3, 1, 2)").to_string(), std::string("3"));
    CHECK_EQ(eval_expression("not true").to_string(), std::string("false"));
    CHECK_EQ(eval_expression("len(\"hello\")").to_string(), std::string("5"));
    CHECK_EQ(eval_expression("1 == 1.0").to_string(), std::string("true"));
    CHECK_EQ(eval_expression("true and false").to_string(), std::string("false"));
    CHECK_EQ(eval_expression("floor(2.9)").to_string(), std::string("2"));
    CHECK_EQ(eval_expression("ceil(2.1)").to_string(), std::string("3"));
}

TEST_CASE(pure_binary_operands_preserve_string_list_and_event_values) {
    const Script script = parse(R"LOOMWRIGHT(
loom "operands" {
    when key A goes down {
        set text = "0123456789abcdef"
        set text = text + (event_key + text)
        set items = [1, 2]
        set joined = items + items
        set observed = [1] + append(items, 3)
    }
}
)LOOMWRIGHT");
    Runtime runtime(script);
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0), "A", "down"));
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().at("text").to_string(), "0123456789abcdefA0123456789abcdef");
    CHECK_EQ(runtime.variables().at("joined").to_string(), "[1, 2, 1, 2]");
    CHECK_EQ(runtime.variables().at("observed").to_string(), "[1, 1, 2, 3]");
}

TEST_CASE(executes_statement_flow) {
    const Script script = parse(R"LOOMWRIGHT(
loom "flow" {
    when app starts {
        set total = 0
        repeat 4 {
            set total = total + 2
        }
        while total > 5 {
            set total = total - 4
        }
        assert total == 4
    }
}
)LOOMWRIGHT");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().at("total").to_string(), std::string("4"));
}

TEST_CASE(stops_a_runaway_while_loop) {
    const Script script = parse(R"LOOMWRIGHT(
loom "runaway" {
    when app starts {
        set count = 0
        while true {
            set count = count + 1
        }
    }
}
)LOOMWRIGHT");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    CHECK_TRUE(runtime.scheduler().stop_requested());
}

TEST_CASE(emits_cursor_and_tap_actions) {
    const Script script = parse(R"LOOMWRIGHT(
loom "taps" {
    when key A goes down {
        cursor center
        mouse left tap for 10ms
        key CTRL+S tap for 10ms
        type "hi"
        scroll 0, -3
    }
}
)LOOMWRIGHT");
    Runtime::Options options;
    options.screen_size = "1920x1080";
    Runtime runtime(script, options);
    auto backend = std::make_unique<RecordingBackend>();
    RecordingBackend* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.set_panic_key("");

    runtime.physical_key("fake", "A", true);
    runtime.scheduler().run();

    CHECK_EQ(runtime.width(), 1920);
    CHECK_EQ(raw->log,
             std::vector<std::string>({"warp 960, 540", "button left down", "button left up", "key LEFTCTRL down",
                                       "key S down", "key S up", "key LEFTCTRL up", "type hi", "scroll 0, -3"}));
}

TEST_CASE(screen_option_is_parsed_and_validated) {
    const Value auto_value = Value::of_str("auto");
    const auto fallback = parse_screen_option(&auto_value, {1920, 1080});
    CHECK_EQ(fallback.first, 1920);
    CHECK_EQ(fallback.second, 1080);

    const Value custom = Value::of_str("2560x1440");
    const auto parsed = parse_screen_option(&custom, {1920, 1080});
    CHECK_EQ(parsed.first, 2560);
    CHECK_EQ(parsed.second, 1440);

    const Value invalid = Value::of_str("2560-1440");
    CHECK_THROWS(parse_screen_option(&invalid, {1920, 1080}), RuntimeErrorLoomwright);
    const Value not_a_string = Value::of_int(5);
    CHECK_THROWS(parse_screen_option(&not_a_string, {1920, 1080}), RuntimeErrorLoomwright);
}

TEST_CASE(screen_geometry_parses_compositor_tool_outputs) {
    const auto wl = detail::parse_wlr_randr("DP-2\n  800x600 px\n  1920x1080 px, 60 Hz\n");
    CHECK_TRUE(wl.has_value());
    CHECK_EQ(wl->first, 1920);
    CHECK_EQ(wl->second, 1080);
    const auto x11 = detail::parse_xrandr(
        "Screen 0: minimum 8 x 8\nDP-1 connected 1920x1080+-1920+0 (normal)\n"
        "HDMI-1 connected primary 2560x1440+0+0 (normal)\nDP-2 disconnected 800x600+5000+5000\n");
    CHECK_TRUE(x11.has_value());
    CHECK_EQ(x11->first, 4480);
    CHECK_EQ(x11->second, 1440);
    const auto negative = detail::parse_xrandr("DP-1 connected 1920x1080-100-200\n");
    CHECK_TRUE(negative.has_value());
    CHECK_EQ(negative->first, 1920);
    CHECK_EQ(negative->second, 1080);
    const auto info = detail::parse_xdpyinfo("screen #0:\n  dimensions:\t1368x768 pixels (355x200 millimeters)\n");
    CHECK_TRUE(info.has_value());
    CHECK_EQ(info->first, 1368);
    CHECK_EQ(info->second, 768);
}

TEST_CASE(screen_geometry_rejects_malformed_or_unrepresentable_results) {
    CHECK_TRUE(!detail::parse_xrandr("DP-1 connected 10x20+5+6junk\nDP-2 connected (normal)\n"));
    CHECK_TRUE(!detail::parse_xrandr("DP-1 connected 1920x1080+2147483647+0\n"));
    CHECK_TRUE(!detail::parse_xrandr("DP-1 connected 100x100-2147483648+0\nDP-2 connected 100x100+0+0\n"));
    CHECK_TRUE(!detail::parse_wlr_randr("  0x1080 px\n  2147483648x1080 px\n"));
    CHECK_TRUE(!detail::parse_xdpyinfo("  dimensions: 1920x0 pixels\n"));
}

TEST_CASE(split_shell_words_matches_shlex) {
    CHECK_EQ(split_shell_words("gnome-calculator"), std::vector<std::string>({"gnome-calculator"}));
    CHECK_EQ(split_shell_words("cmd -a 'two words' \"three four\""),
             std::vector<std::string>({"cmd", "-a", "two words", "three four"}));
    CHECK_EQ(split_shell_words(R"(printf "a\"b")"), std::vector<std::string>({"printf", "a\"b"}));
    CHECK_THROWS(split_shell_words("cmd 'unclosed"), BackendError);
}

TEST_CASE(run_command_captures_output_and_exit_status) {
    const CommandResult ok = run_command({"/bin/sh", "-c", "printf out; printf err >&2"}, "", 2000);
    CHECK_TRUE(!ok.spawn_failed);
    CHECK_TRUE(!ok.timed_out);
    CHECK_EQ(ok.returncode, 0);
    CHECK_EQ(ok.out, "out");
    CHECK_EQ(ok.err, "err");

    const CommandResult status = run_command({"/bin/sh", "-c", "exit 7"}, "", 2000);
    CHECK_TRUE(!status.spawn_failed);
    CHECK_EQ(status.returncode, 7);
}

TEST_CASE(run_command_writes_stdin) {
    const CommandResult result = run_command({"/bin/cat"}, "hello\nfrom stdin", 2000);
    CHECK_TRUE(!result.spawn_failed);
    CHECK_TRUE(!result.timed_out);
    CHECK_EQ(result.returncode, 0);
    CHECK_EQ(result.out, "hello\nfrom stdin");
}

TEST_CASE(run_command_enforces_timeout) {
    const CommandResult result = run_command({"/bin/sleep", "10"}, "", 50);
    CHECK_TRUE(!result.spawn_failed);
    CHECK_TRUE(result.timed_out);
}

TEST_CASE(spawn_detached_returns_without_waiting_and_reports_missing_command) {
    const auto start = std::chrono::steady_clock::now();
    const auto started = spawn_detached({"/bin/sleep", "2"});
    CHECK_TRUE(started.has_value());
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    CHECK_TRUE(elapsed.count() < 1500);

    const auto missing = spawn_detached({"/definitely/not/a/loomwright-command"});
    CHECK_TRUE(!missing.has_value());
    CHECK_TRUE(!missing.error().empty());
}

TEST_CASE(evaluates_list_and_string_functions) {
    CHECK_EQ(eval_expression("len([\"a\", \"b\"])").to_string(), std::string("2"));
    CHECK_EQ(eval_expression("[\"a\", \"b\"][0]").to_string(), std::string("a"));
    CHECK_EQ(eval_expression("[\"a\", \"b\"][-1]").to_string(), std::string("b"));
    CHECK_EQ(eval_expression("join([\"a\", \"b\"], \"-\")").to_string(), std::string("a-b"));
    CHECK_EQ(eval_expression("split(\"a,b\", \",\")[1]").to_string(), std::string("b"));
    CHECK_EQ(eval_expression("upper(\"hi\")").to_string(), std::string("HI"));
    CHECK_EQ(eval_expression("lower(\"HI\")").to_string(), std::string("hi"));
    CHECK_EQ(eval_expression("trim(\"  hi  \")").to_string(), std::string("hi"));
    CHECK_EQ(eval_expression("starts_with(\"hello\", \"he\")").to_string(), std::string("true"));
    CHECK_EQ(eval_expression("ends_with(\"hello\", \"lo\")").to_string(), std::string("true"));
    CHECK_EQ(eval_expression("contains(\"hello\", \"ell\")").to_string(), std::string("true"));
    CHECK_EQ(eval_expression("contains([\"a\", \"b\"], \"b\")").to_string(), std::string("true"));
    CHECK_EQ(eval_expression("replace(\"a-b\", \"-\", \"+\")").to_string(), std::string("a+b"));
    CHECK_EQ(eval_expression("substr(\"hello\", 1, 3)").to_string(), std::string("ell"));
    CHECK_EQ(eval_expression("substr(\"hello\", -2)").to_string(), std::string("lo"));
    CHECK_EQ(eval_expression("index_of(\"hello\", \"l\")").to_string(), std::string("2"));
    CHECK_EQ(eval_expression("index_of(\"hello\", \"z\")").to_string(), std::string("-1"));
}

TEST_CASE(slices_return_forgiving_copies) {
    // All four spellings: start inclusive, end exclusive.
    CHECK_EQ(eval_expression("[1, 2, 3, 4][1:3]").to_string(), std::string("[2, 3]"));
    CHECK_EQ(eval_expression("[1, 2, 3, 4][1:]").to_string(), std::string("[2, 3, 4]"));
    CHECK_EQ(eval_expression("[1, 2, 3, 4][:2]").to_string(), std::string("[1, 2]"));
    CHECK_EQ(eval_expression("[1, 2, 3, 4][:]").to_string(), std::string("[1, 2, 3, 4]"));

    // Negative bounds count from the end, as indexing does.
    CHECK_EQ(eval_expression("[1, 2, 3, 4][-2:]").to_string(), std::string("[3, 4]"));
    CHECK_EQ(eval_expression("[1, 2, 3, 4][:-1]").to_string(), std::string("[1, 2, 3]"));
    CHECK_EQ(eval_expression("[1, 2, 3, 4][-3:-1]").to_string(), std::string("[2, 3]"));
    CHECK_EQ(eval_expression("[1, 2, 3, 4][-1:4]").to_string(), std::string("[4]"));

    // Out-of-range bounds clamp instead of raising "index out of range".
    CHECK_EQ(eval_expression("[1, 2, 3][-100:100]").to_string(), std::string("[1, 2, 3]"));
    CHECK_EQ(eval_expression("[1, 2, 3][2:100]").to_string(), std::string("[3]"));
    CHECK_EQ(eval_expression("[1, 2, 3][100:]").to_string(), std::string("[]"));
    CHECK_EQ(eval_expression("[1, 2, 3][:-100]").to_string(), std::string("[]"));

    // Inverted and empty ranges yield an empty list.
    CHECK_EQ(eval_expression("[1, 2, 3][2:1]").to_string(), std::string("[]"));
    CHECK_EQ(eval_expression("[1, 2, 3][2:2]").to_string(), std::string("[]"));
    CHECK_EQ(eval_expression("[][:]").to_string(), std::string("[]"));
    CHECK_EQ(eval_expression("[][0:5]").to_string(), std::string("[]"));

    // Variable and expression bounds.
    const Script variables = parse(R"LOOMWRIGHT(
 loom "slice-vars" {
     when app starts {
         set items = [10, 20, 30, 40]
         set i = 1
         set n = 4
         set a = items[i:n-1]
         set b = items[len(items) - 2:]
     }
 }
 )LOOMWRIGHT");
    const auto variable_result = run_parsed(variables);
    CHECK_TRUE(variable_result.error.empty());
    CHECK_EQ(variable_result.variables.at("a").to_string(), std::string("[20, 30]"));
    CHECK_EQ(variable_result.variables.at("b").to_string(), std::string("[30, 40]"));

    // Function-call bounds, both waiting user functions and builtins.
    const Script calls = parse(R"LOOMWRIGHT(
 loom "slice-calls" {
     func slice_lo() {
         return 1
     }
     func slice_hi() {
         return 3
     }
     when app starts {
         set items = [10, 20, 30, 40]
         set picked = items[slice_lo():slice_hi()]
         set tail = items[len(items) - 2:]
     }
 }
 )LOOMWRIGHT");
    const auto call_result = run_parsed(calls);
    CHECK_TRUE(call_result.error.empty());
    CHECK_EQ(call_result.variables.at("picked").to_string(), std::string("[20, 30]"));
    CHECK_EQ(call_result.variables.at("tail").to_string(), std::string("[30, 40]"));
}

TEST_CASE(slice_result_is_independent_of_the_source_list) {
    const Script script = parse(R"LOOMWRIGHT(
 loom "slice-copy" {
     when app starts {
         set source = [1, 2, 3]
         set part = source[:]
         call append(part, 4)
         set part += [9]
         set grown = part + [5]
     }
 }
 )LOOMWRIGHT");
    const auto result = run_parsed(script);
    CHECK_TRUE(result.error.empty());
    CHECK_EQ(result.variables.at("source").to_string(), std::string("[1, 2, 3]"));
    CHECK_EQ(result.variables.at("part").to_string(), std::string("[1, 2, 3, 4, 9]"));
    CHECK_EQ(result.variables.at("grown").to_string(), std::string("[1, 2, 3, 4, 9, 5]"));
}

TEST_CASE(slice_bounds_may_wait_and_are_evaluated_once) {
    const Script script = parse(R"LOOMWRIGHT(
 loom "slice-wait" {
     func tick() {
         wait 1ms
         call append(counter, 1)
         return len(counter) - 1
     }
     when app starts {
         set items = [10, 20, 30, 40]
         set counter = []
         set picked = items[tick():tick()]
     }
 }
 )LOOMWRIGHT");
    const auto result = run_parsed(script);
    CHECK_TRUE(result.error.empty());
    // First tick returns 0 (start), second returns 1 (end): items[0:1].
    CHECK_EQ(result.variables.at("picked").to_string(), std::string("[10]"));
    CHECK_EQ(result.variables.at("counter").to_string(), std::string("[1, 1]"));
}

TEST_CASE(slice_rejects_non_list_bases_and_keeps_index_errors) {
    CHECK_TRUE(expression_error("42[:]").contains("only a list can be sliced, got int"));
    CHECK_TRUE(expression_error("\"abc\"[:1]").contains("only a list can be sliced, got str"));

    // A bound is not evaluated when the base is not a list.
    const Script script = parse(R"LOOMWRIGHT(
 loom "slice-bad-base" {
     func tick() {
         call append(counter, 1)
         return 0
     }
     when app starts {
         set counter = []
         set result = 42[tick():]
     }
 }
 )LOOMWRIGHT");
    const auto result = run_parsed(script);
    CHECK_TRUE(result.error.contains("only a list can be sliced, got int"));
    CHECK_EQ(result.variables.at("counter").to_string(), std::string("[]"));

    // Ordinary indexing errors are unchanged.
    CHECK_TRUE(expression_error("42[0]").contains("only a list can be indexed, got int"));
    CHECK_TRUE(expression_error("[1, 2][5]").contains("list index out of range"));
    CHECK_TRUE(expression_error("[1, 2][-3]").contains("list index out of range"));
    CHECK_EQ(eval_expression("[1, 2][-1]").to_string(), std::string("2"));
}

TEST_CASE(evaluates_math_helper_functions) {
    CHECK_EQ(eval_expression("pow(2, 10)").to_string(), std::string("1024.0"));
    CHECK_EQ(eval_expression("pow(4, 0.5)").to_string(), std::string("2.0"));
    CHECK_EQ(eval_expression("pow(-1, 0.5)").to_string(), std::string("NaN"));
    CHECK_EQ(eval_expression("sign(-42)").to_string(), std::string("-1"));
    CHECK_EQ(eval_expression("sign(0)").to_string(), std::string("0"));
    CHECK_EQ(eval_expression("sign(3.5)").to_string(), std::string("1"));
    CHECK_EQ(eval_expression("gcd(12, 18)").to_string(), std::string("6"));
    CHECK_EQ(eval_expression("gcd(0, 0)").to_string(), std::string("0"));
    CHECK_EQ(eval_expression("gcd(0, -15)").to_string(), std::string("15"));
    CHECK_EQ(eval_expression("lcm(4, 6)").to_string(), std::string("12"));
    CHECK_EQ(eval_expression("lcm(0, 7)").to_string(), std::string("0"));
    CHECK_EQ(eval_expression("lcm(-4, 6)").to_string(), std::string("12"));
    CHECK_EQ(eval_expression("sum([1, 2, 3])").to_string(), std::string("6"));
    CHECK_EQ(eval_expression("sum([])").to_string(), std::string("0"));
    CHECK_EQ(eval_expression("sum([1, 2.5])").to_string(), std::string("3.5"));
    CHECK_EQ(eval_expression("avg([1, 2, 3])").to_string(), std::string("2.0"));
    CHECK_EQ(eval_expression("avg([1, 2])").to_string(), std::string("1.5"));
}

TEST_CASE(math_helpers_report_errors_instead_of_undefined_results) {
    CHECK_TRUE(expression_error("avg([])").contains("avg() requires a non-empty list"));
    CHECK_TRUE(expression_error("sum([\"x\"])").contains("sum() expects a list of numbers"));
    CHECK_TRUE(expression_error("sum(42)").contains("sum() expects a list"));
    CHECK_TRUE(expression_error("sign(\"NaN\")").contains("sign() cannot be applied to NaN"));
    CHECK_TRUE(expression_error("abs(-9223372036854775807 - 1)").contains("integer overflow in abs()"));
    CHECK_TRUE(expression_error("gcd(-9223372036854775807 - 1, 0)").contains("integer overflow in gcd()"));
    CHECK_TRUE(expression_error("lcm(4611686018427387903, 4)").contains("integer overflow in lcm()"));
    CHECK_TRUE(expression_error("sum([9223372036854775807, 1])").contains("integer overflow in sum()"));
}

TEST_CASE(reverse_returns_a_copy_and_keeps_utf8_characters_intact) {
    CHECK_EQ(eval_expression("reverse(\"hello\")").to_string(), std::string("olleh"));
    CHECK_EQ(eval_expression("reverse(\"caf\xc3\xa9\")").to_string(), std::string("\xc3\xa9" "fac"));
    CHECK_EQ(eval_expression("reverse([1, 2, 3])").to_string(), std::string("[3, 2, 1]"));
    CHECK_EQ(eval_expression("reverse([])").to_string(), std::string("[]"));

    const Script script = parse(R"LOOMWRIGHT(
loom "reverse" {
    when app starts {
        set original = [1, 2]
        set copy = original
        set reversed = reverse(original)
        set changed = reversed + [3]
    }
}
)LOOMWRIGHT");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().at("original").to_string(), std::string("[1, 2]"));
    CHECK_EQ(runtime.variables().at("copy").to_string(), std::string("[1, 2]"));
    CHECK_EQ(runtime.variables().at("reversed").to_string(), std::string("[2, 1]"));
    CHECK_EQ(runtime.variables().at("changed").to_string(), std::string("[2, 1, 3]"));
}

TEST_CASE(evaluates_list_and_string_access_helpers) {
    CHECK_EQ(eval_expression("first([1, 2])").to_string(), std::string("1"));
    CHECK_EQ(eval_expression("last([1, 2])").to_string(), std::string("2"));
    CHECK_EQ(eval_expression("count([1, 2, 1], 1)").to_string(), std::string("2"));
    CHECK_EQ(eval_expression("count([1, 1.0, true], 1)").to_string(), std::string("3"));
    CHECK_EQ(eval_expression("count([[1], [2]], [1])").to_string(), std::string("1"));
    CHECK_EQ(eval_expression("count([], 1)").to_string(), std::string("0"));
    CHECK_EQ(eval_expression("contains([1, 2.0], 2)").to_string(), std::string("true"));
    CHECK_EQ(eval_expression("contains([[1], 2], [1])").to_string(), std::string("true"));
    CHECK_EQ(eval_expression("title(\"hello WORLD! it's 2fast\")").to_string(),
             std::string("Hello World! It'S 2Fast"));
    CHECK_EQ(eval_expression("title(\"  48ALPHA beta\")").to_string(), std::string("  48Alpha Beta"));
}

TEST_CASE(list_and_string_access_helpers_report_empty_and_type_errors) {
    CHECK_TRUE(expression_error("first([])").contains("first() requires a non-empty list"));
    CHECK_TRUE(expression_error("last([])").contains("last() requires a non-empty list"));
    CHECK_TRUE(expression_error("first(\"abc\")").contains("first() expects a list"));
    CHECK_TRUE(expression_error("count(\"abc\", \"a\")").contains("count() expects a list"));
    CHECK_TRUE(expression_error("reverse(42)").contains("reverse() expects a list or a string"));
    CHECK_TRUE(expression_error("title(42)").contains("title() expects a string"));
    CHECK_TRUE(expression_error("contains(42, 2)").contains("contains() expects a string, got"));
}

TEST_CASE(compound_set_applies_every_arithmetic_operator) {
    const auto result = run_statements(R"LOOM(
        set n = 10
        set n += 5
        set after_add = n
        set n -= 3
        set after_subtract = n
        set n *= 2
        set after_multiply = n
        set n /= 4
        set after_divide = n
        set n %= 3
        set after_modulo = n
        set k = 7
        set k %= 4
        set int_modulo = k
    )LOOM");
    CHECK_TRUE(result.error.empty());
    CHECK_EQ(result.variables.at("after_add").to_string(), std::string("15"));
    CHECK_EQ(result.variables.at("after_subtract").to_string(), std::string("12"));
    CHECK_EQ(result.variables.at("after_multiply").to_string(), std::string("24"));
    CHECK_EQ(result.variables.at("after_divide").to_string(), std::string("6.0"));
    CHECK_EQ(result.variables.at("after_modulo").to_string(), std::string("0.0"));
    CHECK_EQ(result.variables.at("int_modulo").to_string(), std::string("3"));
}

TEST_CASE(compound_set_concatenates_strings_and_lists) {
    const auto result = run_statements(R"LOOM(
        set text = "hello"
        set text += " world"
        set items = [1, 2]
        set items += [3]
        set original = [1, 2]
        set alias = original
        set original += [3]
    )LOOM");
    CHECK_TRUE(result.error.empty());
    CHECK_EQ(result.variables.at("text").to_string(), std::string("hello world"));
    CHECK_EQ(result.variables.at("items").to_string(), std::string("[1, 2, 3]"));
    // Like `items = items + [3]`, `+=` builds a new list instead of mutating
    // the shared storage of the previous value.
    CHECK_EQ(result.variables.at("original").to_string(), std::string("[1, 2, 3]"));
    CHECK_EQ(result.variables.at("alias").to_string(), std::string("[1, 2]"));
}

TEST_CASE(compound_set_keeps_expression_errors_and_undefined_variables) {
    const auto undefined = run_statements("set missing += 1");
    CHECK_TRUE(undefined.error.contains("cannot use compound assignment on undefined variable \"missing\""));

    const auto division = run_statements("set n = 10\nset n /= 0");
    CHECK_TRUE(division.error.contains("Division by zero"));
    const auto modulo = run_statements("set n = 10\nset n %= 0");
    CHECK_TRUE(modulo.error.contains("Division by zero"));
    const auto wrong_type = run_statements("set text = \"a\"\nset text -= \"b\"");
    CHECK_TRUE(wrong_type.error.contains("unsupported operand type(s) for -"));
}

TEST_CASE(plain_set_keeps_its_original_behavior) {
    const auto result = run_statements(R"LOOM(
        set fresh = 1
        set fresh = "text"
        set fresh = [1, 2]
        set fresh = fresh + [3]
    )LOOM");
    CHECK_TRUE(result.error.empty());
    CHECK_EQ(result.variables.at("fresh").to_string(), std::string("[1, 2, 3]"));
}

TEST_CASE(compound_set_uses_the_function_scope) {
    const Script script = parse(R"LOOMWRIGHT(
loom "scope" {
    func bump() {
        set local = 5
        set local += 2
        return local
    }
    when app starts {
        set result = bump()
    }
}
)LOOMWRIGHT");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    CHECK_TRUE(runtime.runtime_error().empty());
    CHECK_EQ(runtime.variables().at("result").to_string(), std::string("7"));
}

TEST_CASE(compound_set_accepts_a_waiting_rhs) {
    const Script script = parse(R"LOOMWRIGHT(
loom "waiting-rhs" {
    func delayed_value() {
        wait 1ms
        return 5
    }
    when app starts {
        set n = 10
        set n += delayed_value()
    }
}
)LOOMWRIGHT");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    CHECK_TRUE(runtime.runtime_error().empty());
    CHECK_EQ(runtime.variables().at("n").to_string(), std::string("15"));
}

TEST_CASE(evaluates_ternary_truthiness_and_pure_path) {
    CHECK_EQ(eval_expression("true ? 1 : 2").to_string(), std::string("1"));
    CHECK_EQ(eval_expression("false ? 1 : 2").to_string(), std::string("2"));
    CHECK_EQ(eval_expression("0 ? 1 : 2").to_string(), std::string("2"));
    CHECK_EQ(eval_expression("0.0 ? 1 : 2").to_string(), std::string("2"));
    CHECK_EQ(eval_expression("\"\" ? 1 : 2").to_string(), std::string("2"));
    CHECK_EQ(eval_expression("[] ? 1 : 2").to_string(), std::string("2"));  // an empty list is falsy
    CHECK_EQ(eval_expression("[0] ? 1 : 2").to_string(), std::string("1"));
    CHECK_EQ(eval_expression("1 or 0 ? 2 : 3").to_string(), std::string("2"));
    CHECK_EQ(eval_expression("0 and 1 ? 2 : 3").to_string(), std::string("3"));
    CHECK_EQ(eval_expression("1 ? 2 : 3 ? 4 : 5").to_string(), std::string("2"));
    CHECK_EQ(eval_expression("0 ? 2 : 3 ? 4 : 5").to_string(), std::string("4"));
    CHECK_EQ(eval_expression("1 ? 0 ? 4 : 5 : 6").to_string(), std::string("5"));
    CHECK_EQ(eval_expression("1 + 2 * 3 > 6 ? \"big\" : \"small\"").to_string(), std::string("big"));
    CHECK_EQ(eval_expression("len([1, 2]) ? \"yes\" : \"no\"").to_string(), std::string("yes"));
}

TEST_CASE(ternary_uses_value_truthiness_for_no_value) {
    const Script script = parse(R"LOOMWRIGHT(
loom "ternary-truth" {
    func nothing() {
        return
    }
    when app starts {
        set no_value = nothing()
        set from_none = no_value ? 1 : 2
        set from_empty = "" ? 1 : 2
        set from_empty_list = [] ? 1 : 2
    }
}
)LOOMWRIGHT");
    const auto result = run_parsed(script);
    CHECK_TRUE(result.error.empty());
    CHECK_EQ(result.variables.at("from_none").to_string(), std::string("2"));
    CHECK_EQ(result.variables.at("from_empty").to_string(), std::string("2"));
    CHECK_EQ(result.variables.at("from_empty_list").to_string(), std::string("2"));
}

TEST_CASE(empty_list_is_falsy_in_conditions_and_boolean_operators) {
    CHECK_EQ(eval_expression("bool([])").to_string(), std::string("false"));
    CHECK_EQ(eval_expression("bool([1])").to_string(), std::string("true"));
    CHECK_EQ(eval_expression("not []").to_string(), std::string("true"));
    CHECK_EQ(eval_expression("[] ? \"then\" : \"else\"").to_string(), std::string("else"));
    CHECK_EQ(eval_expression("[1] ? \"then\" : \"else\"").to_string(), std::string("then"));
    // A falsy list short-circuits `and` and lets `or` evaluate its right arm.
    CHECK_EQ(eval_expression("[] and 1 % 0").to_string(), std::string("false"));
    CHECK_EQ(eval_expression("[] or 7").to_string(), std::string("true"));

    const Script script = parse(R"LOOMWRIGHT(
loom "empty-list-falsy" {
    func nothing() {
        return
    }
    when app starts {
        if [] {
            set branch = "then"
        } else {
            set branch = "else"
        }
        set no_value = nothing()
        set none_bool = bool(no_value)
        set none_ternary = no_value ? "then" : "else"
    }
}
)LOOMWRIGHT");
    const auto result = run_parsed(script);
    CHECK_TRUE(result.error.empty());
    CHECK_EQ(result.variables.at("branch").to_string(), std::string("else"));
    CHECK_EQ(result.variables.at("none_bool").to_string(), std::string("false"));
    CHECK_EQ(result.variables.at("none_ternary").to_string(), std::string("else"));
}

TEST_CASE(ternary_is_lazy_and_keeps_branch_errors_identical) {
    const Script skipped_else = parse(R"LOOMWRIGHT(
loom "lazy-else" {
    func boom() {
        return 1 % 0
    }
    when app starts {
        set result = true ? 5 : boom()
    }
}
)LOOMWRIGHT");
    const auto else_result = run_parsed(skipped_else);
    CHECK_TRUE(else_result.error.empty());
    CHECK_EQ(else_result.variables.at("result").to_string(), std::string("5"));

    const Script skipped_then = parse(R"LOOMWRIGHT(
loom "lazy-then" {
    func boom() {
        return 1 % 0
    }
    when app starts {
        set result = false ? boom() : 6
    }
}
)LOOMWRIGHT");
    const auto then_result = run_parsed(skipped_then);
    CHECK_TRUE(then_result.error.empty());
    CHECK_EQ(then_result.variables.at("result").to_string(), std::string("6"));

    CHECK_TRUE(run_statements("set result = false ? 7 : 1 % 0").error.contains("Division by zero"));
    CHECK_TRUE(run_statements("set result = true ? 1 % 0 : 7").error.contains("Division by zero"));
}

TEST_CASE(ternary_skips_unselected_side_effects) {
    const Script script = parse(R"LOOMWRIGHT(
loom "lazy-side-effects" {
    func add() {
        call append(items, 9)
        return 0
    }
    when app starts {
        set items = [1]
        set kept = true ? 1 : add()
        set changed = false ? 2 : add()
    }
}
)LOOMWRIGHT");
    const auto result = run_parsed(script);
    CHECK_TRUE(result.error.empty());
    CHECK_EQ(result.variables.at("kept").to_string(), std::string("1"));
    CHECK_EQ(result.variables.at("changed").to_string(), std::string("0"));
    CHECK_EQ(result.variables.at("items").to_string(), std::string("[1, 9]"));
}

TEST_CASE(ternary_evaluates_only_the_selected_waiting_branch) {
    const Script script = parse(R"LOOMWRIGHT(
loom "lazy-wait" {
    func delayed() {
        wait 1ms
        return 7
    }
    when app starts {
        set chosen = true ? delayed() : 1
        set other = false ? delayed() : 2
        set waiting_condition = delayed() ? 3 : 4
    }
}
)LOOMWRIGHT");
    const auto result = run_parsed(script);
    CHECK_TRUE(result.error.empty());
    CHECK_EQ(result.variables.at("chosen").to_string(), std::string("7"));
    CHECK_EQ(result.variables.at("other").to_string(), std::string("2"));
    CHECK_EQ(result.variables.at("waiting_condition").to_string(), std::string("3"));
}

TEST_CASE(ternary_works_in_set_log_if_and_arguments) {
    const Script script = parse(R"LOOMWRIGHT(
loom "ternary-contexts" {
    when app starts {
        set items = [10, 20]
        set picked = len(items) > 1 ? first(items) : last(items)
        log (picked == 10 ? "first" : "last")
        if (false ? 1 : 0) {
            set branch = "then"
        } else {
            set branch = "else"
        }
        set nested = str(true ? 1 : 2)
    }
}
)LOOMWRIGHT");
    StatementResult result;
    const auto output = loomwright_test::capture_output([&]() {
        result = run_parsed(script);
        return 0;
    });
    CHECK_TRUE(result.error.empty());
    CHECK_TRUE(output.out.contains("first"));
    CHECK_EQ(result.variables.at("picked").to_string(), std::string("10"));
    CHECK_EQ(result.variables.at("branch").to_string(), std::string("else"));
    CHECK_EQ(result.variables.at("nested").to_string(), std::string("1"));
}

TEST_CASE(lists_are_mutable_and_compare_by_value) {
    const Script script = parse(R"LOOMWRIGHT(
loom "lists" {
    when app starts {
        set items = ["b", "a"]
        call append(items, "c")
        call sort(items)
        set joined = join(items, "")
        set same = items == ["a", "b", "c"]
    }
}
)LOOMWRIGHT");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().at("joined").to_string(), std::string("abc"));
    CHECK_EQ(runtime.variables().at("same").to_string(), std::string("true"));
}

TEST_CASE(for_loops_iterate_ranges_and_lists) {
    const Script script = parse(R"LOOMWRIGHT(
loom "loops" {
    when app starts {
        set total = 0
        for i = 1 to 4 {
            set total = total + i
        }
        set down = ""
        for i = 3 downto 1 {
            set down = down + str(i)
        }
        set stepped = ""
        for i = 0 to 6 step 3 {
            set stepped = stepped + str(i)
        }
        set items = ["x", "y"]
        set seen = ""
        for item in items {
            set seen = seen + item
        }
    }
}
)LOOMWRIGHT");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().at("total").to_string(), std::string("10"));
    CHECK_EQ(runtime.variables().at("down").to_string(), std::string("321"));
    CHECK_EQ(runtime.variables().at("stepped").to_string(), std::string("036"));
    CHECK_EQ(runtime.variables().at("seen").to_string(), std::string("xy"));
}

TEST_CASE(break_and_continue_control_loops) {
    const Script script = parse(R"LOOMWRIGHT(
loom "control" {
    when app starts {
        set visited = ""
        for i = 1 to 10 {
            if i == 2 {
                continue
            }
            if i > 4 {
                break
            }
            set visited = visited + str(i)
        }
        set count = 0
        while true {
            set count = count + 1
            if count == 3 {
                break
            }
        }
    }
}
)LOOMWRIGHT");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().at("visited").to_string(), std::string("134"));
    CHECK_EQ(runtime.variables().at("count").to_string(), std::string("3"));
}

TEST_CASE(user_functions_return_values_and_restore_their_scope) {
    const Script script = parse(R"LOOMWRIGHT(
loom "functions" {
    func double(x) {
        return x * 2
    }
    func shout(text) {
        return upper(text)
    }
    when app starts {
        set value = double(21)
        set loud = shout("hey")
        set shadow = 5
        call double(shadow)
        set after = shadow
    }
}
)LOOMWRIGHT");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().at("value").to_string(), std::string("42"));
    CHECK_EQ(runtime.variables().at("loud").to_string(), std::string("HEY"));
    CHECK_EQ(runtime.variables().at("after").to_string(), std::string("5"));
}

TEST_CASE(single_return_keeps_ready_order_and_wait_still_yields) {
    const Script script = parse(R"LOOMWRIGHT(
loom "return order" {
    func direct(x) {
        return x + event_key
    }
    func delayed(x) {
        wait 0ms
        return x
    }
    when key A goes down {
        type direct("first ")
        type delayed("last")
    }
    when key B goes down {
        type "other"
    }
}
)LOOMWRIGHT");
    Runtime runtime(script);
    auto backend = std::make_unique<RecordingBackend>();
    auto* recorder = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0), "A", "down"));
    runtime.scheduler().spawn(runtime.run_event(script.events.at(1), "B", "down"));
    runtime.scheduler().run();
    CHECK_EQ(recorder->log, std::vector<std::string>({"type first A", "type other", "type last"}));
}

TEST_CASE(single_return_reports_expression_errors) {
    const Script script = parse(R"LOOMWRIGHT(
loom "return error" {
    func fail(x) {
        return x % 0
    }
    when app starts {
        set result = fail(10)
    }
}
)LOOMWRIGHT");
    Runtime runtime(script);
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    CHECK_TRUE(runtime.runtime_error().contains("Division by zero"));
    CHECK_TRUE(runtime.scheduler().stop_requested());
    CHECK_TRUE(!runtime.variables().contains("result"));
}

TEST_CASE(cancelling_a_waiting_function_does_not_run_its_return) {
    const Script script = parse(R"LOOMWRIGHT(
loom "cancel return" {
    func delayed(x) {
        wait 1s
        return x
    }
    when app starts {
        set result = delayed("unreachable")
    }
}
)LOOMWRIGHT");
    Runtime runtime(script);
    runtime.set_backend(std::make_unique<RecordingBackend>());
    auto task = runtime.run_event(script.events.at(0));
    runtime.scheduler().spawn(task);
    runtime.scheduler().run_ready();
    CHECK_TRUE(runtime.scheduler().is_live(task.handle));
    runtime.scheduler().cancel(task.handle);
    runtime.scheduler().run_ready();
    CHECK_EQ(runtime.scheduler().live_count(), std::size_t{0});
    CHECK_TRUE(!runtime.variables().contains("result"));
    CHECK_TRUE(runtime.runtime_error().empty());
}

TEST_CASE(backend_stop_inside_a_function_skips_its_return) {
    const Script script = parse(R"LOOMWRIGHT(
loom "backend stop" {
    func direct(x) {
        return x * 2
    }
    func stop() {
        type "stop"
        return unknown
    }
    when app starts {
        set first = direct(21)
        set stopped = stop()
        type "unreachable"
    }
}
)LOOMWRIGHT");
    struct StopBackend : RecordingBackend {
        explicit StopBackend(Scheduler& owner) : scheduler(owner) {}
        void type_text(const std::string& text) override {
            RecordingBackend::type_text(text);
            scheduler.request_stop();
        }
        Scheduler& scheduler;
    };
    Runtime runtime(script);
    auto backend = std::make_unique<StopBackend>(runtime.scheduler());
    auto* recorder = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().at("first").as_int(), 42);
    CHECK_TRUE(runtime.variables().at("stopped").is_none());
    CHECK_TRUE(runtime.runtime_error().empty());
    CHECK_EQ(recorder->log, std::vector<std::string>({"type stop"}));
}

TEST_CASE(user_functions_can_call_each_other) {
    const Script script = parse(R"LOOMWRIGHT(
loom "recursion" {
    func countdown(n) {
        if n <= 0 {
            return 0
        }
        return countdown(n - 1) + 1
    }
    when app starts {
        set depth = countdown(4)
    }
}
)LOOMWRIGHT");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().at("depth").to_string(), std::string("4"));
}

TEST_CASE(runaway_recursion_is_stopped) {
    const Script script = parse(R"LOOMWRIGHT(
loom "runaway" {
    func loop_forever(n) {
        return loop_forever(n + 1)
    }
    when app starts {
        set value = loop_forever(0)
    }
}
)LOOMWRIGHT");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    CHECK_TRUE(runtime.runtime_error().contains("Maximum function call depth"));
}

TEST_CASE(chord_events_need_their_modifiers) {
    const Script script = parse(R"LOOMWRIGHT(
loom "chords" {
    when key CTRL+ALT+T goes down {
        set fired = 1
    }
}
)LOOMWRIGHT");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());

    // Without the modifiers the chord stays quiet.
    runtime.physical_key("fake-keyboard", "T", true);
    runtime.physical_key("fake-keyboard", "T", false);
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().count("fired"), static_cast<size_t>(0));

    // With both modifiers held the chord fires.
    runtime.physical_key("fake-keyboard", "LEFTCTRL", true);
    runtime.physical_key("fake-keyboard", "LEFTALT", true);
    runtime.physical_key("fake-keyboard", "T", true);
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().count("fired"), static_cast<size_t>(1));

    // Releasing the primary key does not fire the chord again.
    runtime.physical_key("fake-keyboard", "T", false);
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().count("fired"), static_cast<size_t>(1));
}

TEST_CASE(chord_bindings_can_be_disabled_by_name) {
    const Script script = parse(R"LOOMWRIGHT(
loom "chords" {
    when key CTRL+ALT+T goes down {
        set fired = 1
    }
    when key F9 goes down {
        call set_hotkey_enabled("CTRL+ALT+T", false)
    }
}
)LOOMWRIGHT");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.physical_key("fake-keyboard", "F9", true);
    runtime.scheduler().run();
    runtime.physical_key("fake-keyboard", "LEFTCTRL", true);
    runtime.physical_key("fake-keyboard", "LEFTALT", true);
    runtime.physical_key("fake-keyboard", "T", true);
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().count("fired"), static_cast<size_t>(0));
}

TEST_CASE(function_assignments_stay_inside_the_function) {
    const Script script = parse(R"LOOMWRIGHT(
loom "scope" {
    func helper(x) {
        set doubled = x * 2
        return doubled
    }
    func reader() {
        return counter * 2
    }
    when app starts {
        set counter = 5
        set value = helper(21)
        set doubled = reader()
    }
}
)LOOMWRIGHT");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().at("value").to_string(), std::string("42"));
    CHECK_EQ(runtime.variables().at("doubled").to_string(), std::string("10"));
    // The helper's own variable never reaches the script scope.
    CHECK_EQ(runtime.variables().count("doubled"), static_cast<size_t>(1));
    CHECK_EQ(runtime.variables().count("x"), static_cast<size_t>(0));
}

TEST_CASE(suspending_hotkeys_keeps_the_chord_binding_enabled) {
    const Script script = parse(R"LOOMWRIGHT(
loom "chords" {
    when key CTRL+ALT+T goes down {
        call suspend_hotkeys(true)
        set fired = 1
    }
    when key F9 goes down {
        set other = 1
    }
}
)LOOMWRIGHT");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.physical_key("fake-keyboard", "LEFTCTRL", true);
    runtime.physical_key("fake-keyboard", "LEFTALT", true);
    runtime.physical_key("fake-keyboard", "T", true);
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().count("fired"), static_cast<size_t>(1));

    // Other bindings are suspended, the chord that suspended them is not.
    runtime.physical_key("fake-keyboard", "F9", true);
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().count("other"), static_cast<size_t>(0));

    runtime.physical_key("fake-keyboard", "T", false);
    runtime.physical_key("fake-keyboard", "LEFTALT", false);
    runtime.physical_key("fake-keyboard", "LEFTCTRL", false);
    runtime.physical_key("fake-keyboard", "T", true);
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().count("fired"), static_cast<size_t>(1));
}

TEST_CASE(chord_release_events_fire_when_the_primary_key_is_released) {
    const Script script = parse(R"LOOMWRIGHT(
loom "chords" {
    when key CTRL+ALT+T goes up {
        set released = 1
    }
}
)LOOMWRIGHT");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.physical_key("fake-keyboard", "LEFTCTRL", true);
    runtime.physical_key("fake-keyboard", "LEFTALT", true);
    runtime.physical_key("fake-keyboard", "T", true);
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().count("released"), static_cast<size_t>(0));

    // The release fires even when the modifiers went up first.
    runtime.physical_key("fake-keyboard", "LEFTALT", false);
    runtime.physical_key("fake-keyboard", "T", false);
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().count("released"), static_cast<size_t>(1));
}
