#!/usr/bin/env bash
# Exercise the native Wayland picker and active-window queries on a private
# headless Weston compositor with no X display at all. The screenshot tool is
# stubbed so this works on compositors without a screencopy protocol; the
# picker itself is the real GTK3 overlay, and its input is driven by the
# deterministic test hooks in src/picker_wayland.cpp.
#
# Exit codes: 0 pass, 1 failure, 77 skipped (no Weston or no built binary).
set -euo pipefail

command -v weston >/dev/null || { echo 'SKIP: weston is not installed'; exit 77; }
command -v base64 >/dev/null || { echo 'SKIP: base64 is not installed'; exit 77; }
BIN=${LOOMWRIGHT_BIN:-}
[[ -n $BIN && -x $BIN ]] || { echo 'FAIL: set LOOMWRIGHT_BIN to the built loomwright binary' >&2; exit 1; }

dir=$(mktemp -d "${TMPDIR:-/tmp}/loomwright-wayland-native.XXXXXXXX")
server= app= stage='starting Weston'
cleanup() {
    local result=$?
    if [[ $result -ne 0 && $result -ne 77 ]]; then
        echo "Native Wayland integration failed during: $stage" >&2
        cat "$dir/out" "$dir/doctor" "$dir/script.loom" "$dir/weston.log" >&2 2>/dev/null || true
    fi
    [[ -z $app ]] || kill "$app" 2>/dev/null || true
    [[ -z $app ]] || wait "$app" 2>/dev/null || true
    [[ -z $server ]] || kill "$server" 2>/dev/null || true
    [[ -z $server ]] || wait "$server" 2>/dev/null || true
    rm -rf "$dir"
}
trap cleanup EXIT
chmod 700 "$dir"

export XDG_RUNTIME_DIR=$dir XDG_SESSION_TYPE=wayland WAYLAND_DISPLAY=loomwright-native
unset DISPLAY WAYLAND_SOCKET
weston --backend=headless-backend.so --socket="$WAYLAND_DISPLAY" --idle-time=0 \
    --log="$dir/weston.log" >/dev/null 2>&1 &
server=$!
for _ in $(seq 1 400); do
    [[ -S $dir/$WAYLAND_DISPLAY ]] && break
    kill -0 "$server" 2>/dev/null || { cat "$dir/weston.log" >&2; exit 1; }
    sleep 0.05
done
[[ -S $dir/$WAYLAND_DISPLAY ]] || { cat "$dir/weston.log" >&2; exit 1; }

# A valid but tiny PNG. grim is the only screenshot tool on PATH, so the
# picker uses it and we do not need a real screencopy-capable compositor.
base64 -d >"$dir/shot.png" <<'PNG'
iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAAaklEQVR4nAXB0QAAAAhDwZGMJJJI+nwUkYwkou4kYVGixYgVESckY1OmzZg1MWekwkUVXUyxRYorpMZNNd1Ms02aa6TBQw09zLBDhhukxUstvcyyS5ZbpOBQocOEDQkXpMNHHX3MsUeOOx6wLFSBm6x5zwAAAABJRU5ErkJggg==
PNG
mkdir "$dir/bin"
cat >"$dir/bin/capture_stub" <<EOF
#!/usr/bin/env bash
for last; do :; done
cp "$dir/shot.png" "\$last"
EOF
chmod +x "$dir/bin/capture_stub"
# Stub every known screenshot tool: the first one on PATH wins and a real
# tool might fail on a headless compositor.
for tool in spectacle grim gnome-screenshot scrot import; do
    ln -s capture_stub "$dir/bin/$tool"
done
export PATH="$dir/bin:$PATH"

stage='doctor'
"$BIN" --doctor >"$dir/doctor" 2>&1
grep -q 'Mouse picker: native Wayland' "$dir/doctor"
grep -q 'Active window sources: no native Wayland toplevel protocol' "$dir/doctor"

stage='active-window builtins'
cat >"$dir/script.loom" <<'EOF'
loom "native-query" {
    when app starts {
        log "native-title=[" + active_window_title() + "]"
        log "native-class=[" + active_window_class() + "]"
        log "native-process=[" + active_window_process() + "]"
        if active_window_pid() == 0 { log "native-pid=0" }
    }
}
EOF
"$BIN" --run "$dir/script.loom" --dry-run --input-mode direct >"$dir/out" 2>&1 &
app=$!
for _ in $(seq 1 400); do
    grep -q 'native-pid=0' "$dir/out" && break
    kill -0 "$app" 2>/dev/null || break
    sleep 0.05
done
grep -q 'native-title=\[\]' "$dir/out"
grep -q 'native-class=\[\]' "$dir/out"
grep -q 'native-process=\[\]' "$dir/out"
grep -q 'native-pid=0' "$dir/out"
# Non-interactive shells can start background jobs with SIGINT ignored.
kill -TERM "$app"
status=0
wait "$app" || status=$?
app=
[[ $status -eq 0 || $status -eq 130 ]]

stage='native picker click'
LOOMWRIGHT_PICKER_TEST_MOTION=120,80 LOOMWRIGHT_PICKER_TEST_CLICK=120,80 \
    "$BIN" --get-mouse-pos --verbose >"$dir/out" 2>&1
grep -q 'picker overlay: native Wayland' "$dir/out"
grep -qx '120, 80' "$dir/out"

stage='native picker cancel'
status=0
LOOMWRIGHT_PICKER_TEST_CANCEL=1 "$BIN" --get-mouse-pos >"$dir/out" 2>&1 || status=$?
[[ $status -eq 130 ]] || { echo "cancel returned $status, expected 130" >&2; cat "$dir/out" >&2; exit 1; }
grep -q 'mouse position pick cancelled' "$dir/out"

stage='no display at all'
status=0
env -u DISPLAY -u WAYLAND_DISPLAY -u XDG_SESSION_TYPE "$BIN" --get-mouse-pos >"$dir/out" 2>&1 || status=$?
[[ $status -eq 2 ]] || { echo "missing display returned $status, expected 2" >&2; cat "$dir/out" >&2; exit 1; }
grep -q 'Cannot open a display for the mouse position picker' "$dir/out"

echo 'ok   native Wayland picker and active-window queries without X11'
