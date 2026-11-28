#include "wayland_native.hpp"

#include "mousepos.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

#include <poll.h>

#ifdef LOOMWRIGHT_WAYLAND_GUI
#include <gtk/gtk.h>
#include <gdk/gdkwayland.h>

#include "runtime.hpp"
#endif

namespace loomwright {
namespace {

// One message per send: a type byte followed by an optional text payload.
[[maybe_unused]] bool send_picker_message(int fd, char type, const std::string& payload = {}) {
    if (payload.size() > 4096) return false;
    std::string packet(1, type);
    packet += payload;
    ssize_t written;
    do { written = ::send(fd, packet.data(), packet.size(), MSG_NOSIGNAL | MSG_DONTWAIT); }
    while (written < 0 && errno == EINTR);
    return written == static_cast<ssize_t>(packet.size());
}

// 1 = message, 0 = peer closed, -1 = no data yet, -2 = malformed.
int receive_picker_message(int fd, char& type, std::string& payload) {
    char packet[4096];
    ssize_t count;
    do { count = ::recv(fd, packet, sizeof(packet), MSG_DONTWAIT); } while (count < 0 && errno == EINTR);
    if (count == 0) return 0;
    if (count < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? -1 : -2;
    if (count < 1) return -2;
    type = packet[0];
    payload.assign(packet + 1, static_cast<size_t>(count) - 1);
    return 1;
}

std::optional<std::pair<int, int>> parse_coordinates(const std::string& text) {
    const size_t comma = text.find(',');
    if (comma == std::string::npos) return std::nullopt;
    try {
        size_t used = 0;
        const int x = std::stoi(text.substr(0, comma), &used);
        if (used != comma) return std::nullopt;
        const std::string rest = text.substr(comma + 1);
        const int y = std::stoi(rest, &used);
        if (used != rest.size()) return std::nullopt;
        return std::pair{std::max(0, x), std::max(0, y)};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

volatile sig_atomic_t picker_cancelled = 0;
void picker_signal(int) { picker_cancelled = 1; }

struct SignalGuard {
    struct sigaction old_int {};
    struct sigaction old_term {};
    bool have_int = false;
    bool have_term = false;

    SignalGuard() {
        struct sigaction action {};
        action.sa_handler = picker_signal;
        sigemptyset(&action.sa_mask);
        picker_cancelled = 0;
        if (sigaction(SIGINT, &action, &old_int) < 0) throw PickerError("Could not install picker SIGINT handler.");
        have_int = true;
        if (sigaction(SIGTERM, &action, &old_term) < 0) {
            sigaction(SIGINT, &old_int, nullptr);
            have_int = false;
            throw PickerError("Could not install picker SIGTERM handler.");
        }
        have_term = true;
    }
    ~SignalGuard() {
        if (have_term) sigaction(SIGTERM, &old_term, nullptr);
        if (have_int) sigaction(SIGINT, &old_int, nullptr);
    }
};

#ifdef LOOMWRIGHT_WAYLAND_GUI

struct ScreenshotGuard {
    std::string path;
    ~ScreenshotGuard() { if (!path.empty()) ::unlink(path.c_str()); }
};

class WaylandPicker {
public:
    WaylandPicker(int fd, GdkDisplay* display, const std::string& screenshot, bool verbose)
        : fd_(fd), display_(display), verbose_(verbose) {
        GError* error = nullptr;
        pixbuf_ = gdk_pixbuf_new_from_file(screenshot.c_str(), &error);
        if (!pixbuf_) {
            const std::string detail = error != nullptr && error->message != nullptr ? error->message : "unknown error";
            g_clear_error(&error);
            throw PickerError("Could not decode the desktop screenshot: " + detail);
        }
        const auto bounds = detect_screen_size();
        screen_width_ = std::max(1, bounds.first);
        screen_height_ = std::max(1, bounds.second);
        const int monitors = gdk_display_get_n_monitors(display_);
        int width = 0;
        int height = 0;
        for (int i = 0; i < monitors; ++i) {
            GdkRectangle geometry{};
            gdk_monitor_get_geometry(gdk_display_get_monitor(display_, i), &geometry);
            width = std::max(width, geometry.x + geometry.width);
            height = std::max(height, geometry.y + geometry.height);
        }
        if (width > 0 && height > 0) {
            screen_width_ = width;
            screen_height_ = height;
        }
        if (verbose_)
            std::cerr << "[loomwright] picker overlay: native Wayland; screen bounds: " << screen_width_ << "x"
                      << screen_height_ << "\n";
        pointer_ = {screen_width_ / 2, screen_height_ / 2};
        text_ = format_coordinates(pointer_.first, pointer_.second);
    }

    ~WaylandPicker() {
        if (window_) gtk_widget_destroy(window_);
        if (pixbuf_) g_object_unref(pixbuf_);
    }

    void run() {
        window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(window_), "Loomwright mouse position");
        gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
        gtk_window_set_keep_above(GTK_WINDOW(window_), TRUE);
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window_), TRUE);
        area_ = gtk_drawing_area_new();
        gtk_widget_add_events(area_, GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK | GDK_KEY_PRESS_MASK);
        gtk_widget_set_can_focus(area_, TRUE);
        gtk_container_add(GTK_CONTAINER(window_), area_);
        g_signal_connect(window_, "map-event", G_CALLBACK(on_map), this);
        g_signal_connect(window_, "key-press-event", G_CALLBACK(on_key), this);
        g_signal_connect(area_, "draw", G_CALLBACK(on_draw), this);
        g_signal_connect(area_, "button-press-event", G_CALLBACK(on_button), this);
        g_signal_connect(area_, "motion-notify-event", G_CALLBACK(on_motion), this);
        gtk_widget_show_all(window_);
        gtk_window_fullscreen(GTK_WINDOW(window_));
        gtk_widget_grab_focus(area_);
        gtk_window_present(GTK_WINDOW(window_));

        // Deterministic hooks for the Weston integration test. They feed the
        // same state changes as real GDK events; no input injection is needed.
        if (const char* motion = std::getenv("LOOMWRIGHT_PICKER_TEST_MOTION"))
            add_test_hook(200, on_test_motion, motion);
        const char* click = std::getenv("LOOMWRIGHT_PICKER_TEST_CLICK");
        const char* cancel = std::getenv("LOOMWRIGHT_PICKER_TEST_CANCEL");
        if (click && *click) add_test_hook(400, on_test_click, click);
        else if (cancel && std::string(cancel) == "1") add_test_hook(400, on_test_cancel, "1");

        gtk_main();
    }

private:
    struct TestHook {
        WaylandPicker* picker;
        std::string value;
    };

    using HookFunction = gboolean (*)(gpointer);

    void add_test_hook(guint delay, HookFunction function, const char* value) {
        auto hook = std::make_unique<TestHook>(TestHook{this, value != nullptr ? value : ""});
        TestHook* raw = hook.get();
        hooks_.push_back(std::move(hook));
        g_timeout_add(delay, function, raw);
    }

    void update_monitor() {
        GdkWindow* window = gtk_widget_get_window(window_);
        if (window == nullptr) return;
        GdkMonitor* monitor = gdk_display_get_monitor_at_window(display_, window);
        if (monitor == nullptr) return;
        GdkRectangle geometry{};
        gdk_monitor_get_geometry(monitor, &geometry);
        monitor_x_ = geometry.x;
        monitor_y_ = geometry.y;
    }

    std::pair<int, int> to_global(double x, double y) const {
        return clamp_to_screen(static_cast<int>(x) + monitor_x_, static_cast<int>(y) + monitor_y_, screen_width_,
                               screen_height_);
    }

    void set_pointer(const std::pair<int, int>& point) {
        const auto bounded = clamp_to_screen(point.first, point.second, screen_width_, screen_height_);
        const std::string next = format_coordinates(bounded.first, bounded.second);
        if (next == text_ && bounded == pointer_) return;
        pointer_ = bounded;
        text_ = next;
        if (area_ != nullptr) gtk_widget_queue_draw(area_);
    }

    void draw(cairo_t* cr) {
        GtkAllocation allocation{};
        gtk_widget_get_allocation(area_, &allocation);
        cairo_set_source_rgb(cr, 0.05, 0.05, 0.05);
        cairo_paint(cr);
        const int image_width = gdk_pixbuf_get_width(pixbuf_);
        const int image_height = gdk_pixbuf_get_height(pixbuf_);
        if (image_width > 0 && image_height > 0 && allocation.width > 0 && allocation.height > 0) {
            cairo_save(cr);
            cairo_scale(cr, static_cast<double>(allocation.width) / static_cast<double>(image_width),
                        static_cast<double>(allocation.height) / static_cast<double>(image_height));
            gdk_cairo_set_source_pixbuf(cr, pixbuf_, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
        }
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 16);
        cairo_text_extents_t extents{};
        cairo_text_extents(cr, text_.c_str(), &extents);
        const int tip_width = static_cast<int>(extents.width) + 24;
        const int tip_height = static_cast<int>(extents.height) + 16;
        const auto where = tooltip_position(pointer_.first, pointer_.second, screen_width_, screen_height_, tip_width,
                                            tip_height);
        const double x = static_cast<double>(where.first - monitor_x_);
        const double y = static_cast<double>(where.second - monitor_y_);
        cairo_rectangle(cr, x, y, tip_width, tip_height);
        cairo_set_source_rgb(cr, 0.07, 0.07, 0.07);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.96, 0.96, 0.96);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        cairo_set_source_rgb(cr, 0.96, 0.96, 0.96);
        cairo_move_to(cr, x + 12, y + static_cast<double>(tip_height) / 2 + extents.height / 2 - extents.y_bearing / 2);
        cairo_show_text(cr, text_.c_str());
    }

