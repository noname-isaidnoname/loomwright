#include "input_broker.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "input.hpp"
#include "input_broker_protocol.hpp"
#include "parser.hpp"
#include "runtime.hpp"
#include "runtime_fixtures.hpp"
#include "scheduler.hpp"

namespace {

using namespace loomwright;
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

/// Records the broker's real injections on a pipe inherited across fork.
class PipeBackend : public OutputBackend {
public:
    explicit PipeBackend(int fd) : fd_(fd) {}

    void warp(int, int) override {}
    void move(int, int) override {}
    void scroll(int, int) override {}
    void type_text(const std::string&) override {}
    void close() override {}

    void key(const std::string& name, bool down) override { emit("key " + name + (down ? " down" : " up")); }
    void button(const std::string& name, bool down) override {
        emit("button " + name + (down ? " down" : " up"));
    }
    const char* name() const override { return "PipeBackend"; }

private:
    void emit(const std::string& line) const {
        const std::string text = line + "\n";
        size_t sent = 0;
        while (sent < text.size()) {
            const ssize_t count = ::write(fd_, text.data() + sent, text.size() - sent);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return;
            sent += static_cast<size_t>(count);
        }
    }

    int fd_;
};

bool read_line(int fd, std::string& line, int timeout_ms) {
    pollfd item{fd, POLLIN, 0};
    if (::poll(&item, 1, timeout_ms) <= 0) return false;
    std::string text;
    while (true) {
        char byte = 0;
        const ssize_t count = ::read(fd, &byte, 1);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        if (byte == '\n') {
            line = text;
            return true;
        }
        text += byte;
    }
}

bool wait_owners(InputClient& client, OutputKind kind, const std::string& name, int owners) {
    const auto deadline = std::chrono::steady_clock::now() + 1000ms;
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd item{client.fd(), POLLIN, 0};
        if (::poll(&item, 1, 10) > 0) (void)client.receive();
        if (client.output_owners(kind, name) == owners) return true;
    }
    return false;
}

struct Fixture {
    char directory[256] = "/tmp/loomwright-output-XXXXXX";
    int pipe_fds[2] = {-1, -1};
    pid_t broker = -1;

    Fixture() {
        require(::mkdtemp(directory) != nullptr, "could not create private runtime directory");
        require(::setenv("XDG_RUNTIME_DIR", directory, 1) == 0, "could not set runtime directory");
        require(::pipe(pipe_fds) == 0, "could not create output log pipe");
    }

    ~Fixture() {
        if (broker > 0) {
            ::kill(broker, SIGTERM);
            int status = 0;
            ::waitpid(broker, &status, 0);
        }
        for (int fd : pipe_fds) if (fd >= 0) ::close(fd);
        const std::string root = directory;
        for (const char* suffix : {"/loomwright-input.sock", "/loomwright-input.sock.lock"})
            ::unlink((root + suffix).c_str());
        ::rmdir(directory);
    }

    std::string absent_device() const { return std::string(directory) + "/no-input"; }
};

Task stop_after(double seconds, Runtime& runtime) {
    co_await sleep_for(seconds);
    runtime.scheduler().request_stop();
}

