#include "input_broker_protocol.hpp"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <linux/input.h>
#include <map>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

#include "backends.hpp"
#include "input.hpp"
#include "runtime_internal.hpp"

namespace loomwright {

void InputSuppression::configure(std::set<int> consume, const std::set<int>& panic) {
    for (int code : panic) consume.erase(code);
    consume_ = std::move(consume);
}

bool InputSuppression::filter(const RawEvent& event) {
    if (event.type != EV_KEY) return false;
    if (event.value == 1 && consume_.contains(event.code)) swallowed_.insert(event.code);
    if (event.value == 0) return swallowed_.erase(event.code) != 0;
    return swallowed_.contains(event.code);
}

OutputArbiter::OutputArbiter(OutputBackend& backend) : backend_(backend) {}

void OutputArbiter::inject(OutputKind kind, const std::string& name, bool down) {
    if (kind == OutputKind::Button) backend_.button(name, down);
    else backend_.key(name, down);
}

bool OutputArbiter::acquire(int owner, OutputKind kind, const std::string& name) {
    auto& members = owners_[{kind, name}];
    const bool first = members.empty();
    members.insert(owner);
    if (first) inject(kind, name, true);
    return first;
}

bool OutputArbiter::release(int owner, OutputKind kind, const std::string& name) {
    const auto found = owners_.find({kind, name});
    if (found == owners_.end() || found->second.erase(owner) == 0) return false;
    if (!found->second.empty()) return false;
    owners_.erase(found);
    inject(kind, name, false);
    return true;
}

std::vector<OutputState> OutputArbiter::drop(int owner) {
    std::vector<OutputState> changes;
    for (auto it = owners_.begin(); it != owners_.end();) {
        if (it->second.erase(owner) == 0) {
            ++it;
            continue;
        }
        const OutputState state{it->first.first, it->first.second, static_cast<int>(it->second.size())};
        if (it->second.empty()) {
            it = owners_.erase(it);
            // One broken release must not hide the other claims of a client
            // that is already leaving.
            try { inject(state.kind, state.name, false); } catch (...) {}
        } else {
            ++it;
        }
        changes.push_back(state);
    }
    return changes;
}

void OutputArbiter::clear() {
    while (!owners_.empty()) {
        const auto it = owners_.begin();
        const auto kind = it->first.first;
        const auto name = it->first.second;
        owners_.erase(it);
        try { inject(kind, name, false); } catch (...) {}
    }
}

std::vector<OutputState> OutputArbiter::held() const {
    std::vector<OutputState> result;
    for (const auto& [claim, members] : owners_)
        result.push_back({claim.first, claim.second, static_cast<int>(members.size())});
    return result;
}

int OutputArbiter::owners(OutputKind kind, const std::string& name) const {
    const auto found = owners_.find({kind, name});
    return found == owners_.end() ? 0 : static_cast<int>(found->second.size());
}

bool OutputArbiter::is_held(OutputKind kind, const std::string& name) const {
    return owners(kind, name) > 0;
}

namespace {
using namespace broker;
using namespace std::chrono_literals;

struct Client {
    UniqueFd fd;
    Clock::time_point deadline;
    bool registered = false;
    bool dead = false;
    std::set<int> consume;
    std::set<int> panic;
    std::set<dev_t> devices;
};

struct Device {
    std::unique_ptr<InputDevice> input;
    bool keyboard = false;
    bool mouse = false;
    std::set<int> down;
    InputSuppression suppression;
    std::unique_ptr<PassthroughDevice> replay;
    bool lost = false;
    bool mid_frame = false;

    void release() {
        if (!replay) return;
        auto sink = std::move(replay);
        // Always ungrab, even if flushing releases failed. Keep the fd for
        // observers; the broker is still the only reader for this device.
        release_input_capture(sink.get(), [this] { input->ungrab(); }, [] {});
    }

