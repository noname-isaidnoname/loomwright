#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "backends.hpp"
#include "unique_fd.hpp"

namespace loomwright {

class InputDevice;

namespace broker {
class Packet;
}

/// A synthetic key or button whose held state is shared through the broker.
enum class OutputKind : int32_t { Key = 0, Button = 1 };

struct OutputState {
    OutputKind kind = OutputKind::Key;
    std::string name;
    int owners = 0;
};

struct BrokerRequest {
    std::string device;
    std::set<int> consume;
    std::set<int> panic;
    bool mice = false;
    bool require_keyboard = false;
};

struct BrokerDeviceInfo {
    std::string path;
    std::string name;
    bool mouse = false;
    std::vector<int> down;
};

struct BrokerEvent {
    std::string path;
    std::vector<RawEvent> edges;
    bool disconnected = false;
};

class InputClient {
public:
    explicit InputClient(const BrokerRequest& request);
    int fd() const { return socket_.get(); }
    const std::vector<BrokerDeviceInfo>& devices() const { return devices_; }
    std::optional<BrokerEvent> receive();

    /// `down` acquires a shared claim, `up` releases this client's claim.
    void claim_output(OutputKind kind, const std::string& name, bool down);
    /// Mirrors the broker's held-state updates, including the registration snapshot.
    bool output_held(OutputKind kind, const std::string& name) const;
    int output_owners(OutputKind kind, const std::string& name) const;

private:
    void apply_output_state(broker::Packet& packet);

    UniqueFd socket_;
    std::vector<BrokerDeviceInfo> devices_;
    std::map<std::pair<OutputKind, std::string>, int> shared_output_;
};

/// Injects one physical down per key/button while one or more scripts claim
/// it, and one up after the last claim is released. The broker owns the only
/// backend that injects shared keys and buttons, so two scripts cannot inject
/// the same event twice.
class OutputArbiter {
public:
    explicit OutputArbiter(OutputBackend& backend);

    /// Returns true when this client is the first owner and a down was injected.
    bool acquire(int owner, OutputKind kind, const std::string& name);
    /// Returns true when this was the last owner and an up was injected.
    bool release(int owner, OutputKind kind, const std::string& name);
    /// Releases a departing client's claims; failed releases are best-effort.
    std::vector<OutputState> drop(int owner);
    /// Releases every claim at broker shutdown. Never throws.
    void clear();
    std::vector<OutputState> held() const;
    int owners(OutputKind kind, const std::string& name) const;
    bool is_held(OutputKind kind, const std::string& name) const;

private:
    using Claim = std::pair<OutputKind, std::string>;
    void inject(OutputKind kind, const std::string& name, bool down);

    OutputBackend& backend_;
    std::map<Claim, std::set<int>> owners_;
};

// Keep the desktop's delivery decision for the entire physical press, even
// if clients join or leave before its repeat or release.
class InputSuppression {
public:
    void configure(std::set<int> consume, const std::set<int>& panic);
    bool filter(const RawEvent& event);

private:
    std::set<int> consume_;
    std::set<int> swallowed_;
};

// Tests can classify a private, non-desktop evdev device as a keyboard and
// replace the shared output backend with one that records injections.
using BrokerKeyboard = std::function<bool(const InputDevice&)>;
using BrokerOutputFactory = std::function<std::unique_ptr<OutputBackend>()>;
int run_input_broker(const BrokerKeyboard& is_keyboard = {}, BrokerOutputFactory output = {});
bool input_broker_running();

}  // namespace loomwright
