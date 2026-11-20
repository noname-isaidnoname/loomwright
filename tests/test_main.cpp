#include "harness.hpp"

#include <cstdio>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace loomwright_test {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

void fail(const char* file, int line, std::string_view message) {
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) + ": " + std::string(message));
}

void fail_equal(const char* file, int line, std::string_view message,
                const std::string& actual, const std::string& expected) {
    fail(file, line, std::string(message) + "\n  actual:   " + actual + "\n  expected: " + expected);
}

CapturedOutput capture_output(const std::function<int()>& fn) {
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        throw std::runtime_error("capture_output: pipe failed");
    }
    const int saved_out = dup(STDOUT_FILENO);
    const int saved_err = dup(STDERR_FILENO);
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);
    close(out_pipe[1]);
    close(err_pipe[1]);

    CapturedOutput result;
    try {
        result.code = fn();
    } catch (...) {
        dup2(saved_out, STDOUT_FILENO);
        dup2(saved_err, STDERR_FILENO);
        close(saved_out);
        close(saved_err);
        close(out_pipe[0]);
        close(err_pipe[0]);
        throw;
    }
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
    dup2(saved_out, STDOUT_FILENO);
    dup2(saved_err, STDERR_FILENO);
    close(saved_out);
    close(saved_err);

    const auto drain = [](int fd) {
        std::string text;
        char buffer[4096];
        ssize_t count = 0;
        while ((count = read(fd, buffer, sizeof(buffer))) > 0) text.append(buffer, static_cast<size_t>(count));
        close(fd);
        return text;
    };
    result.out = drain(out_pipe[0]);
    result.err = drain(err_pipe[0]);
    return result;
}

int run_all(const std::string& filter) {
    int failures = 0;
    size_t executed = 0;
    for (const auto& test : registry()) {
        if (!filter.empty() && !test.name.contains(filter)) continue;
        ++executed;
        try {
            test.function();
            std::cout << "ok   " << test.name << "\n";
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "FAIL " << test.name << "\n     " << error.what() << "\n";
        } catch (...) {
            ++failures;
            std::cout << "FAIL " << test.name << "\n     unknown exception\n";
        }
    }
    std::cout << (executed - static_cast<size_t>(failures)) << "/" << executed << " tests passed\n";
    return failures == 0 ? 0 : 1;
}

}  // namespace loomwright_test

int main(int argc, char** argv) {
    const std::string filter = argc > 1 ? argv[1] : std::string();
    return loomwright_test::run_all(filter);
}