    ~Device() {
        try { release(); } catch (...) {}
    }
};

class Server {
public:
    Server(const BrokerKeyboard& is_keyboard, BrokerOutputFactory output)
        : is_keyboard_(is_keyboard), output_factory_(std::move(output)) {}
    int run();

private:
    void accept_client(int listener);
    void request(Client& client);
    void register_client(Client& client, Packet& packet);
    void fail_client(Client& client, const std::string& detail);
    void maintain();
    bool read_device(dev_t id, Device& device);
    void broadcast(dev_t id, const Packet& packet);
    void broadcast_output_state(const OutputState& state);
    void disconnect_client(Client& client);
    OutputArbiter& ensure_output();
    void remove_device(dev_t id, const std::string& error = {});
    void activate(Device& device);

    const BrokerKeyboard& is_keyboard_;
    BrokerOutputFactory output_factory_;
    std::unique_ptr<OutputBackend> output_backend_;
    std::unique_ptr<OutputArbiter> output_;
    std::map<int, Client> clients_;
    std::map<dev_t, Device> devices_;
};

int Server::run() {
    const auto endpoint = address();
    UniqueFd election;
    struct SocketFile {
        std::string path;
        bool bound = false;
        ~SocketFile() { if (bound) (void)::unlink(path.c_str()); }
    } file{endpoint.path};
    if (!endpoint.path.empty()) {
        const auto lock_path = endpoint.path + ".lock";
        election = UniqueFd(::open(lock_path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600));
        if (election.get() < 0)
            throw std::system_error(errno, std::generic_category(), "opening input broker lock");
        struct stat status{};
        if (::fstat(election.get(), &status) < 0 || !S_ISREG(status.st_mode) || status.st_uid != ::geteuid())
            throw RuntimeErrorLoomwright("Input broker lock is not an owned file");
        if (::flock(election.get(), LOCK_EX | LOCK_NB) < 0) {
            if (errno == EWOULDBLOCK) return 0;  // another launcher won
            throw std::system_error(errno, std::generic_category(), "locking input broker");
        }
        if (::lstat(endpoint.path.c_str(), &status) == 0) {
            if (!S_ISSOCK(status.st_mode) || status.st_uid != ::geteuid())
                throw RuntimeErrorLoomwright("Input broker socket path exists and is not an owned socket");
            if (::unlink(endpoint.path.c_str()) < 0)
                throw std::system_error(errno, std::generic_category(), "removing stale input broker socket");
        } else if (errno != ENOENT) {
            throw std::system_error(errno, std::generic_category(), "checking input broker socket");
        }
    }
    auto listener = open_socket();
    if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&endpoint.value), endpoint.length) < 0) {
        if (errno == EADDRINUSE) return 0;  // another launcher won
        throw std::system_error(errno, std::generic_category(), "binding input broker");
    }
    if (!endpoint.path.empty()) {
        file.bound = true;
        if (::chmod(endpoint.path.c_str(), 0600) < 0)
            throw std::system_error(errno, std::generic_category(), "setting input broker socket permissions");
    }
    if (::listen(listener.get(), 32) < 0)
        throw std::system_error(errno, std::generic_category(), "listening on input broker");

    sigset_t mask;
    ::sigemptyset(&mask);
    ::sigaddset(&mask, SIGTERM);
    ::sigaddset(&mask, SIGINT);
    if (::sigprocmask(SIG_BLOCK, &mask, nullptr) < 0)
        throw std::system_error(errno, std::generic_category(), "blocking input broker signals");
    UniqueFd signals(::signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK));
    if (signals.get() < 0)
        throw std::system_error(errno, std::generic_category(), "input broker signalfd");
    struct Cleanup {
        Server& server;
        ~Cleanup() {
            // Release shared synthetic holds before the backend and sockets
            // disappear; a key held by a dead broker would stay stuck.
            if (server.output_) {
                server.output_->clear();
                try { server.output_backend_->close(); } catch (...) {}
            }
            server.devices_.clear();
            server.clients_.clear();
        }
    } cleanup{*this};

    auto idle_since = Clock::now();
    while (true) {
        std::vector<pollfd> ready{{signals.get(), POLLIN, 0}, {listener.get(), POLLIN, 0}};
        for (const auto& [_, device] : devices_) ready.push_back({device.input->fd(), POLLIN, 0});
        const size_t client_start = ready.size();
        for (const auto& [fd, _] : clients_) ready.push_back({fd, POLLIN, 0});
        const bool timing = clients_.empty() || std::ranges::any_of(clients_, [](const auto& entry) {
            return !entry.second.registered;
        });
        const int result = ::poll(ready.data(), ready.size(), timing ? 250 : -1);
        if (result < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "polling input broker");
        }
        if (ready[0].revents != 0) return 0;

        // Handle input already queued under the current policy before applying
        // registrations. Each ready device consumes at most one read batch.
        size_t i = 2;
        for (auto it = devices_.begin(); it != devices_.end();) {
            auto& [id, device] = *it++;
            const auto events = ready[i++].revents;
            try {
                if (events & (POLLERR | POLLHUP | POLLNVAL)) remove_device(id);
                else if ((events & POLLIN) && !read_device(id, device)) remove_device(id);
            } catch (const std::exception& error) {
                remove_device(id, error.what());
            }
        }
        for (i = client_start; i < ready.size(); ++i) {
            auto& client = clients_.at(ready[i].fd);
            if (client.dead) continue;
            if (ready[i].revents & (POLLHUP | POLLERR | POLLNVAL)) disconnect_client(client);
            else if (ready[i].revents & POLLIN) request(client);
            else if (!client.registered && Clock::now() >= client.deadline) disconnect_client(client);
            // A subsequent registration must not include already-dead claims.
            if (client.dead) maintain();
        }
        if (ready[1].revents & POLLIN) accept_client(listener.get());
        std::erase_if(clients_, [](const auto& entry) { return entry.second.dead; });
        maintain();
        if (!clients_.empty() || !devices_.empty()) idle_since = Clock::now();
        else if (result == 0 && Clock::now() - idle_since >= 1s) return 0;
    }
}