    void finish(const std::pair<int, int>& point) {
        if (finished_) return;
        finished_ = true;
        const auto bounded = clamp_to_screen(point.first, point.second, screen_width_, screen_height_);
        (void)send_picker_message(fd_, 'P', format_coordinates(bounded.first, bounded.second));
        gtk_main_quit();
    }

    void cancel() {
        if (finished_) return;
        finished_ = true;
        (void)send_picker_message(fd_, 'C');
        gtk_main_quit();
    }

    static gboolean on_map(GtkWidget*, GdkEventAny*, gpointer data) {
        auto* self = static_cast<WaylandPicker*>(data);
        self->update_monitor();
        (void)send_picker_message(self->fd_, 'R');
        return FALSE;
    }
    static gboolean on_key(GtkWidget*, GdkEventKey* event, gpointer data) {
        if (event->keyval != GDK_KEY_Escape) return FALSE;
        static_cast<WaylandPicker*>(data)->cancel();
        return TRUE;
    }
    static gboolean on_draw(GtkWidget*, cairo_t* cr, gpointer data) {
        static_cast<WaylandPicker*>(data)->draw(cr);
        return FALSE;
    }
    static gboolean on_button(GtkWidget*, GdkEventButton* event, gpointer data) {
        auto* self = static_cast<WaylandPicker*>(data);
        if (event->type != GDK_BUTTON_PRESS) return FALSE;
        if (event->button == 1) self->finish(self->to_global(event->x, event->y));
        else self->cancel();
        return TRUE;
    }
    static gboolean on_motion(GtkWidget*, GdkEventMotion* event, gpointer data) {
        auto* self = static_cast<WaylandPicker*>(data);
        self->set_pointer(self->to_global(event->x, event->y));
        return TRUE;
    }
    static gboolean on_test_motion(gpointer data) {
        auto* hook = static_cast<TestHook*>(data);
        const auto point = parse_coordinates(hook->value);
        if (point) hook->picker->set_pointer(*point);
        return FALSE;
    }
    static gboolean on_test_click(gpointer data) {
        auto* hook = static_cast<TestHook*>(data);
        const auto point = parse_coordinates(hook->value);
        if (point) hook->picker->finish(*point);
        return FALSE;
    }
    static gboolean on_test_cancel(gpointer data) {
        static_cast<TestHook*>(data)->picker->cancel();
        return FALSE;
    }