int run() {
    Fixture fixture;
    const auto is_keyboard = [](const InputDevice&) { return false; };
    const auto output = [fd = fixture.pipe_fds[1]]() -> std::unique_ptr<OutputBackend> {
        return std::make_unique<PipeBackend>(fd);
    };
    fixture.broker = ::fork();
    require(fixture.broker >= 0, "fork failed");
    if (fixture.broker == 0) {
        ::close(fixture.pipe_fds[0]);
        _exit(run_input_broker(is_keyboard, output));
    }
    ::close(fixture.pipe_fds[1]);
    fixture.pipe_fds[1] = -1;
    bool running = false;
    for (int i = 0; i < 200 && !running; ++i) {
        running = input_broker_running();
        if (!running) ::poll(nullptr, 0, 10);
    }
    require(running, "broker did not start");

    BrokerRequest request;
    request.name = "first script";
    request.device = fixture.absent_device();
    auto first = std::make_unique<InputClient>(request);
    BrokerRequest second_request = request;
    second_request.name = "second script";
    auto second = std::make_unique<InputClient>(second_request);
    std::string line;

    // Two scripts claim one key. Only the first down and the last up are
    // injected, and both clients see the intermediate owner counts.
    first->claim_output(OutputKind::Key, "A", true);
    second->claim_output(OutputKind::Key, "A", true);
    require(wait_owners(*first, OutputKind::Key, "A", 2), "first client missed the overlapping claim");
    require(wait_owners(*second, OutputKind::Key, "A", 2), "second client missed the overlapping claim");

    // The read-only status query sees both script names and their shared hold.
    const auto status = read_broker_status();
    require(status.has_value(), "broker status was unavailable");
    require(status->protocol == broker::kProtocol, "status reported the wrong protocol version");
    require(status->clients.size() == 2, "status missed a connected script");
    require(std::any_of(status->clients.begin(), status->clients.end(),
                        [](const BrokerStatusClient& client) { return client.name == "first script"; }),
            "status lost the first script's name");
    require(status->claims.size() == 1 && status->claims[0].name == "A" &&
                status->claims[0].owners.size() == 2,
            "status lost the shared key claim");
    const auto conflicts = broker_conflicts(*status);
    require(conflicts.size() == 1 && conflicts[0].kind == "hold" && conflicts[0].key == "key A",
            "status did not surface the overlapping hold");
    require(format_broker_status(*status).contains("\"second script\""),
            "formatted status did not name the second script");

    require(read_line(fixture.pipe_fds[0], line, 1000) && line == "key A down", "missing initial key injection");
    require(!read_line(fixture.pipe_fds[0], line, 100), "overlapping claim injected a duplicate key down");
    first->claim_output(OutputKind::Key, "A", false);
    require(wait_owners(*second, OutputKind::Key, "A", 1), "release did not reach the remaining owner");
    require(!read_line(fixture.pipe_fds[0], line, 100), "early release injected a duplicate key up");
    second->claim_output(OutputKind::Key, "A", false);
    require(wait_owners(*second, OutputKind::Key, "A", 0), "final release was not broadcast");
    require(read_line(fixture.pipe_fds[0], line, 1000) && line == "key A up", "missing final key release");

    // Buttons follow the same rule, and a client that dies while holding one
    // must not leave the other client's claim short or a button stuck down.
    first->claim_output(OutputKind::Button, "LEFT", true);
    second->claim_output(OutputKind::Button, "LEFT", true);
    require(wait_owners(*first, OutputKind::Button, "LEFT", 2), "button owners were not shared");
    require(read_line(fixture.pipe_fds[0], line, 1000) && line == "button LEFT down", "missing button injection");
    second.reset();
    require(wait_owners(*first, OutputKind::Button, "LEFT", 1), "client death did not drop its button claim");
    require(!read_line(fixture.pipe_fds[0], line, 100), "client death released another client's button");
    first->claim_output(OutputKind::Button, "LEFT", false);
    require(read_line(fixture.pipe_fds[0], line, 1000) && line == "button LEFT up", "missing button release");

    // A script joining while another holds a key starts with the shared state.
    first->claim_output(OutputKind::Key, "X", true);
    require(read_line(fixture.pipe_fds[0], line, 1000) && line == "key X down", "missing held key injection");
    auto late = std::make_unique<InputClient>(request);
    require(late->output_held(OutputKind::Key, "X"), "registration snapshot lost the held key");
    first.reset();
    require(wait_owners(*late, OutputKind::Key, "X", 0), "last owner's death did not release the key");
    require(read_line(fixture.pipe_fds[0], line, 1000) && line == "key X up", "missing release after client death");
    late.reset();

    // Drive an actual Runtime: `key press` in shared mode must reach the
    // broker's backend, not the script's own backend.
    auto script = parse("loom \"shared output\" {\n"
                        "    when app starts {\n"
                        "        key A press\n"
                        "        wait 0.05\n"
                        "        key A free\n"
                        "    }\n"
                        "}\n");
    Runtime::Options options;
    options.shared_input = true;
    options.input_device = fixture.absent_device();
    Runtime runtime(script, options);
    auto local = std::make_unique<loomwright_test::RecordingBackend>();
    const auto* recording = local.get();
    runtime.set_backend(std::move(local));
    runtime.scheduler().spawn(runtime.run());
    runtime.scheduler().spawn(stop_after(0.3, runtime));
    runtime.scheduler().run();
    if (runtime.failure()) std::rethrow_exception(runtime.failure());
    require(std::none_of(recording->log.begin(), recording->log.end(),
                         [](const std::string& entry) { return entry.starts_with("key "); }),
            "a shared key was injected by the local backend");
    require(read_line(fixture.pipe_fds[0], line, 1000) && line == "key A down", "runtime did not claim the key");
    require(read_line(fixture.pipe_fds[0], line, 1000) && line == "key A up", "runtime did not release the key");

    require(!read_line(fixture.pipe_fds[0], line, 100), "unexpected extra injection");
    std::cout << "ok   overlapping synthetic claims inject once and share held state\n";
    return 0;
}

}  // namespace

int main() {
    try {
        return run();
    } catch (const std::exception& error) {
        std::cerr << "shared output broker: " << error.what() << '\n';
        return 1;
    }
}