void Server::accept_client(int listener) {
    UniqueFd fd(::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK));
    if (fd.get() < 0 || !same_user(fd.get()) || clients_.size() >= 128) return;
    const int key = fd.get();
    clients_.try_emplace(key, Client{std::move(fd), Clock::now() + 10s, false, false, {}, {}, {}});
}

void Server::fail_client(Client& client, const std::string& detail) {
    Packet reply(Message::Error);
    reply.text(detail.substr(0, kMaxPacket - 8));
    (void)broker::send(client.fd.get(), reply);
    disconnect_client(client);
}

void Server::disconnect_client(Client& client) {
    if (client.dead) return;
    client.dead = true;
    if (!output_) return;
    for (const auto& state : output_->drop(client.fd.get())) broadcast_output_state(state);
}

OutputArbiter& Server::ensure_output() {
    if (!output_) {
        auto backend = output_factory_ ? output_factory_() : choose_backend("auto", 1920, 1080);
        if (!backend) throw RuntimeErrorLoomwright("No output backend is available for shared output");
        output_backend_ = std::move(backend);
        output_ = std::make_unique<OutputArbiter>(*output_backend_);
    }
    return *output_;
}

void Server::broadcast_output_state(const OutputState& state) {
    Packet packet(Message::OutputState);
    packet.number(std::to_underlying(state.kind));
    packet.text(state.name);
    packet.number(state.owners);
    for (auto& [_, client] : clients_) {
        if (!client.registered || client.dead) continue;
        // The queue is bounded; a client that cannot keep up must stop rather
        // than keep a stale view of which keys and buttons are held.
        if (!broker::send(client.fd.get(), packet)) fail_client(client, "Input broker client could not keep up");
    }
}

