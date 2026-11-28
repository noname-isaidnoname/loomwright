#include <string>
#include <string_view>
#include <vector>

#include "cli.hpp"
#include "input_broker.hpp"

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--internal-input-broker")
        return loomwright::run_input_broker();
    std::vector<std::string> args;
    args.reserve(argc > 1 ? static_cast<size_t>(argc - 1) : 0);
    for (int index = 1; index < argc; ++index) args.emplace_back(argv[index]);
    return loomwright::run_cli(args);
}
