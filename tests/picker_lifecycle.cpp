#include "mousepos.hpp"
#include "picker_xcb.hpp"
#include "xcb_connection.hpp"

#include <X11/Xlib.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace {
void previous_signal_handler(int) {}
int previous_xerror_handler(Display*, XErrorEvent*) { return 0; }
size_t fd_count() {
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) { (void)entry; ++count; }
    return count;
}
}

int main() {
    struct sigaction action {};
    action.sa_handler = previous_signal_handler;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGUSR1);
    if (sigaction(SIGINT, &action, nullptr) < 0 || sigaction(SIGTERM, &action, nullptr) < 0) return 1;
    XSetErrorHandler(previous_xerror_handler);
    const bool expect_disconnect = std::getenv("PICKER_EXPECT_DISCONNECT") != nullptr;
    const size_t before = fd_count();
    bool disconnected = false;
    std::unique_ptr<loomwright::XcbConnection> grab;
    try {
        if (expect_disconnect) {
            if (loomwright::pick_mouse_position_with_query_hook([&](const std::function<void()>& submit) {
                    // A checked server grab is processed before we submit the
                    // picker query on its *other* connection. Until the server
                    // dies it cannot reply to that client.
                    grab = std::make_unique<loomwright::XcbConnection>();
                    auto* connection = grab->get();
                    auto* error = xcb_request_check(connection, xcb_grab_server_checked(connection));
                    const bool failed = error != nullptr || grab->error() != 0;
                    std::free(error);
                    if (failed) throw std::runtime_error("could not grab private test server");
                    submit();
                    std::puts("query pending");
                    std::fflush(stdout);
                })) return 1;
        } else if (loomwright::pick_mouse_position()) {
            return 1;
        }
    } catch (const loomwright::PickerError& error) {
        if (!expect_disconnect || !std::string(error.what()).contains("X11 connection closed")) {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
        disconnected = true;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    grab.reset();
    if (expect_disconnect != disconnected || fd_count() != before) return 1;
    for (int signal : {SIGINT, SIGTERM}) {
        struct sigaction after {};
        if (sigaction(signal, nullptr, &after) < 0 || after.sa_handler != previous_signal_handler ||
            (after.sa_flags & SA_RESTART) == 0 || sigismember(&after.sa_mask, SIGUSR1) != 1) return 1;
    }
    const XErrorHandler restored = XSetErrorHandler(nullptr);
    if (restored != previous_xerror_handler) return 1;
    std::puts(disconnected ? "ok   picker disconnect releases fd and restores handlers" :
                             "ok   picker restores signal and X error handlers");
    return 0;
}
