#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fixture="$root/tests/keytable_fixture.h"
expected="$root/tests/keytable_expected.inc"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/loomwright-keytable-test.XXXXXXXX")"
trap 'rm -rf "$tmp"' EXIT

# The fixture exercises hex/decimal values, duplicate codes, duplicate names,
# extra whitespace, comments, and defines that must be ignored. Its golden
# output keeps the generator's byte-for-byte format under regression.
bash "$root/scripts/gen-keytable.sh" "$fixture" "$tmp/keys.inc" > "$tmp/log"
grep -q "wrote $tmp/keys.inc: 311 names, 9 codes" "$tmp/log"
cmp "$expected" "$tmp/keys.inc"

# A truncated header must be rejected before an incomplete table is written.
printf '#define KEY_A 1\n' > "$tmp/short.h"
if bash "$root/scripts/gen-keytable.sh" "$tmp/short.h" "$tmp/short.inc" > "$tmp/short.log" 2>&1; then
    printf 'expected the short header to be rejected\n' >&2
    exit 1
fi
grep -q 'only 1 KEY_ definitions found in' "$tmp/short.log"
[[ ! -e "$tmp/short.inc" ]]

echo 'ok   keytable generator reproduces the golden table from a synthetic header'
