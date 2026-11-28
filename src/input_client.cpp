#include "input_broker_protocol.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <linux/input.h>
#include <poll.h>
#include <sys/stat.h>
#include <system_error>
#include <thread>
#include <unistd.h>

#include "subprocess.hpp"

namespace loomwright::broker {

Address address() {
    // The runtime directory groups terminals and desktop launchers without
    // depending on the compositor or on a systemd user service.
    Address result;
    result.value.sun_family = AF_UNIX;
    uint64_t scope = 14695981039346656037ULL;
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime && runtime[0] == '/') {
        struct stat status{};
        const auto path = std::string(runtime) + "/loomwright-input.sock";
        if (path.size() < sizeof(result.value.sun_path) && ::stat(runtime, &status) == 0 &&
            S_ISDIR(status.st_mode) && status.st_uid == ::geteuid() && (status.st_mode & 0077) == 0) {
            result.path = path;
            std::memcpy(result.value.sun_path, path.c_str(), path.size() + 1);
            result.length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
            return result;
        }
        for (unsigned char c : std::string_view(runtime))
            scope = (scope ^ c) * 1099511628211ULL;
    }
    // An abstract socket is the fallback when there is no private runtime
    // directory. It vanishes after a crash without leaving an on-disk name.
    const std::string name = "loomwright-input-" + std::to_string(::geteuid()) + "-" + std::to_string(scope);
    std::memcpy(result.value.sun_path + 1, name.data(), name.size());
    result.length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
    return result;
}

UniqueFd open_socket() {
    UniqueFd socket(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (socket.get() < 0) throw std::system_error(errno, std::generic_category(), "input broker socket");
    return socket;
}

bool same_user(int fd) {
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    return ::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0 &&
           length == sizeof(credentials) && credentials.uid == ::geteuid();
}

bool send(int fd, const Packet& packet) {
    const auto& bytes = packet.bytes();
    if (bytes.size() > kMaxPacket) Packet::invalid();
    ssize_t count;
    do { count = ::send(fd, bytes.data(), bytes.size(), MSG_DONTWAIT | MSG_NOSIGNAL); }
    while (count < 0 && errno == EINTR);
    return count == static_cast<ssize_t>(bytes.size());
}

std::optional<Packet> receive(int fd) {
    char buffer[kMaxPacket];
    ssize_t count;
    do { count = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT | MSG_TRUNC); }
    while (count < 0 && errno == EINTR);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return std::nullopt;
    if (count < 0) throw std::system_error(errno, std::generic_category(), "input broker read");
    if (static_cast<size_t>(count) > sizeof(buffer)) Packet::invalid();
    Packet packet;
    packet.bytes().assign(buffer, static_cast<size_t>(count));
    return packet;
}

std::vector<int> read_codes(Packet& packet) {
    const size_t count = packet.count(KEY_CNT);
    std::vector<int> codes;
    codes.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto value = packet.number();
        if (value < 0 || value > KEY_MAX) Packet::invalid();
        codes.push_back(value);
    }
    return codes;
}

}  // namespace loomwright::broker

namespace loomwright {
namespace {
using namespace broker;
using namespace std::chrono_literals;

UniqueFd connect_broker() {
    auto socket = open_socket();
    const auto endpoint = address();
    if (::connect(socket.get(), reinterpret_cast<const sockaddr*>(&endpoint.value), endpoint.length) == 0) {
        if (!same_user(socket.get())) throw RuntimeErrorLoomwright("Input broker belongs to a different user");
        return socket;
    }
    if (errno == ECONNREFUSED || errno == ENOENT || errno == EAGAIN || errno == EINTR) return UniqueFd();
    throw std::system_error(errno, std::generic_category(), "connecting to input broker");
}

Packet wait_packet(int fd, Clock::time_point deadline) {
    while (Clock::now() < deadline) {
        if (auto packet = broker::receive(fd)) return std::move(*packet);
        pollfd item{fd, POLLIN, 0};
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
        const int ready = ::poll(&item, 1, static_cast<int>(std::max<int64_t>(0, left)));
        if (ready < 0 && errno != EINTR)
            throw std::system_error(errno, std::generic_category(), "waiting for input broker");
    }
    throw RuntimeErrorLoomwright("Timed out waiting for the input broker");
}

[[noreturn]] void read_error(Packet& packet) {
    auto detail = packet.text();
    packet.finish();
    throw RuntimeErrorLoomwright(std::move(detail));
}

}  // namespace

InputClient::InputClient(const BrokerRequest& request) {
    Packet registration(Message::Register);
    registration.number(kProtocol);
    registration.text(request.device);
    registration.number((request.mice ? 1 : 0) | (request.require_keyboard ? 2 : 0));
    registration.codes(request.consume);
    registration.codes(request.panic);

    const auto deadline = Clock::now() + 10s;
    auto next_start = Clock::now();
    while (Clock::now() < deadline) {
        socket_ = connect_broker();
        if (socket_.get() < 0) {
            if (Clock::now() >= next_start) {
                auto started = spawn_detached({"/proc/self/exe", "--internal-input-broker"});
                if (!started) throw RuntimeErrorLoomwright("Cannot start input broker: " + started.error());
                next_start = Clock::now() + 1s;
            }
            std::this_thread::sleep_for(20ms);
            continue;
        }
        if (!broker::send(fd(), registration)) {
            // The last client may have disconnected just as we connected.
            std::this_thread::sleep_for(20ms);
            continue;
        }
        devices_.clear();
        while (true) {
            auto packet = wait_packet(fd(), deadline);
            if (packet.bytes().empty()) break;
            switch (packet.type()) {
                case Message::Device: {
                    if (devices_.size() >= kMaxDevices) Packet::invalid();
                    BrokerDeviceInfo info;
                    info.path = packet.text();
                    info.name = packet.text();
                    info.mouse = packet.count(1) != 0;
                    info.down = read_codes(packet);
                    packet.finish();
                    devices_.push_back(std::move(info));
                    break;
                }
                case Message::Ready:
                    if (packet.number() != kProtocol)
                        throw RuntimeErrorLoomwright("Incompatible input broker; stop older Loomwright instances and retry");
                    packet.finish();
                    return;
                case Message::Error: read_error(packet);
                default: Packet::invalid();
            }
        }
    }
    throw RuntimeErrorLoomwright("Cannot connect to the input broker");
}

std::optional<BrokerEvent> InputClient::receive() {
    auto packet = broker::receive(fd());
    if (!packet) return std::nullopt;
    if (packet->bytes().empty()) throw RuntimeErrorLoomwright("Input broker disconnected");
    const auto type = packet->type();
    if (type == Message::Error) read_error(*packet);
    if (type != Message::Edges && type != Message::Removed) Packet::invalid();
    BrokerEvent event;
    event.path = packet->text();
    event.disconnected = type == Message::Removed;
    if (!event.disconnected) {
        const auto count = packet->count(KEY_CNT * 2);
        event.edges.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto code = packet->count(KEY_MAX);
            const auto value = packet->count(1);
            event.edges.push_back({EV_KEY, static_cast<int>(code), static_cast<int>(value)});
        }
    }
    packet->finish();
    return event;
}

bool input_broker_running() {
    try {
        auto socket = connect_broker();
        return socket.get() >= 0 && broker::send(socket.get(), Packet(Message::Probe));
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace loomwright
