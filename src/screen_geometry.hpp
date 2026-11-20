#pragma once

#include <optional>
#include <string_view>
#include <utility>

namespace loomwright::detail {

// Tool output only; discovery and fallback are owned by detect_screen_size().
std::optional<std::pair<int, int>> parse_wlr_randr(std::string_view output);
std::optional<std::pair<int, int>> parse_xrandr(std::string_view output);
std::optional<std::pair<int, int>> parse_xdpyinfo(std::string_view output);

}  // namespace loomwright::detail
