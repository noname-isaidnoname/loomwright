#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "backends.hpp"
#include "unique_fd.hpp"

namespace loomwright {

class InputDevice;

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

private:
    UniqueFd socket_;
    std::vector<BrokerDeviceInfo> devices_;
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

// Tests can classify a private, non-desktop evdev device as a keyboard.
using BrokerKeyboard = std::function<bool(const InputDevice&)>;
int run_input_broker(const BrokerKeyboard& is_keyboard = {});
bool input_broker_running();

}  // namespace loomwright
