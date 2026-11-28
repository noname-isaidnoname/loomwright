#include "input.hpp"
#include "input_broker.hpp"
#include "keys.hpp"
#include "subprocess.hpp"
#include "uinput.hpp"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <linux/input.h>
#include <linux/uinput.h>
#include <memory>
#include <poll.h>
#include <set>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace loomwright;

namespace {

// A lone BTN_0 is not a keyboard, mouse, or gamepad. We also
// require udev to confirm that it has no input-device classification before
// emitting anything. All writes happen while a test reader owns EVIOCGRAB.
constexpr int kTestCode = BTN_0;

std::string event_path(int fd) {
    char sysname[64]{};
    if (::ioctl(fd, UI_GET_SYSNAME(sizeof(sysname)), sysname) < 0) return {};
    const std::filesystem::path directory = std::filesystem::path("/sys/devices/virtual/input") / sysname;
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
            const std::string name = entry.path().filename().string();
            if (name.rfind("event", 0) == 0 && ::access(("/dev/input/" + name).c_str(), R_OK) == 0)
                return "/dev/input/" + name;
        }
        ::poll(nullptr, 0, 10);
    }
    return {};
}

bool unclassified(const std::string& path) {
    if (which("udevadm").empty()) return false;
    const std::string name = std::filesystem::path(path).filename().string();
    const auto result = run_command({"udevadm", "info", "--query=property", "--path=/sys/class/input/" + name}, "", 3000);
    if (result.returncode != 0) return false;
    bool input = false;
    std::istringstream lines(result.out);
    for (std::string line; std::getline(lines, line);) {
        if (line == "ID_INPUT=1") input = true;
        if (line.rfind("ID_INPUT_", 0) == 0 && line.size() >= 2 && line.substr(line.size() - 2) == "=1") {
            std::cout << "skip: test device classified as " << line << '\n';
            return false;
        }
    }
    return input;
}

bool saw_value(int fd, int expected) {
    pollfd descriptor{fd, POLLIN, 0};
    if (::poll(&descriptor, 1, 1000) <= 0) return false;
    input_event events[32]{};
    const ssize_t bytes = ::read(fd, events, sizeof(events));
    if (bytes <= 0) return false;
    for (size_t i = 0; i < static_cast<size_t>(bytes) / sizeof(input_event); ++i)
        if (events[i].type == EV_KEY && events[i].code == kTestCode && events[i].value == expected) return true;
    return false;
}

void emit(UInputDevice& device, int value) {
    device.write(EV_KEY, kTestCode, value);
    device.write(EV_SYN, SYN_REPORT, 0);
    device.flush();
}

struct Safety {
    UInputDevice& writer;
    InputDevice& observer;
    InputDevice& target;
    bool observer_grabbed = false;
    bool target_grabbed = false;
    bool down = false;
    ~Safety() {
        if (down) {
            try {
                if (!observer_grabbed && !target_grabbed) { observer.grab(); observer_grabbed = true; }
                // Never emit a cleanup event unless a test reader owns the grab.
                if (observer_grabbed || target_grabbed) emit(writer, 0);
            } catch (...) {}
        }
        target.ungrab();
        observer.ungrab();
    }
};