    int fd_;
    GdkDisplay* display_;
    bool verbose_;
    GtkWidget* window_ = nullptr;
    GtkWidget* area_ = nullptr;
    GdkPixbuf* pixbuf_ = nullptr;
    int screen_width_ = 0;
    int screen_height_ = 0;
    int monitor_x_ = 0;
    int monitor_y_ = 0;
    std::pair<int, int> pointer_{0, 0};
    std::string text_;
    bool finished_ = false;
    std::vector<std::unique_ptr<TestHook>> hooks_;
};

int run_picker_child(int fd, bool verbose) {
    gdk_set_allowed_backends("wayland");
    int argc = 0;
    char** argv = nullptr;
    if (!gtk_init_check(&argc, &argv)) {
        (void)send_picker_message(fd, 'E', "Cannot open a native Wayland display for the mouse position picker.");
        return 1;
    }
    GdkDisplay* display = gdk_display_get_default();
    if (!GDK_IS_WAYLAND_DISPLAY(display)) {
        (void)send_picker_message(fd, 'E',
                                  "The mouse position picker needs a native Wayland display (not X11/XWayland).");
        return 1;
    }
    ScreenshotGuard screenshot;
    CaptureResult capture;
    try {
        capture = capture_desktop();
        screenshot.path = capture.path;
        if (verbose) std::cerr << "[loomwright] picker screenshot: " << capture.tool << "\n";
        WaylandPicker picker(fd, display, capture.path, verbose);
        picker.run();
        return 0;
    } catch (const std::exception& error) {
        (void)send_picker_message(fd, 'E', error.what());
        return 1;
    }
}

#else

int run_picker_child(int, bool) { return 1; }

#endif  // LOOMWRIGHT_WAYLAND_GUI

}  // namespace

