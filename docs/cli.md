# CLI

`src/cli.cpp` parses arguments, dispatches one command, and maps failures to
exit codes. `src/main.cpp` is a thin wrapper around `run_cli()`.

## Surface

```text
loomwright [-h] [--init SCRIPT | --check PATH | --run PATH | --docs [TOPIC] | --devices |
               --broker | --screen | --get-mouse-pos | --list-api | --doctor]
               [--force] [--dry-run] [--input-device EVENT_PATH]
               [--input-mode {auto,direct,broker}]
               [--output-backend {auto,ydotool,uinput}] [--screen-size WIDTHxHEIGHT] [--verbose]
               [--version] [script]
```

A positional `script` is shorthand for `--run PATH`. Only one mode flag is
allowed per invocation; `--force`, `--dry-run`, `--verbose`,
`--input-device`, `--input-mode`, `--output-backend` and `--screen-size` modify the mode.
`--docs` takes an optional topic and treats the next non-flag token as the
topic.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Success (including `--run` interrupted by the panic key/Ctrl+C) |
| 1 | Runtime failure while running a script |
| 2 | Usage error, unreadable path, parse error, checker error, `--init` failure, picker startup failure |
| 130 | Mouse-position pick cancelled |

Bad paths and parse errors print `loomwright: ...` and return 2, not a traceback or
exit 1. Runtime failures print `loomwright: ...` and return 1.

## Commands

### `--init SCRIPT`

Creates a starter `.loom` file (a `loom` block with `F8` down/up handlers).
Refuses to overwrite an existing file unless `--force` is passed. Returns 2 on
a write failure or when refusing to overwrite.

### `--check PATH`

Parses and statically checks the script. It must not open input devices or
require permissions. It prints diagnostics and returns 2 on errors, 0 on
success.

### `--run PATH`

Parses, checks, and refuses to run when the checker reports errors. Otherwise
it builds a `Runtime`, spawns `runtime.run()` onto the scheduler, and drives the
loop. After the loop it rethrows `runtime.failure()` if one was recorded.
`--dry-run` logs output actions instead of injecting them and observes physical
keys without requesting suppression. `--input-device`, `--output-backend`,
`--screen-size` and `--verbose` override the script/CLI options. `--input-mode
auto` (the default) and `--input-mode broker` both use shared input and shared
synthetic key/button injection; they do not silently fall back to a direct
reader on broker errors. Overlapping key/button holds are injected once by the
broker and released after the last owner frees them. `--input-mode direct`
opts out for standalone use. A direct reader does not receive input from a
device grabbed by another process, including the broker.

### `--broker`

Queries the running broker through a read-only `Status` packet and prints the
connected scripts (name, subscribed devices, consumed keys, panic exemptions
and shared holds), the watched devices (path, name, kind, grab state and keys
currently down), claimed keys and conflicts. It never starts the broker or
changes claims: when no compatible broker is listening it prints that the
broker is not running and returns 0. A protocol or socket error prints
`loomwright: ...` and returns 2.

A conflict is either two scripts consuming the same key code on the same device
(`consume`), or two scripts holding the same synthetic key/button (`hold`,
coordinated to a single injection by the broker). Both forms name every script
and the contested device, or "synthetic output" for holds. The report is sorted
so it can be diffed or grepped.

### `--docs [TOPIC]`, `--list-api`

`--docs` renders `kGuide` from `src/docs.cpp`, or one topic. `--list-api`
prints the `api_reference()` groups, including the `cli` group.

### `--devices`, `--screen`, `--doctor`

`--devices` opens each `/dev/input/event*` it can read and prints its name and
kind (keyboard/mouse/other). `--screen` prints the detected desktop bounds.
`--doctor` prints the detected session type, desktop bounds, accessible evdev
devices, broker status, ydotool/ydotoold, `/dev/uinput`, the picker backend,
the active-window sources, X11, screenshot tools and clipboard tools. It does
not start the broker. Use `--broker` for the detailed per-script and per-device
view.

`active_window_title()` and `active_window_class()` first use the compositor's
`zwlr_foreign_toplevel_manager_v1` in a Wayland session. `xdotool` and
`kdotool` then fill any missing fields, including the PID used by
`active_window_pid()` and `active_window_process()`. The protocol has no PID
and is optional: with neither it nor a query tool, the helpers return empty
values rather than failing the script.

## Mouse-position picker

`--get-mouse-pos` never needs evdev. The backend is detected at runtime by
`choose_picker_backend()`:

| Session | Native support | Backend |
| --- | --- | --- |
| Wayland (`XDG_SESSION_TYPE=wayland`, or no session type with `WAYLAND_DISPLAY`) | GTK3 + wayland-client | native Wayland picker (`src/picker_wayland.cpp`) |
| X11 (`DISPLAY` set) | any | XCB picker (`src/picker_xcb.cpp`) |
| Neither | — | exit 2 with a clear message |

An X11 session that also has `WAYLAND_DISPLAY` still uses XCB. A Wayland
session without native build support falls back to XCB when `DISPLAY` is set
(XWayland); without either display it fails with an installation hint instead
of crashing. `WAYLAND_SOCKET` belongs to the caller, so both native paths use a
named `WAYLAND_DISPLAY`.

Flow (both backends):

1. Capture the desktop with spectacle, grim, gnome-screenshot, scrot, or
   ImageMagick `import`.
2. Show that still image in a full-screen **managed** window. XCB uses a
   splash window with `_NET_WM_STATE_FULLSCREEN`; Wayland uses a fullscreen
   GTK3 toplevel.
3. Follow the pointer with a live coordinate tooltip; on Wayland the tooltip is
   drawn on the same overlay because there is no override-redirect surface.
4. Left-click prints `x, y` and returns 0. Right-click (or Esc when focused)
   returns 130.

The opaque, managed X11 splash window is what makes clicks over native Wayland
windows land on the picker; an override-redirect or transparent window does not
capture them. Native Wayland has no global pointer query, so the overlay reads
pointer events on its own fullscreen surface and adds the output's origin to
get coordinates for the script's screen space.

GTK exits on a broken Wayland connection, so the native picker runs GTK3 in a
forked helper and reports the result over a private `SOCK_SEQPACKET` socket.
The parent cancels on SIGINT/SIGTERM and never runs GTK itself.

Keep the pure helpers testable without opening a display:
`format_coordinates`, `clamp_to_screen`, `tooltip_position`,
`choose_picker_backend`, `available_screenshot_tools` and
`screenshot_commands` all take injectable finders or none. `--get-mouse-pos`
itself is tested through `run_cli(args, picker_fn)`; pass a stub picker, never
the real overlay. The Weston integration test drives the real overlay through
the deterministic `LOOMWRIGHT_PICKER_TEST_{MOTION,CLICK,CANCEL}` hooks.

## Testing the CLI

`run_cli(const std::vector<std::string>&, const PickerFunction&)` is the seam
used by `tests/test_language.cpp`. `PickerFunction` returns a position,
`std::nullopt` for a cancel, or throws `PickerError` for a startup failure. An
empty function calls the real picker. `parse_cli`, `usage_text`, `help_text`
and `version_string` are directly testable. `read_broker_status()` is the seam
for `--broker`: it returns `std::nullopt` when no broker is listening. The pure
`broker_conflicts()` and `format_broker_status()` helpers take a `BrokerStatus`
snapshot, so conflict naming and output can be tested without a live broker;
`tests/output_broker.cpp` and `tests/shared_input_cli.sh` exercise the real
exchange.
