#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>

namespace loomwright {

/// True when this build includes the native Wayland picker (GTK3 + wayland-client).
bool wayland_picker_available();

/// Show the screenshot overlay on a native Wayland session. Throws PickerError
/// when the session or the build cannot support it.
std::optional<std::pair<int, int>> pick_mouse_position_wayland(bool verbose);

/// True when the compositor offers zwlr_foreign_toplevel_manager_v1, the
/// protocol used for native Wayland active-window queries.
bool wayland_active_window_available();

/// Fill title and class from the activated Wayland toplevel, if there is one.
/// Wayland has no toplevel PID, so pid and process are left untouched.
/// Returns false when the protocol is unavailable or no toplevel is activated.
bool wayland_active_window_info(std::map<std::string, std::string>& info);

}  // namespace loomwright
