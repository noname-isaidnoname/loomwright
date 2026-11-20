#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace loomwright {

/// Test seam called once after an upload request is queued and before its reply
/// is checked. Production callers use pick_mouse_position() without a hook.
std::optional<std::pair<int, int>> pick_mouse_position_with_upload_hook(
    bool verbose, const std::function<void(const std::string&)>& upload_hook);

/// Wrap the first pointer query after the overlay takes focus. The hook must
/// invoke submit exactly once before the picker waits for the reply.
using PickerQueryHook = std::function<void(const std::function<void()>& submit)>;
std::optional<std::pair<int, int>> pick_mouse_position_with_query_hook(const PickerQueryHook& hook);

}  // namespace loomwright