void Server::request(Client& client) {
    try {
        auto packet = broker::receive(client.fd.get());
        if (!packet) return;
        if (packet->bytes().empty()) {
            disconnect_client(client);
            return;
        }
        const auto type = packet->type();
        if (type == Message::Probe && !client.registered) {
            packet->finish();
            disconnect_client(client);
        } else if (type == Message::OutputClaim) {
            if (!client.registered) Packet::invalid();
            const bool acquire = packet->count(1) != 0;
            const auto kind = read_output_kind(*packet);
            const auto name = packet->text();
            if (name.empty()) Packet::invalid();
            packet->finish();
            auto& arbiter = ensure_output();
            const int before = arbiter.owners(kind, name);
            if (acquire) arbiter.acquire(client.fd.get(), kind, name);
            else arbiter.release(client.fd.get(), kind, name);
            const int after = arbiter.owners(kind, name);
            if (before != after) broadcast_output_state({kind, name, after});
        } else {
            if (type != Message::Register || client.registered) Packet::invalid();
            register_client(client, *packet);
        }
    } catch (const std::exception& error) {
        fail_client(client, error.what());
    }
}

void Server::register_client(Client& client, Packet& packet) {
    if (packet.number() != kProtocol)
        throw RuntimeErrorLoomwright("Incompatible input broker; stop older Loomwright instances and retry");
    const auto requested = packet.text();
    if (!requested.empty() && requested.front() != '/') Packet::invalid();
    const auto flags = packet.count(3);
    const bool mice = (flags & 1) != 0;
    const bool require_keyboard = (flags & 2) != 0;
    const auto consume = read_codes(packet);
    const auto panic = read_codes(packet);
    packet.finish();
    client.consume.insert(consume.begin(), consume.end());
    client.panic.insert(panic.begin(), panic.end());

    bool found_keyboard = false;
    bool found_mouse = false;
    const auto paths = requested.empty() ? list_input_devices() : std::vector{requested};
    for (const auto& path : paths) {
        std::error_code ec;
        const auto resolved = std::filesystem::canonical(path, ec);
        if (ec) continue;
        auto input = InputDevice::open(resolved.string());
        if (!input) continue;
        struct stat status{};
        if (::fstat(input->fd(), &status) < 0 || !S_ISCHR(status.st_mode)) continue;
        const auto name = detail::lower_ascii(input->name());
        if (!is_keyboard_ && (name.contains("loomwright") || name.contains("ydotool") ||
                              name.contains("virtual pointer") || name.contains("remap_clicks"))) continue;
        const bool keyboard = is_keyboard_ ? is_keyboard_(*input) : input->is_keyboard();
        const bool mouse = input->is_mouse();
        if (!keyboard && !(mice && mouse)) continue;
        if (!client.devices.insert(status.st_rdev).second) continue;
        if (client.devices.size() > kMaxDevices) throw RuntimeErrorLoomwright("Too many input devices");
        auto [it, inserted] = devices_.try_emplace(status.st_rdev);
        if (inserted) {
            try {
                const auto keys = input->pressed_key_codes();
                it->second.down.insert(keys.begin(), keys.end());
                it->second.keyboard = keyboard;
                it->second.mouse = mouse;
                it->second.input = std::move(input);
            } catch (...) {
                devices_.erase(it);
                throw;
            }
        }
        found_keyboard |= keyboard;
        found_mouse |= mouse;
    }
    if (require_keyboard && !found_keyboard)
        throw RuntimeErrorLoomwright("No accessible keyboard devices were found. Use `loomwright --devices` "
                                     "to inspect evdev permissions");
    if (mice && !found_mouse)
        throw RuntimeErrorLoomwright("No accessible mouse devices were found. Use `loomwright --devices` "
                                     "to inspect evdev permissions");

    // Do not install any claims until all devices can be captured and the
    // reply is queued. On rejection, maintain() drains any device that was
    // already captured without applying the failed client's claims.
    for (const auto id : client.devices) {
        auto& device = devices_.at(id);
        if (!device.keyboard || device.replay) continue;
        const auto& supported = device.input->capabilities().keys;
        const bool needs_grab = std::any_of(supported.begin(), supported.end(), [&](int code) {
            return client.consume.contains(code);
        });
        if (needs_grab) activate(device);
    }
    for (const auto id : client.devices) {
        const auto& device = devices_.at(id);
        Packet info(Message::Device);
        info.text(device.input->path());
        info.text(device.input->name());
        info.number(device.mouse ? 1 : 0);
        info.codes(device.down);
        if (!broker::send(client.fd.get(), info)) throw RuntimeErrorLoomwright("Input broker registration interrupted");
    }
    // A joining script must see the shared held state before it can inject
    // anything, so the snapshot precedes Ready.
    if (output_) {
        for (const auto& state : output_->held()) {
            Packet info(Message::OutputState);
            info.number(std::to_underlying(state.kind));
            info.text(state.name);
            info.number(state.owners);
            if (!broker::send(client.fd.get(), info))
                throw RuntimeErrorLoomwright("Input broker registration interrupted");
        }
    }
    Packet ready(Message::Ready);
    ready.number(kProtocol);
    if (!broker::send(client.fd.get(), ready)) throw RuntimeErrorLoomwright("Input broker registration interrupted");
    client.registered = true;
}

