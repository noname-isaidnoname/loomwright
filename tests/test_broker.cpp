#include "harness.hpp"

#include <linux/input.h>

#include <string>
#include <vector>

#include "cli.hpp"
#include "input_broker.hpp"

using namespace loomwright;

namespace {

BrokerStatus sample_status() {
    BrokerStatus status;
    status.protocol = 3;
    status.clients.push_back({"first", {"/dev/input/event3"}, {KEY_A, KEY_D}, {}});
    status.clients.push_back({"second", {"/dev/input/event3"}, {KEY_A}, {}});
    status.devices.push_back({"/dev/input/event3", "Test Keyboard", true, false, true, {KEY_A}});
    status.claims.push_back({OutputKind::Key, "SHIFT", {"first"}});
    return status;
}

}  // namespace

TEST_CASE(cli_parses_the_broker_flag) {
    const CliArgs args = parse_cli({"--broker"});
    CHECK_TRUE(args.broker);
    CHECK_TRUE(!args.run.has_value());
    CHECK_THROWS(parse_cli({"--broker", "--devices"}), CliUsageError);
}

TEST_CASE(broker_conflicts_name_the_script_and_device) {
    const auto conflicts = broker_conflicts(sample_status());
    CHECK_EQ(conflicts.size(), static_cast<size_t>(1));
    CHECK_EQ(conflicts[0].kind, std::string("consume"));
    CHECK_EQ(conflicts[0].key, std::string("A"));
    CHECK_EQ(conflicts[0].device, std::string("/dev/input/event3 (Test Keyboard)"));
    CHECK_TRUE(conflicts[0].scripts == (std::vector<std::string>{"first", "second"}));
}

TEST_CASE(broker_conflicts_include_shared_synthetic_holds) {
    BrokerStatus status = sample_status();
    status.clients[1].consume.clear();
    status.claims[0].owners.push_back("second");
    const auto conflicts = broker_conflicts(status);
    CHECK_EQ(conflicts.size(), static_cast<size_t>(1));
    CHECK_EQ(conflicts[0].kind, std::string("hold"));
    CHECK_EQ(conflicts[0].key, std::string("key SHIFT"));
    CHECK_EQ(conflicts[0].device, std::string("synthetic output"));
    CHECK_TRUE(conflicts[0].scripts == (std::vector<std::string>{"first", "second"}));
}

TEST_CASE(broker_status_format_lists_scripts_devices_claims_and_conflicts) {
    const std::string text = format_broker_status(sample_status());
    CHECK_TRUE(text.contains("Shared input broker: running (protocol 3)"));
    CHECK_TRUE(text.contains("Scripts (2):"));
    CHECK_TRUE(text.contains("  \"first\" — /dev/input/event3"));
    CHECK_TRUE(text.contains("    consumes: A, D"));
    CHECK_TRUE(text.contains("Devices (1):"));
    CHECK_TRUE(text.contains("  /dev/input/event3: Test Keyboard — keyboard, grabbed"));
    CHECK_TRUE(text.contains("    down: A"));
    CHECK_TRUE(text.contains("    consumed by: \"first\" (A, D), \"second\" (A)"));
    CHECK_TRUE(text.contains("  A (consume, /dev/input/event3 (Test Keyboard)): \"first\", \"second\""));
    CHECK_TRUE(text.contains("  D (consume, /dev/input/event3 (Test Keyboard)): \"first\""));
    CHECK_TRUE(text.contains("  SHIFT (held key): \"first\""));
    CHECK_TRUE(text.contains("Conflicts (1):"));
    CHECK_TRUE(text.contains("  key A on /dev/input/event3 (Test Keyboard): consumed by \"first\", \"second\""));
    CHECK_TRUE(text.ends_with("consumed by \"first\", \"second\"\n"));
}

TEST_CASE(broker_status_format_handles_an_idle_broker) {
    const std::string text = format_broker_status({});
    CHECK_TRUE(text.contains("Scripts: none"));
    CHECK_TRUE(text.contains("Devices: none"));
    CHECK_TRUE(text.contains("Claimed keys: none"));
    CHECK_TRUE(text.contains("Conflicts: none"));
}