int shared_broker() {
    const auto must = [](bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    };
    const auto skip = [](const char* message) {
        std::cout << "skip: " << message << '\n';
        return 77;
    };
    constexpr int a = BTN_0, b = BTN_1, c = BTN_2;
    if (::access("/dev/uinput", W_OK) != 0 || ::access("/dev/input", R_OK) != 0)
        return skip("uinput or evdev access unavailable");
    UInputDevice writer("Loomwright isolated broker test", "loomwright/test/broker", {a, b, c}, {}, {});
    const auto path = event_path(writer.fd());
    if (path.empty() || !unclassified(path)) return skip("physical test device not confirmed isolated");
    const auto scope = "/loomwright-test-" + std::to_string(::getpid());
    must(::setenv("XDG_RUNTIME_DIR", scope.c_str(), 1) == 0, "could not set test socket scope");
    const pid_t pid = ::fork();
    must(pid >= 0, "fork failed");
    if (pid == 0) {
        ::close(writer.fd());
        _exit(run_input_broker([&](const InputDevice& input) { return input.path() == path; }));
    }
    struct Child {
        pid_t pid;
        int stop(int signal) {
            ::kill(pid, signal);
            int status = 0;
            pid_t result;
            do { result = ::waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
            pid = -1;
            if (result < 0) return -1;
            return status;
        }
        ~Child() { if (pid > 0) stop(SIGKILL); }
    } child{pid};
    for (int i = 0; i < 100 && !input_broker_running(); ++i) ::poll(nullptr, 0, 10);
    must(input_broker_running(), "broker did not start");

    BrokerRequest request;
    request.device = path;
    request.require_keyboard = true;
    auto observer = std::make_unique<InputClient>(request);
    const auto identity = [](const std::string& node) {
        std::error_code error;
        return std::filesystem::canonical(
            "/sys/class/input/" + std::filesystem::path(node).filename().string(), error);
    };
    std::set<std::filesystem::path> prior;
    for (const auto& item : list_input_devices()) prior.insert(identity(item));
    request.consume = {a};
    auto first = std::make_unique<InputClient>(request);
    request.consume = {b};
    auto second = std::make_unique<InputClient>(request);
    must(first->devices().size() == 1 && second->devices().at(0).path == path, "bad device registration");

    // No key has been emitted yet. Require udev to confirm that neither end
    // will interact with the real desktop before sending any test event.
    std::unique_ptr<InputDevice> replay;
    for (const auto& item : list_input_devices()) {
        const auto sysfs = identity(item);
        if (sysfs.empty() || prior.contains(sysfs)) continue;
        auto device = InputDevice::open(item);
        if (!device || device->name() != "Loomwright passthrough" ||
            device->capabilities().keys != std::vector<int>{a, b, c}) continue;
        if (!unclassified(item) || replay) return skip("passthrough device not confirmed isolated and unique");
        replay = std::move(device);
    }
    if (!replay) return skip("test passthrough device not found");
    auto physical = InputDevice::open(path);
    must(physical != nullptr, "physical test reader unavailable");

    const auto send_edge = [&](int code, int value) {
        writer.write(EV_KEY, code, value);
        writer.write(EV_SYN, SYN_REPORT, 0);
        writer.flush();
    };
    const auto edge = [&](InputClient& client, int code, int value) {
        pollfd item{client.fd(), POLLIN, 0};
        must(::poll(&item, 1, 1000) > 0, "client did not receive input");
        const auto event = client.receive();
        must(event && event->path == path && event->edges.size() == 1 &&
             event->edges[0].code == code && event->edges[0].value == value, "incorrect client input");
    };
    const auto desktop = [&](InputDevice& device, int code, int value) {
        pollfd item{device.fd(), POLLIN, 0};
        must(::poll(&item, 1, 1000) > 0, "desktop test reader did not receive input");
        input_event events[16];
        const ssize_t bytes = ::read(device.fd(), events, sizeof(events));
        must(bytes > 0, "desktop test read failed");
        int seen = 0;
        for (size_t i = 0; i < static_cast<size_t>(bytes) / sizeof(input_event); ++i) {
            if (events[i].type != EV_KEY) continue;
            must(events[i].code == code && events[i].value == value, "unexpected desktop input");
            ++seen;
        }
        must(seen == 1, "desktop input was missing or duplicated");
    };
    const auto quiet = [&](InputDevice& device) {
        // evdev does not queue empty SYN_REPORT frames.
        pollfd item{device.fd(), POLLIN, 0};
        must(::poll(&item, 1, 0) == 0, "consumed edge leaked to desktop");
    };
    const auto both = [&](int code, int value) {
        edge(*first, code, value);
        edge(*second, code, value);
        edge(*observer, code, value);
    };

    send_edge(a, 1); both(a, 1); quiet(*replay);
    first.reset();
    request.consume.clear();
    auto joined = std::make_unique<InputClient>(request);
    must(joined->devices().at(0).down == std::vector<int>{a}, "join snapshot lost held key");
    send_edge(a, 0);
    edge(*second, a, 0); edge(*observer, a, 0); edge(*joined, a, 0); quiet(*replay);

    for (int value : {1, 0}) {
        send_edge(b, value);
        edge(*second, b, value); edge(*observer, b, value); edge(*joined, b, value); quiet(*replay);
    }
    for (int code : {a, c}) {
        for (int value : {1, 0}) {
            send_edge(code, value);
            edge(*second, code, value); edge(*observer, code, value); edge(*joined, code, value);
            desktop(*replay, code, value);
        }
    }

    // A real panic registration protects the key across clients. Removing
    // that registration while held must not swallow its release.
    request.panic = {b};
    auto panic = std::make_unique<InputClient>(request);
    send_edge(b, 1);
    edge(*second, b, 1); edge(*observer, b, 1); edge(*joined, b, 1); edge(*panic, b, 1);
    desktop(*replay, b, 1);
    panic.reset();
    request.panic.clear();
    auto barrier = std::make_unique<InputClient>(request);
    send_edge(b, 0);
    edge(*second, b, 0); edge(*observer, b, 0); edge(*joined, b, 0); edge(*barrier, b, 0);
    desktop(*replay, b, 0);

    second.reset();
    auto after = std::make_unique<InputClient>(request);
    for (int value : {1, 0}) {
        send_edge(b, value);
        edge(*observer, b, value); edge(*after, b, value);
        desktop(*physical, b, value);
    }
    request.consume = {a};
    auto reacquired = std::make_unique<InputClient>(request);
    const int status = child.stop(SIGKILL);
    must(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "could not kill test broker");
    bool lost = false;
    try { (void)observer->receive(); } catch (const std::exception& error) {
        lost = std::string_view(error.what()).contains("broker disconnected");
    }
    must(lost, "broker death was not noticed");
    physical->grab();
    physical->ungrab();
    must(!input_broker_running(), "dead broker left a listening socket");
    std::cout << "ok   two shared clients consume independently, observers get edges, passthrough and grab recover\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        if (std::string_view(argv[1]) != "--shared-broker") return 1;
        try { return shared_broker(); }
        catch (const std::exception& error) {
            std::cerr << "shared evdev broker: " << error.what() << '\n';
            return 1;
        }
    }
    if (::access("/dev/uinput", W_OK) != 0 || ::access("/dev/input", R_OK) != 0) {
        std::cout << "skip: uinput or evdev access unavailable\n";
        return 77;
    }
    bool isolated = false;
    try {
        UInputDevice writer("Loomwright isolated handoff test", "loomwright/test/handoff", {kTestCode}, {}, {});
        const std::string path = event_path(writer.fd());
        if (path.empty() || !unclassified(path)) {
            std::cout << "skip: test device is not confirmed unclassified and readable (path: " << path << ")\n";
            return 77;
        }
        auto observer = InputDevice::open(path);
        auto target = InputDevice::open(path);
        if (!observer || !target) return 77;
        Safety safety{writer, *observer, *target};
        observer->grab();
        safety.observer_grabbed = true;
        isolated = true;
        bool observer_saw_down = false;
        std::string error;
        try {
            grab_idle_input([&] { return target->pressed_key_codes(); },
                            [&] { return target->discard_pending_events(); },
                            [&] {
                                // The test observer represents the desktop; no
                                // event is emitted while neither reader is grabbed.
                                safety.down = true;
                                emit(writer, 1);
                                observer_saw_down = saw_value(observer->fd(), 1);
                                observer->ungrab();
                                safety.observer_grabbed = false;
                                target->grab();
                                safety.target_grabbed = true;
                                emit(writer, 0);
                                safety.down = false;
                            },
                            [&] { target->ungrab(); safety.target_grabbed = false; });
        } catch (const std::exception& failure) {
            error = failure.what();
        }
        const auto name = normalize_key_event(kTestCode);
        const std::string label = name ? *name : "code " + std::to_string(kTestCode);
        if (!observer_saw_down || !error.contains("release of " + label) ||
            !error.contains("physically tap and release") || safety.target_grabbed) {
            std::cerr << "handoff detection or rollback failed: " << error << '\n';
            return 1;
        }
        pollfd descriptor{observer->fd(), POLLIN, 0};
        if (::poll(&descriptor, 1, 50) > 0 && saw_value(observer->fd(), 0)) {
            std::cerr << "observer unexpectedly received the grabbed release\n";
            return 1;
        }
        if (!target->pressed_key_codes().empty()) {
            std::cerr << "test input remained held after rollback\n";
            return 1;
        }
        std::cout << "ok   isolated evdev split handoff reports the intercepted release\n";
        return 0;
    } catch (const std::exception& error) {
        if (!isolated) {
            std::cout << "skip: isolated evdev setup unavailable: " << error.what() << '\n';
            return 77;
        }
        std::cerr << "isolated evdev handoff: " << error.what() << '\n';
        return 1;
    }
}
