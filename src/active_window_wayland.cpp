#include "wayland_native.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <poll.h>
#include <string>
#include <vector>

#if defined(LOOMWRIGHT_WAYLAND_TOPLEVEL)
#include <wayland-client.h>

#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#endif

namespace loomwright {
namespace {

#if defined(LOOMWRIGHT_WAYLAND_TOPLEVEL)

constexpr int kRoundtripTimeoutMs = 1000;

struct CallbackState {
    bool done = false;
};

void callback_done(void* data, wl_callback* callback, uint32_t) {
    static_cast<CallbackState*>(data)->done = true;
    wl_callback_destroy(callback);
}

const wl_callback_listener kCallbackListener = {callback_done};

// A roundtrip with a deadline: a stuck compositor must not hang the runtime.
bool bounded_roundtrip(wl_display* display, int timeout_ms) {
    CallbackState state;
    wl_callback* callback = wl_display_sync(display);
    if (callback == nullptr) return false;
    wl_callback_add_listener(callback, &kCallbackListener, &state);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!state.done) {
        if (wl_display_get_error(display) != 0) return false;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) return false;
        const int pending = wl_display_prepare_read(display);
        if (pending < 0) return false;
        if (pending > 0) {
            if (wl_display_dispatch_pending(display) < 0) return false;
            continue;
        }
        if (wl_display_flush(display) < 0) {
            wl_display_cancel_read(display);
            return false;
        }
        pollfd item{wl_display_get_fd(display), POLLIN, 0};
        const int ready = ::poll(&item, 1, static_cast<int>(std::min<int>(1000, static_cast<int>(left))));
        if (ready < 0 && errno == EINTR) {
            wl_display_cancel_read(display);
            continue;
        }
        if (ready == 0) {
            wl_display_cancel_read(display);
            return false;
        }
        if (wl_display_read_events(display) < 0) return false;
        if (wl_display_dispatch_pending(display) < 0) return false;
    }
    return true;
}

// WAYLAND_SOCKET is consumed by libwayland and belongs to our caller, so use
// the named display for this short-lived connection and restore the variable.
wl_display* connect_display() {
    const char* socket = std::getenv("WAYLAND_SOCKET");
    if (socket == nullptr || socket[0] == '\0') return wl_display_connect(nullptr);
    const std::string saved = socket;
    ::unsetenv("WAYLAND_SOCKET");
    wl_display* display = wl_display_connect(nullptr);
    ::setenv("WAYLAND_SOCKET", saved.c_str(), 1);
    return display;
}

struct Toplevel {
    std::string title;
    std::string app_id;
    bool activated = false;
    zwlr_foreign_toplevel_handle_v1* handle = nullptr;
};

struct RegistryState {
    zwlr_foreign_toplevel_manager_v1* manager = nullptr;
    std::vector<std::unique_ptr<Toplevel>> toplevels;
};

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    (void)name;
    auto* state = static_cast<RegistryState*>(data);
    if (interface != nullptr && version >= 1 &&
        std::strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
        state->manager = static_cast<zwlr_foreign_toplevel_manager_v1*>(
            wl_registry_bind(registry, name, &zwlr_foreign_toplevel_manager_v1_interface, 1));
    }
}

void registry_remove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener kRegistryListener = {registry_global, registry_remove};

void toplevel_title(void* data, zwlr_foreign_toplevel_handle_v1*, const char* title) {
    static_cast<Toplevel*>(data)->title = title != nullptr ? title : "";
}

void toplevel_app_id(void* data, zwlr_foreign_toplevel_handle_v1*, const char* app_id) {
    static_cast<Toplevel*>(data)->app_id = app_id != nullptr ? app_id : "";
}

void toplevel_state(void* data, zwlr_foreign_toplevel_handle_v1*, wl_array* states) {
    auto* toplevel = static_cast<Toplevel*>(data);
    const auto* values = static_cast<const uint32_t*>(states->data);
    for (size_t i = 0; i < states->size / sizeof(uint32_t); ++i) {
        if (values[i] == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) toplevel->activated = true;
    }
}

void toplevel_noop(void*, zwlr_foreign_toplevel_handle_v1*) {}
void toplevel_noop_output(void*, zwlr_foreign_toplevel_handle_v1*, wl_output*) {}

const zwlr_foreign_toplevel_handle_v1_listener kToplevelListener = {
    toplevel_title, toplevel_app_id, toplevel_noop_output, toplevel_noop_output, toplevel_state,
    toplevel_noop, toplevel_noop};

void manager_toplevel(void* data, zwlr_foreign_toplevel_manager_v1*, zwlr_foreign_toplevel_handle_v1* handle) {
    auto* state = static_cast<RegistryState*>(data);
    auto toplevel = std::make_unique<Toplevel>();
    toplevel->handle = handle;
    if (toplevel->handle != nullptr) {
        zwlr_foreign_toplevel_handle_v1_add_listener(toplevel->handle, &kToplevelListener, toplevel.get());
        state->toplevels.push_back(std::move(toplevel));
    }
}

void manager_finished(void*, zwlr_foreign_toplevel_manager_v1*) {}

const zwlr_foreign_toplevel_manager_v1_listener kManagerListener = {manager_toplevel, manager_finished};

bool query(bool presence_only, std::map<std::string, std::string>* info) {
    wl_display* display = connect_display();
    if (display == nullptr) return false;
    RegistryState state;
    wl_registry* registry = wl_display_get_registry(display);
    bool ok = false;
    bool found = false;
    if (registry != nullptr) {
        wl_registry_add_listener(registry, &kRegistryListener, &state);
        ok = bounded_roundtrip(display, kRoundtripTimeoutMs);
        if (ok && presence_only) {
            found = state.manager != nullptr;
        } else if (ok && state.manager != nullptr) {
            zwlr_foreign_toplevel_manager_v1_add_listener(state.manager, &kManagerListener, &state);
            ok = bounded_roundtrip(display, kRoundtripTimeoutMs);
            if (ok) {
                for (const auto& toplevel : state.toplevels) {
                    if (!toplevel->activated || (toplevel->title.empty() && toplevel->app_id.empty())) continue;
                    (*info)["title"] = toplevel->title;
                    (*info)["class"] = toplevel->app_id;
                    found = true;
                    break;
                }
            }
        }
    }
    if (state.manager != nullptr) {
        zwlr_foreign_toplevel_manager_v1_stop(state.manager);
        zwlr_foreign_toplevel_manager_v1_destroy(state.manager);
    }
    for (const auto& toplevel : state.toplevels) {
        if (toplevel->handle != nullptr) zwlr_foreign_toplevel_handle_v1_destroy(toplevel->handle);
    }
    if (registry != nullptr) wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return found;
}

#endif  // LOOMWRIGHT_WAYLAND_TOPLEVEL

}  // namespace

bool wayland_active_window_available() {
#if defined(LOOMWRIGHT_WAYLAND_TOPLEVEL)
    return query(true, nullptr);
#else
    return false;
#endif
}

bool wayland_active_window_info(std::map<std::string, std::string>& info) {
#if defined(LOOMWRIGHT_WAYLAND_TOPLEVEL)
    return query(false, &info);
#else
    (void)info;
    return false;
#endif
}

}  // namespace loomwright