void Server::activate(Device& device) {
    try {
        // Keep an existing observer's queue untouched until the handoff
        // succeeds. EVIOCGKEY flushes EV_KEY on the client's own fd.
        auto capture = InputDevice::open(device.input->path());
        if (!capture) throw RuntimeErrorLoomwright("could not reopen device");
        struct stat before{}, after{};
        if (::fstat(device.input->fd(), &before) < 0 || ::fstat(capture->fd(), &after) < 0 ||
            before.st_ino != after.st_ino || before.st_dev != after.st_dev || before.st_rdev != after.st_rdev)
            throw RuntimeErrorLoomwright("input device changed during capture setup");
        auto replay = std::make_unique<PassthroughDevice>(capture->capabilities());
        grab_idle_input([&] { return capture->pressed_key_codes(); }, [&] { return capture->discard_pending_events(); },
                        [&] { capture->grab(); }, [&] { capture->ungrab(); });
        // The old queue is now finite. Forward transitions the desktop already
        // saw, but do not replay them a second time.
        for (int attempt = 0; ; ++attempt) {
            pollfd item{device.input->fd(), POLLIN, 0};
            const int ready = ::poll(&item, 1, 0);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0 || (item.revents & (POLLERR | POLLHUP | POLLNVAL)))
                throw RuntimeErrorLoomwright("could not drain observed input during capture setup");
            if (ready == 0) break;
            if (attempt >= 128 || device.lost)
                throw RuntimeErrorLoomwright("too much pending input during capture setup; retry with keyboard idle");
            if (!read_device(before.st_rdev, device))
                throw RuntimeErrorLoomwright("input device disconnected during capture setup");
        }
        if (!device.down.empty() || device.lost || device.mid_frame)
            throw RuntimeErrorLoomwright("input changed during capture setup; release all keys and buttons and retry");
        device.input = std::move(capture);
        device.replay = std::move(replay);
    } catch (const std::exception& error) {
        throw RuntimeErrorLoomwright("Cannot grab input device " + device.input->path() + " (" +
                                     device.input->name() + ") for key consumption. " + error.what() +
                                     ". If busy, stop any direct-mode Loomwright or other remapper using it");
    }
}

void Server::maintain() {
    // Keep dead entries until the current poll snapshot is exhausted. Their
    // claims are inactive immediately, regardless of when the fd is closed.
    std::set<int> panic;
    for (const auto& [_, client] : clients_)
        if (client.registered && !client.dead) panic.insert(client.panic.begin(), client.panic.end());
    for (auto it = devices_.begin(); it != devices_.end();) {
        auto& [id, device] = *it++;
        bool subscribed = false;
        std::set<int> consume;
        for (const auto& [_, client] : clients_) {
            if (!client.registered || client.dead || !client.devices.contains(id)) continue;
            subscribed = true;
            if (!device.keyboard) continue;
            for (int code : client.consume) {
                const auto& supported = device.input->capabilities().keys;
                if (std::ranges::find(supported, code) != supported.end()) consume.insert(code);
            }
        }
        const bool claimed = !consume.empty();
        device.suppression.configure(std::move(consume), panic);
        try {
            if (device.replay && !claimed && device.down.empty() && !device.lost && !device.mid_frame) {
                pollfd item{device.input->fd(), POLLIN, 0};
                if (::poll(&item, 1, 0) == 0) device.release();
            }
            if (!subscribed && !device.replay) devices_.erase(id);
        } catch (const std::exception& error) {
            remove_device(id, error.what());
        }
    }
}

