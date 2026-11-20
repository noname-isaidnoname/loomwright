// Exercises GuiHost MappingNotify handling: runs an event loop while the test
// harness changes XKB layout via setxkbmap (triggering MappingNotify). Exits 0
// if the GUI stays alive and processes events without error.
#include "gui.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <poll.h>
#include <unistd.h>

int main() {
    loomwright::GuiPanel panel;
    panel.title = "Mapping probe";
    panel.widgets.push_back({loomwright::GuiWidget::Kind::Label, "label", "mapping probe", 0});
    panel.widgets.push_back({loomwright::GuiWidget::Kind::Input, "text", "", 1});

    auto host = loomwright::GuiHost::create(panel);
    if (!host) {
        std::fprintf(stderr, "failed to create GUI host\n");
        return 1;
    }
    host->set_visible(true);

    // Drive the GUI while the harness changes layouts. Acknowledge each
    // switch only once the focused input decodes the test key correctly
    // under the new map; just receiving a control byte is not readiness.
    std::puts("ready");
    std::fflush(stdout);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::string expected;
    int changes = 0;
    while (changes < 2) {
        const auto time_left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (time_left.count() <= 0) return 1;
        pollfd fds[] = {{host->fd(), POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
        const int ready = ::poll(fds, 2, static_cast<int>(time_left.count()));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return 1;
        host->process_events();
        if (host->is_closed()) {
            std::fprintf(stderr, "GUI closed unexpectedly during mapping probe\n");
            return 1;
        }
        if (fds[1].revents != 0) {
            char command;
            if (::read(STDIN_FILENO, &command, 1) != 1 || (command != 'a' && command != 'z')) return 1;
            expected.push_back(command);
        }
        if (expected.size() > static_cast<size_t>(changes) && host->value("text") == expected) {
            std::printf("change %d\n", ++changes);
            std::fflush(stdout);
        }
    }

    host->set_visible(false);
    host->close();
    std::puts("ok   GUI survives runtime MappingNotify/layout changes");
    return 0;
}
