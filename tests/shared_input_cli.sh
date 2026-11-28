#!/usr/bin/env bash
set -euo pipefail
bin=$1
dir=$(mktemp -d "${TMPDIR:-/tmp}/loomwright-input.XXXXXXXX")
export XDG_RUNTIME_DIR=$dir
first= second= stale=
cleanup() {
    local status=$?
    if (( status != 0 )); then cat "$dir/first.log" "$dir/second.log" >&2 2>/dev/null || true; fi
    for pid in "$first" "$second" "$stale"; do
        if [[ -n $pid ]]; then kill -TERM "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
    done
    # The detached broker can still be finishing its idle grace period; this
    # directory belongs solely to the test, never to a real desktop.
    rm -f "$dir/run.loom" "$dir/first.log" "$dir/second.log" "$dir/loomwright-input.sock" "$dir/loomwright-input.sock.lock"
    rmdir "$dir"
}
trap cleanup EXIT

"$bin" --internal-input-broker &
stale=$!
for ((i=0; i<100; i++)); do
    [[ ! -S $dir/loomwright-input.sock ]] || break
    sleep 0.01
done
[[ -S $dir/loomwright-input.sock ]]
kill -KILL "$stale"
wait "$stale" 2>/dev/null || true
stale=
[[ -S $dir/loomwright-input.sock ]]

cat >"$dir/run.loom" <<'EOF'
loom "shared startup" {
    when app starts { log "registered" }
}
EOF
# Explicitly use a missing test path so neither process opens the real
# keyboard. The default panic binding still causes broker registration.
"$bin" --run "$dir/run.loom" --input-device "$dir/no-input" --dry-run >"$dir/first.log" 2>&1 &
first=$!
"$bin" --run "$dir/run.loom" --input-device "$dir/no-input" --dry-run --input-mode broker >"$dir/second.log" 2>&1 &
second=$!
for ((i=0; i<200; i++)); do
    if grep -q registered "$dir/first.log" && grep -q registered "$dir/second.log"; then break; fi
    kill -0 "$first"
    kill -0 "$second"
    sleep 0.02
done
grep -q registered "$dir/first.log"
grep -q registered "$dir/second.log"
[[ -S $dir/loomwright-input.sock ]]
kill -TERM "$first" "$second"
wait "$first"
first=
wait "$second"
second=
for ((i=0; i<200; i++)); do
    [[ -S $dir/loomwright-input.sock ]] || break
    sleep 0.02
done
[[ ! -S $dir/loomwright-input.sock ]]
echo 'ok   simultaneous CLI startup recovered a stale socket; the broker exited after both scripts'