void Server::broadcast(dev_t id, const Packet& packet) {
    for (auto& [_, client] : clients_) {
        if (client.registered && !client.dead && client.devices.contains(id) &&
            !broker::send(client.fd.get(), packet)) {
            fail_client(client, "Input broker client could not keep up");
            // The send queue may be full even for the error. EOF still
            // forces the affected runtime to stop instead of losing edges.
        }
    }
}

void Server::remove_device(dev_t id, const std::string& error) {
    const auto& path = devices_.at(id).input->path();
    if (error.empty()) {
        Packet packet(Message::Removed);
        packet.text(path);
        broadcast(id, packet);
    } else {
        for (auto& [_, client] : clients_)
            if (!client.dead && client.devices.contains(id))
                fail_client(client, "Input broker failed on " + path + ": " + error);
    }
    for (auto& [_, client] : clients_) client.devices.erase(id);
    devices_.erase(id);
}

bool Server::read_device(dev_t id, Device& device) {
    input_event events[64];
    const ssize_t bytes = ::read(device.input->fd(), events, sizeof(events));
    if (bytes == 0) return false;
    if (bytes < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return true;
        if (errno == ENODEV) return false;
        throw std::system_error(errno, std::generic_category(), "reading evdev");
    }
    std::vector<RawEvent> edges;
    const auto apply = [&](const RawEvent& event) {
        const bool sync = event.type == EV_SYN && event.code == SYN_REPORT;
        device.mid_frame = !sync;
        if (event.type == EV_KEY) {
            // Filter only real edges/repeat. In particular, never reclassify a
            // held passthrough key if a new client now wants to consume it.
            if (event.value == 1 && !device.down.insert(event.code).second) return;
            if (event.value == 0 && device.down.erase(event.code) == 0) return;
            if (event.value == 0 || event.value == 1) edges.push_back(event);
        }
        if (device.replay && !device.suppression.filter(event))
            device.replay->write_event(event.type, event.code, event.value);
    };
    for (size_t i = 0; i < static_cast<size_t>(bytes) / sizeof(input_event); ++i) {
        const auto& ev = events[i];
        if (ev.type == EV_SYN && ev.code == SYN_DROPPED) {
            device.lost = true;
            continue;
        }
        if (device.lost) {
            if (ev.type != EV_SYN || ev.code != SYN_REPORT) continue;
            const auto snapshot = device.input->pressed_key_codes();
            const std::set<int> now(snapshot.begin(), snapshot.end());
            const auto before = device.down;
            for (int code : before) if (!now.contains(code)) apply({EV_KEY, code, 0});
            for (int code : now) if (!before.contains(code)) apply({EV_KEY, code, 1});
            apply({EV_SYN, SYN_REPORT, 0});
            device.lost = false;
            break;  // remainder predates EVIOCGKEY
        }
        apply({ev.type, ev.code, ev.value});
    }
    if (edges.empty()) return true;
    Packet packet(Message::Edges);
    packet.text(device.input->path());
    packet.number(static_cast<int32_t>(edges.size()));
    for (const auto& edge : edges) {
        packet.number(edge.code);
        packet.number(edge.value);
    }
    broadcast(id, packet);
    return true;
}

}  // namespace

int run_input_broker(const BrokerKeyboard& is_keyboard, BrokerOutputFactory output) {
    try {
        return Server(is_keyboard, std::move(output)).run();
    } catch (const std::exception& error) {
        std::cerr << "loomwright input broker: " << error.what() << '\n';
        return 1;
    }
}

}  // namespace loomwright
