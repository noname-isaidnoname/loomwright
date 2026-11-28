#include "mousepos.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "subprocess.hpp"
#include "wayland_native.hpp"

namespace loomwright {
namespace {
const char* const kScreenshotTools[] = {"spectacle", "grim", "gnome-screenshot", "scrot", "import"};

bool has_value(const char* value) { return value != nullptr && value[0] != '\0'; }
}

std::string format_coordinates(int x, int y) {
    return std::to_string(x) + ", " + std::to_string(y);
}

std::pair<int, int> clamp_to_screen(int x, int y, int width, int height) {
    const int bounded_width = std::max(1, width);
    const int bounded_height = std::max(1, height);
    return {std::max(0, std::min(bounded_width - 1, x)), std::max(0, std::min(bounded_height - 1, y))};
}

std::pair<int, int> tooltip_position(int x, int y, int screen_width, int screen_height, int tip_width,
                                     int tip_height, int offset) {
    const int bounded_width = std::max(1, screen_width);
    const int bounded_height = std::max(1, screen_height);
    const int bounded_tip_width = std::max(1, tip_width);
    const int bounded_tip_height = std::max(1, tip_height);
    int tip_x = x + offset;
    int tip_y = y + offset;
    if (tip_x + bounded_tip_width > bounded_width) tip_x = x - bounded_tip_width - offset;
    if (tip_y + bounded_tip_height > bounded_height) tip_y = y - bounded_tip_height - offset;
    tip_x = std::max(0, std::min(bounded_width - bounded_tip_width, tip_x));
    tip_y = std::max(0, std::min(bounded_height - bounded_tip_height, tip_y));
    return {tip_x, tip_y};
}

std::vector<std::string> available_screenshot_tools(const ExecutableFinder& finder) {
    const ExecutableFinder lookup = finder ? finder : ExecutableFinder([](const std::string& name) { return which(name); });
    std::vector<std::string> tools;
    for (const char* name : kScreenshotTools) if (!lookup(name).empty()) tools.emplace_back(name);
    return tools;
}

std::vector<std::vector<std::string>> screenshot_commands(const std::string& path, const ExecutableFinder& finder) {
    std::vector<std::vector<std::string>> commands;
    for (const auto& name : available_screenshot_tools(finder)) {
        if (name == "spectacle") commands.push_back({"spectacle", "-b", "-n", "-f", "--scaled", "-o", path});
        else if (name == "grim") commands.push_back({"grim", path});
        else if (name == "gnome-screenshot") commands.push_back({"gnome-screenshot", "-f", path});
        else if (name == "scrot") commands.push_back({"scrot", path});
        else if (name == "import") commands.push_back({"import", "-window", "root", path});
    }
    return commands;
}

CaptureResult capture_desktop(const ExecutableFinder& finder) {
    char template_path[] = "/tmp/loomwright-picker-XXXXXX.png";
    const int handle = ::mkstemps(template_path, 4);
    if (handle < 0) throw PickerError(std::string("Could not create a screenshot file: ") + std::strerror(errno));
    ::close(handle);
    const std::string path = template_path;
    const auto commands = screenshot_commands(path, finder);
    if (commands.empty()) {
        ::unlink(path.c_str());
        throw PickerError("The mouse position picker needs a screenshot tool so the desktop stays visible while you pick. "
                          "Install spectacle (KDE), grim (wlroots), gnome-screenshot, scrot, or ImageMagick's import.");
    }
    std::vector<std::string> failures;
    for (const auto& argv : commands) {
        const CommandResult result = run_command(argv, std::string(), kScreenshotTimeoutMs);
        if (result.spawn_failed || result.timed_out) {
            failures.push_back(argv.front() + ": " + (result.spawn_failed ? result.spawn_error : "timed out"));
            continue;
        }
        if (result.returncode == 0 && ::access(path.c_str(), R_OK) == 0) return CaptureResult{path, argv.front()};
        std::string detail = result.err;
        while (!detail.empty() && (detail.back() == '\n' || detail.back() == ' ')) detail.pop_back();
        failures.push_back(argv.front() + ": " + (detail.empty() ? "exit " + std::to_string(result.returncode) : detail));
    }
    ::unlink(path.c_str());
    std::string detail;
    for (const auto& failure : failures) {
        if (!detail.empty()) detail += "; ";
        detail += failure;
    }
    throw PickerError("Could not capture the desktop for the mouse position picker. " + detail);
}

PickerBackend choose_picker_backend(const char* session, const char* wayland_display, const char* display,
                                    bool native_wayland_available) {
    const bool wayland_session = (session != nullptr && std::string(session) == "wayland") ||
                                 (session == nullptr && has_value(wayland_display));
    if (native_wayland_available && (wayland_session || (!has_value(display) && has_value(wayland_display))))
        return PickerBackend::Wayland;
    if (has_value(display)) return PickerBackend::X11;
    return PickerBackend::None;
}

std::optional<std::pair<int, int>> pick_mouse_position_xcb(bool verbose);

std::optional<std::pair<int, int>> pick_mouse_position(bool verbose) {
    const PickerBackend backend =
        choose_picker_backend(std::getenv("XDG_SESSION_TYPE"), std::getenv("WAYLAND_DISPLAY"), std::getenv("DISPLAY"),
                              wayland_picker_available());
    switch (backend) {
        case PickerBackend::Wayland:
            return pick_mouse_position_wayland(verbose);
        case PickerBackend::X11:
            return pick_mouse_position_xcb(verbose);
        case PickerBackend::None:
            break;
    }
    throw PickerError(
        "Cannot open a display for the mouse position picker. Use an X11 session (DISPLAY set; XWayland is enough) or "
        "a native Wayland session (WAYLAND_DISPLAY set) built with GTK3 and wayland-client, or use loomwright --screen "
        "to calibrate coordinates manually.");
}

}  // namespace loomwright
