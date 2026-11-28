#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>

#include "errors.hpp"
#include "input_broker.hpp"

namespace loomwright::broker {

// Private same-machine protocol. Integers are fixed-width in host byte order;
// SOCK_SEQPACKET supplies record boundaries. Never send native C++ structs.
enum class Message : int32_t {
    Register = 1,
    Device,
    Ready,
    Edges,
    Removed,
    Error,
    Probe,
    OutputClaim,
    OutputState,
};
// Version 2 adds shared output claims and held-state snapshots. An older
// broker cannot coordinate output, so clients refuse to talk to it.
constexpr int32_t kProtocol = 2;
constexpr size_t kMaxPacket = 16384;
constexpr size_t kMaxDevices = 256;
using Clock = std::chrono::steady_clock;

class Packet {
public:
    Packet() = default;
    explicit Packet(Message type) { number(static_cast<int32_t>(type)); }
    void number(int32_t value) { bytes_.append(reinterpret_cast<const char*>(&value), sizeof(value)); }
    void text(std::string_view value) {
        if (value.size() > kMaxPacket) invalid();
        number(static_cast<int32_t>(value.size()));
        bytes_.append(value);
    }
    template <typename Range>
    void codes(const Range& values) {
        number(static_cast<int32_t>(values.size()));
        for (int value : values) number(value);
    }
    int32_t number() {
        if (remaining() < sizeof(int32_t)) invalid();
        int32_t value;
        std::memcpy(&value, bytes_.data() + offset_, sizeof(value));
        offset_ += sizeof(value);
        return value;
    }
    size_t count(size_t max) {
        const auto n = number();
        if (n < 0 || static_cast<size_t>(n) > max) invalid();
        return static_cast<size_t>(n);
    }
    std::string text() {
        const size_t length = count(remaining());
        if (length > remaining()) invalid();
        auto value = bytes_.substr(offset_, length);
        offset_ += length;
        if (value.contains('\0')) invalid();
        return value;
    }
    Message type() { return static_cast<Message>(number()); }
    void finish() const { if (remaining() != 0) invalid(); }
    std::string& bytes() { return bytes_; }
    const std::string& bytes() const { return bytes_; }
    [[noreturn]] static void invalid() { throw RuntimeErrorLoomwright("Invalid input broker packet"); }

private:
    size_t remaining() const { return bytes_.size() - offset_; }
    std::string bytes_;
    size_t offset_ = 0;
};

struct Address {
    sockaddr_un value{};
    socklen_t length = 0;
    std::string path;
};

Address address();
UniqueFd open_socket();
bool same_user(int fd);
bool send(int fd, const Packet& packet);
// Returns nullopt on EAGAIN; an empty packet means EOF.
std::optional<Packet> receive(int fd);
std::vector<int> read_codes(Packet& packet);
OutputKind read_output_kind(Packet& packet);

}  // namespace loomwright::broker
