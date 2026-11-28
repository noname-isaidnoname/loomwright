#include "runtime.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

#include "errors.hpp"
#include "subprocess.hpp"
#include "wayland_native.hpp"


namespace loomwright {
namespace {

std::string trim_line_endings(const std::string& text) {
    std::string value = text;
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' ||
                              value.back() == '\t')) {
        value.pop_back();
    }
    return value;
}

bool is_pid(const std::string& value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](char c) static {
               return std::isdigit(static_cast<unsigned char>(c)) != 0;
           });
}

/// Fill only the fields the earlier source did not provide. Both xdotool and
/// kdotool speak to a window manager; kdotool works on native KDE Wayland.
void fill_from_window_tool(const std::string& executable, std::map<std::string, std::string>& info) {
    const CommandResult active = run_command({executable, "getactivewindow"}, std::string(), 250);
    const std::string id = trim_line_endings(active.out);
    if (active.returncode != 0 || id.empty()) return;

    const auto query = [&](const char* option) {
        const CommandResult result = run_command({executable, option, id}, std::string(), 250);
        return result.returncode == 0 ? trim_line_endings(result.out) : std::string();
    };
    if (info["title"].empty()) info["title"] = query("getwindowname");
    if (info["class"].empty()) info["class"] = query("getwindowclassname");
    if (info["pid"].empty()) {
        const std::string pid = query("getwindowpid");
        if (is_pid(pid)) info["pid"] = pid;
    }
}

}  // namespace


Task Runtime::gui_loop(GuiHost* host) {
    while (host != nullptr && !host->is_closed() && !scheduler_.stop_requested()) {
        try {
            host->process_events();
        } catch (const std::exception& error) {
            runtime_error_ = std::string("GUI event handling failed: ") + error.what();
            say(runtime_error_);
            scheduler_.request_stop();
            co_return;
        }
        if (host->is_closed() || scheduler_.stop_requested()) break;
        const int fd = host->fd();
        if (fd < 0) break;
        try {
            co_await readable(fd);
        } catch (const TaskCancelled&) {
            throw;
        } catch (const std::exception& error) {
            if (host->is_closed() || scheduler_.stop_requested()) break;
            runtime_error_ = std::string("GUI connection failed: ") + error.what();
            say(runtime_error_);
            scheduler_.request_stop();
            break;
        }
    }
}

void Runtime::dispatch_gui_event(const GuiHost::Event& event) {
    const std::string edge = event.kind == GuiHost::EventKind::Click ? "clicked" : "close";
    const std::string key = event.kind == GuiHost::EventKind::Click ? event.widget_id : "window";
    bool handled = false;
    for (const auto& candidate : script_.events) {
        if (candidate.event_kind == "gui" && candidate.event_key == key && candidate.edge == edge) {
            handled = true;
            spawn_background(safe_dispatch(candidate, key, edge));
        }
    }
    if (!handled && event.kind == GuiHost::EventKind::Close) scheduler_.request_stop();
}

void Runtime::gui_alert(const std::string& message, const std::string& title) {
    const std::string executable = which("zenity");
    if (!executable.empty()) {
        const CommandResult result =
            run_command({executable, "--info", "--title=" + title, "--text=" + message}, std::string(), 120000);
        if (result.returncode != 0 && !result.spawn_failed) {
            throw RuntimeErrorLoomwright("GUI alert failed with exit code " + std::to_string(result.returncode));
        }
        return;
    }
    const std::string kdialog = which("kdialog");
    if (!kdialog.empty()) {
        const CommandResult result =
            run_command({kdialog, "--title", title, "--msgbox", message}, std::string(), 120000);
        if (result.returncode != 0 && !result.spawn_failed) {
            throw RuntimeErrorLoomwright("GUI alert failed with exit code " + std::to_string(result.returncode));
        }
        return;
    }
    const std::string yad = which("yad");
    if (!yad.empty()) {
        const CommandResult result =
            run_command({yad, "--info", "--title=" + title, "--text=" + message}, std::string(), 120000);
        if (result.returncode != 0 && !result.spawn_failed) {
            throw RuntimeErrorLoomwright("GUI alert failed with exit code " + std::to_string(result.returncode));
        }
        return;
    }
    throw RuntimeErrorLoomwright("GUI dialogs need zenity, kdialog, or yad.");
}

std::string Runtime::gui_input(const std::string& prompt, const std::string& default_value, const std::string& title) {
    const std::string executable = which("zenity");
    if (!executable.empty()) {
        const CommandResult result = run_command(
            {executable, "--entry", "--title=" + title, "--text=" + prompt, "--entry-text=" + default_value},
            std::string(), 120000);
        if (result.returncode != 0) return std::string();
        std::string value = result.out;
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
        return value;
    }
    const std::string kdialog = which("kdialog");
    if (!kdialog.empty()) {
        const CommandResult result =
            run_command({kdialog, "--title", title, "--inputbox", prompt, default_value}, std::string(), 120000);
        if (result.returncode != 0) return std::string();
        std::string value = result.out;
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
        return value;
    }
    const std::string yad = which("yad");
    if (!yad.empty()) {
        const CommandResult result = run_command(
            {yad, "--entry", "--title=" + title, "--text=" + prompt, "--entry-text=" + default_value},
            std::string(), 120000);
        if (result.returncode != 0) return std::string();
        std::string value = result.out;
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
        return value;
    }
    throw RuntimeErrorLoomwright("GUI input needs zenity, kdialog, or yad.");
}

bool Runtime::gui_confirm(const std::string& question, const std::string& title) {
    const std::string executable = which("zenity");
    if (!executable.empty()) {
        const CommandResult result =
            run_command({executable, "--question", "--title=" + title, "--text=" + question}, std::string(), 120000);
        return result.returncode == 0;
    }
    const std::string kdialog = which("kdialog");
    if (!kdialog.empty()) {
        const CommandResult result =
            run_command({kdialog, "--title", title, "--yesno", question}, std::string(), 120000);
        return result.returncode == 0;
    }
    const std::string yad = which("yad");
    if (!yad.empty()) {
        const CommandResult result =
            run_command({yad, "--question", "--title=" + title, "--text=" + question}, std::string(), 120000);
        return result.returncode == 0;
    }
    throw RuntimeErrorLoomwright("GUI confirmation needs zenity, kdialog, or yad.");
}

std::map<std::string, std::string> Runtime::active_window_info() const {
    std::map<std::string, std::string> info;
    info["title"] = std::string();
    info["class"] = std::string();
    info["pid"] = std::string();
    info["process"] = std::string();

    const char* session = std::getenv("XDG_SESSION_TYPE");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    const bool wayland_session =
        (session != nullptr && std::string(session) == "wayland") ||
        (session == nullptr && wayland != nullptr && wayland[0] != '\0');
    if (wayland_session) (void)wayland_active_window_info(info);

    for (const char* tool : {"xdotool", "kdotool"}) {
        const std::string executable = which(tool);
        if (!executable.empty()) fill_from_window_tool(executable, info);
    }

    if (!info["pid"].empty()) {
        std::ifstream comm("/proc/" + info["pid"] + "/comm");
        std::string process;
        comm >> process;
        if (!comm.fail()) info["process"] = process;
    }
    return info;
}

}  // namespace loomwright