bool wayland_picker_available() {
#ifdef LOOMWRIGHT_WAYLAND_GUI
    return true;
#else
    return false;
#endif
}

std::optional<std::pair<int, int>> pick_mouse_position_wayland(bool verbose) {
    if (!wayland_picker_available())
        throw PickerError(
            "Native Wayland picker is unavailable: rebuild with GTK3 and wayland-client development files.");

    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) < 0)
        throw PickerError(std::string("Could not create the Wayland picker socket: ") + std::strerror(errno));
    const pid_t parent = ::getpid();
    const pid_t child = ::fork();
    if (child < 0) {
        const int error = errno;
        ::close(sockets[0]);
        ::close(sockets[1]);
        throw PickerError(std::string("Could not start the Wayland picker: ") + std::strerror(error));
    }
    if (child == 0) {
        ::close(sockets[0]);
        // GTK exits on a broken Wayland connection; keep it in this helper.
        ::unsetenv("WAYLAND_SOCKET");
        (void)::prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (::getppid() != parent) _exit(1);
        sigset_t empty;
        sigemptyset(&empty);
        ::sigprocmask(SIG_SETMASK, &empty, nullptr);
        ::signal(SIGTERM, SIG_DFL);
        ::signal(SIGINT, SIG_IGN);
        const int result = run_picker_child(sockets[1], verbose);
        ::close(sockets[1]);
        _exit(result);
    }
    ::close(sockets[1]);

    SignalGuard signals;
    std::optional<std::pair<int, int>> result;
    std::string error;
    bool ready = false;
    bool done = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kOverlayTimeoutMs + 5000);
    while (!done) {
        if (picker_cancelled) break;
        pollfd item{sockets[0], POLLIN, 0};
        int wait_ms = 100;
        if (!ready) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) { error = "The native Wayland picker never showed its overlay."; break; }
            wait_ms = static_cast<int>(std::min<int>(100, static_cast<int>(remaining)));
        }
        const int ready_fd = ::poll(&item, 1, wait_ms);
        if (ready_fd < 0 && errno == EINTR) continue;
        if (ready_fd == 0) continue;
        char type = 0;
        std::string payload;
        const int received = receive_picker_message(sockets[0], type, payload);
        if (received == 0 || received == -2) {
            error = "The native Wayland picker closed before reporting a position.";
            break;
        }
        if (received < 0) continue;
        switch (type) {
            case 'R':
                ready = true;
                break;
            case 'P':
                result = parse_coordinates(payload);
                if (!result) error = "The native Wayland picker reported invalid coordinates.";
                done = true;
                break;
            case 'C':
                done = true;
                break;
            case 'E':
                error = payload.empty() ? "The native Wayland picker failed to start." : payload;
                done = true;
                break;
            default:
                error = "The native Wayland picker sent an unknown message.";
                done = true;
                break;
        }
    }
    if (picker_cancelled) {
        ::kill(child, SIGTERM);
        result.reset();
    } else if (!done && error.empty()) {
        ::kill(child, SIGTERM);
        error = "The native Wayland picker stopped without a result.";
    }
    ::close(sockets[0]);

    int status = 0;
    for (int i = 0; i < 200; ++i) {
        const pid_t waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child || (waited < 0 && errno == ECHILD)) break;
        ::usleep(1000);
    }
    if (::kill(child, 0) == 0) {
        ::kill(child, SIGKILL);
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
    if (!error.empty()) throw PickerError(error);
    return result;
}

}  // namespace loomwright
